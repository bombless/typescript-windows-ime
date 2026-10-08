import { createConnection, type Socket } from "node:net";
import { PROTOCOL_VERSION, type RequestMessage, type ResponseMessage } from "../protocol/messages.js";
import { encodeMessage, JsonlDecoder, ProtocolError } from "../protocol/codec.js";
import { SimpleEngine } from "../ime/engine.js";
import { getCandidates } from "../ime/candidates.js";
import { initialImeState, type ImeState } from "../ime/state.js";
import { appendFileSync } from "node:fs";

export const PIPE_NAME = "\\\\.\\pipe\\TypeScriptWindowsIME.Host";

function logPipe(direction: "← C++" | "→ C++", message: unknown): void {
  const line = `[PIPE ${direction}] ${JSON.stringify(message)}`;
  console.log(line);
  try { appendFileSync("ts-pipe.log", line + "\n"); } catch {}
}

function handleMessage(message: RequestMessage, engine: SimpleEngine, state: ImeState): { response: ResponseMessage; state: ImeState } {
  switch (message.type) {
    case "hello":
      return message.protocol === PROTOCOL_VERSION
        ? { response: { id: message.id, consume: false }, state }
        : { response: { id: message.id, consume: false, error: {
            code: "UNSUPPORTED_PROTOCOL",
            message: `Unsupported protocol version: ${message.protocol}`,
          } }, state };
    case "query":
      return { response: {
        id: message.id,
        consume: message.composition.length > 0,
        composition: message.composition,
        candidates: getCandidates(message.composition),
      }, state };
    case "testKeyDown":
      return { response: { id: message.id, consume: engine.testKey(message, state).consume }, state };
    case "reset": return { response: { id: message.id, consume: false, composition: "" }, state: initialImeState() };
    case "keyDown":
    case "keyUp":
      return engine.processKey(message, state);
  }
}

function handleConnection(socket: Socket): void {
  const decoder = new JsonlDecoder();
  const engine = new SimpleEngine();
  let state = initialImeState();
  socket.setNoDelay(true);
  socket.on("data", (chunk) => {
    try {
      for (const message of decoder.push(chunk)) {
        logPipe("← C++", message);
        const result = handleMessage(message, engine, state);
        state = result.state;
        const encoded = encodeMessage(result.response);
        logPipe("→ C++", result.response);
        socket.write(encoded);
      }
    } catch (error) {
      const protocolError = error instanceof ProtocolError ? error : new ProtocolError("Protocol failure", "INVALID_MESSAGE");
      const response = { id: 0, consume: false, error: { code: protocolError.code, message: protocolError.message } };
      logPipe("→ C++", response);
      socket.write(encodeMessage(response));
    }
  });

  socket.on("close", () => console.log("[PIPE] disconnected from C++"));
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
        console.error(`[PIPE] socket error: ${code ?? error.message}`);
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
