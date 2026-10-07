import { createServer, type Server, type Socket } from "node:net";
import { PROTOCOL_VERSION, type RequestMessage, type ResponseMessage } from "../protocol/messages.js";
import { encodeMessage, JsonlDecoder, ProtocolError } from "../protocol/codec.js";
import { SimpleEngine } from "../ime/engine.js";
import { getCandidates } from "../ime/candidates.js";
import { initialImeState, type ImeState } from "../ime/state.js";

export const PIPE_NAME = "\\\\.\\pipe\\TypeScriptWindowsIME";

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
        const result = handleMessage(message, engine, state);
        state = result.state;
        socket.write(encodeMessage(result.response));
      }
    } catch (error) {
      const protocolError = error instanceof ProtocolError ? error : new ProtocolError("Protocol failure", "INVALID_MESSAGE");
      socket.write(encodeMessage({ id: 0, consume: false, error: { code: protocolError.code, message: protocolError.message } }));
    }
  });
}

export function startPipeServer(pipeName = PIPE_NAME): Server {
  const server = createServer(handleConnection);
  server.listen(pipeName);
  return server;
}