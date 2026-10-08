import { createServer, type Socket } from "node:net";
import { PROTOCOL_VERSION, type RequestMessage, type ResponseMessage } from "../protocol/messages.js";
import { encodeMessage, JsonlDecoder, ProtocolError } from "../protocol/codec.js";
import { SimpleEngine } from "../ime/engine.js";
import { getCandidates } from "../ime/candidates.js";
import { initialImeState, type ImeState } from "../ime/state.js";
import { appendFileSync } from "node:fs";

export const PIPE_NAME = "\\\\.\\pipe\\TypeScriptWindowsIME.Tsf";

const engine = new SimpleEngine();
let state = initialImeState();

function logPipe(direction: "← C++" | "→ C++", message: unknown): void {
  const line = `[PIPE ${direction}] ${JSON.stringify(message)}`;
  console.log(line);
  try { appendFileSync("ts-pipe.log", line + "\n"); } catch {}
}

function logCandidates(response: ResponseMessage): void {
  const candidates = response.candidates ?? [];
  const selected = response.selectedCandidate ?? 0;
  const texts = candidates.map((candidate, index) => `${index}=${candidate.text}`).join(" | ");
  const line = `[CANDIDATES] count=${candidates.length} selected=${selected} composition=${JSON.stringify(response.composition ?? "")} commit=${JSON.stringify(response.commit ?? "")} :: ${texts}`;
  console.log(line);
  try { appendFileSync("ts-pipe.log", line + "\n"); } catch {}
}

function handleMessage(message: RequestMessage): ResponseMessage {
  switch (message.type) {
    case "hello":
      return message.protocol === PROTOCOL_VERSION
        ? { id: message.id, consume: false }
        : { id: message.id, consume: false, error: {
            code: "UNSUPPORTED_PROTOCOL",
            message: `Unsupported protocol version: ${message.protocol}`,
          } };
    case "query":
      return {
        id: message.id,
        consume: message.composition.length > 0,
        composition: message.composition,
        candidates: getCandidates(message.composition),
      };
    case "testKeyDown":
      return { id: message.id, consume: engine.testKey(message, state).consume };
    case "reset": state = initialImeState(); return { id: message.id, consume: false, composition: "" };
    case "keyDown":
    case "keyUp": {
      const result = engine.processKey(message, state);
      state = result.state;
      return result.response;
    }
  }
}

function handleConnection(socket: Socket): void {
  const decoder = new JsonlDecoder();
  socket.setNoDelay(true);
  socket.on("data", (chunk) => {
    try {
      for (const message of decoder.push(chunk)) {
        logPipe("← C++", message);
        const response = handleMessage(message);
        logPipe("→ C++", response);
        logCandidates(response);
        socket.write(encodeMessage(response));
      }
    } catch (error) {
      const protocolError = error instanceof ProtocolError ? error : new ProtocolError("Protocol failure", "INVALID_MESSAGE");
      const response = { id: 0, consume: false, error: { code: protocolError.code, message: protocolError.message } };
      logPipe("→ C++", response);
      socket.write(encodeMessage(response));
    }
  });
  socket.on("close", () => console.log("[PIPE] TSF client disconnected"));
}

export function startPipeServer(pipeName = PIPE_NAME): { close(): Promise<void> } {
  const server = createServer((socket) => handleConnection(socket));
  server.on("error", (error) => console.log(`[PIPE] server error: ${error.message}`));
  server.listen(pipeName, () => console.log(`[PIPE] TS listening on ${pipeName}`));
  return {
    close() {
      return new Promise((resolve) => server.close(() => resolve()));
    },
  };
}
