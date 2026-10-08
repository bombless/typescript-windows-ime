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

export function startPipeClient(pipeName = PIPE_NAME): { close(): void } {
  let socket: Socket | undefined;
  let stopped = false;

  const connect = () => {
    if (stopped || socket) return;
    const next = createConnection(pipeName);
    socket = next;
    handleConnection(next);
    next.once("connect", () => console.log(`[PIPE] connected to C++: ${pipeName}`));
    next.once("error", (error) => {
      const code = (error as NodeJS.ErrnoException).code;
      console.log(`[PIPE] connection error: ${code ?? error.message}`);
    });
    next.once("close", () => {
      if (socket === next) socket = undefined;
      // The C++ server intentionally lets the newest client win. If another
      // client connects, this socket may be kicked; do not reconnect here or
      // Node would immediately steal the pipe back from the new client.
      if (!stopped) console.log("[PIPE] not reconnecting after disconnect; yielding to the newer client");
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
