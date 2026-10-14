import { createConnection, type Socket } from "node:net";
import { PROTOCOL_VERSION, type RequestMessage, type ResponseMessage } from "../protocol/messages.js";
import { encodeMessage, JsonlDecoder, ProtocolError } from "../protocol/codec.js";
import { SimpleEngine } from "../ime/engine.js";
import { getCandidates } from "../ime/candidates.js";
import { initialImeState, type ImeState } from "../ime/state.js";
import { appendFileSync } from "node:fs";
import { runActivationScripts } from "../automation.js";

export const PIPE_NAME = "\\\\.\\pipe\\TypeScriptWindowsIME.Host";

function timestamp(): string {
  const now = new Date();
  const pad = (n: number, width = 2) => String(n).padStart(width, "0");
  return `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())} ${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}.${pad(now.getMilliseconds(), 3)}`;
}

function log(message: string, level: "log" | "error" = "log"): void {
  const line = `[${timestamp()}][pid=${process.pid}] ${message}`;
  (level === "error" ? console.error : console.log)(line);
  try { appendFileSync("ts-pipe.log", line + "\n"); } catch {}
}

function logPipe(direction: "← C++" | "→ C++", message: unknown): void {
  log(`[PIPE ${direction}] ${JSON.stringify(message)}`);
}

function handleMessage(
  message: RequestMessage,
  engine: SimpleEngine,
  state: ImeState,
): { response: ResponseMessage; state: ImeState } {
  const session = message.session;
  switch (message.type) {
    case "hello":
      return message.protocol === PROTOCOL_VERSION
        ? { response: { id: message.id, session, consume: false }, state }
        : { response: { id: message.id, session, consume: false, error: {
            code: "UNSUPPORTED_PROTOCOL",
            message: `Unsupported protocol version: ${message.protocol}`,
          } }, state };
    case "query":
      return { response: {
        id: message.id,
        session,
        consume: message.composition.length > 0,
        composition: message.composition,
        candidates: getCandidates(message.composition),
        selectedCandidate: 0,
      }, state };
    case "testKeyDown":
      return { response: { id: message.id, session, consume: engine.testKey(message, state).consume }, state };
    case "reset": {
      const resetState: ImeState = { composition: "", selectedCandidate: 0, ...(state.mode ? { mode: state.mode } : {}) };
      return { response: { id: message.id, session, consume: false, composition: "", mode: resetState.mode ?? "chinese" }, state: resetState };
    }
    case "showCandidates":
    case "hideCandidates":
      return { response: { id: message.id, session, consume: false }, state };
    case "keyDown":
    case "keyUp":
      return engine.processKey(message, state);
  }
}

function handleConnection(socket: Socket): void {
  const decoder = new JsonlDecoder();
  const engine = new SimpleEngine();
  // TSF activates one text service per application process and the Host
  // multiplexes them over this single connection, so each session needs its
  // own composition. A shared state would let two apps interleave pinyin.
  const sessions = new Map<number, ImeState>();
  socket.setNoDelay(true);
  socket.on("data", (chunk) => {
    let lastSession: number | undefined;
    try {
      for (const message of decoder.push(chunk)) {
        logPipe("← C++", message);
        if ((message.type === "testKeyDown" || message.type === "keyDown") && message.key === "Shift" && (message.modifiers & (2 | 4 | 8)) === 0) {
          log(`[KEY] received Shift toggle phase=${message.type} session=${message.session} modifiers=${message.modifiers}`);
        }
        lastSession = message.session;
        const isNewSession = !sessions.has(message.session);
        if (message.type === "hello" || isNewSession) {
          runActivationScripts({ type: "activate", session: message.session, pid: process.pid, timestamp: new Date().toISOString() });
        }
        const state = sessions.get(message.session) ?? initialImeState();
        if (message.type === "testKeyDown" || message.type === "keyDown" || message.type === "keyUp") {
          log(`[IME BEFORE] type=${message.type} session=${message.session} key=${JSON.stringify(message.key)} vk=0x${message.vk.toString(16)} modifiers=${message.modifiers} mode=${state.mode ?? "chinese"} composition=${JSON.stringify(state.composition)} pendingShift=${state.shiftToggleOrigin ?? "none"}`);
        }
        const result = handleMessage(message, engine, state);
        sessions.set(message.session, result.state);
        if (message.type === "testKeyDown" || message.type === "keyDown" || message.type === "keyUp") {
          log(`[IME AFTER] type=${message.type} session=${message.session} key=${JSON.stringify(message.key)} consume=${result.response.consume} mode=${result.state.mode ?? "chinese"} composition=${JSON.stringify(result.state.composition)} pendingShift=${result.state.shiftToggleOrigin ?? "none"} responseMode=${result.response.mode ?? "omitted"}`);
        }
        const encoded = encodeMessage(result.response);
        logPipe("→ C++", result.response);
        socket.write(encoded);
      }
    } catch (error) {
      const protocolError = error instanceof ProtocolError ? error : new ProtocolError("Protocol failure", "INVALID_MESSAGE");
      const response = {
        id: 0,
        session: lastSession ?? 0,
        consume: false,
        error: { code: protocolError.code, message: protocolError.message },
      };
      logPipe("→ C++", response);
      socket.write(encodeMessage(response));
    }
  });

  socket.on("close", () => log(`[PIPE] disconnected from C++ sessions=${sessions.size}`));
}

export interface PipeClient {
  close(): void;
}

export interface PipeClientCallbacks {
  onConnected?: () => void;
  onUnavailable?: (error: Error) => void;
  onDisconnected?: () => void;
}

export function startPipeClient(pipeName = PIPE_NAME, callbacks: PipeClientCallbacks = {}): PipeClient {
  let socket: Socket | undefined;
  let stopped = false;
  let connected = false;

  const connect = () => {
    if (stopped || socket) return;
    const next = createConnection(pipeName);
    socket = next;
    handleConnection(next);
    next.once("connect", () => {
      connected = true;
      callbacks.onConnected?.();
    });
    next.once("error", (error) => {
      const code = (error as NodeJS.ErrnoException).code;
      if (!connected) {
        const detail = code ? `${code}: ${error.message}` : error.message;
        callbacks.onUnavailable?.(new Error(detail, { cause: error }));
      } else {
        log(`[PIPE] socket error: ${code ?? error.message}`, "error");
      }
    });
    next.once("close", () => {
      if (socket === next) socket = undefined;
      // Host owns replacement. A displaced client must exit, never reconnect
      // and steal the pipe back from the newer process.
      if (!stopped && connected) callbacks.onDisconnected?.();
      else if (!stopped && !connected) callbacks.onUnavailable?.(new Error(`Could not connect to Host pipe ${pipeName}`));
    });
  };

  connect();
  return {
    close() {
      stopped = true;
      socket?.destroy();
      socket = undefined;
    },
  };
}
