import { MAX_MESSAGE_BYTES, type RequestMessage, type ResponseMessage, isRequestMessage } from "./messages.js";

export class ProtocolError extends Error {
  constructor(message: string, public readonly code: "INVALID_JSON" | "INVALID_MESSAGE" | "MESSAGE_TOO_LARGE") {
    super(message);
    this.name = "ProtocolError";
  }
}

const byteLength = (value: string) => Buffer.byteLength(value, "utf8");

export function encodeMessage(message: RequestMessage | ResponseMessage): string {
  const line = JSON.stringify(message);
  if (byteLength(line) > MAX_MESSAGE_BYTES) throw new ProtocolError("Message exceeds maximum size", "MESSAGE_TOO_LARGE");
  return line + "\n";
}

export function decodeRequestLine(line: string): RequestMessage {
  if (byteLength(line) > MAX_MESSAGE_BYTES) throw new ProtocolError("Message exceeds maximum size", "MESSAGE_TOO_LARGE");
  let value: unknown;
  try { value = JSON.parse(line); } catch { throw new ProtocolError("Malformed JSON", "INVALID_JSON"); }
  if (!isRequestMessage(value)) throw new ProtocolError("Invalid protocol message", "INVALID_MESSAGE");
  return value;
}

export class JsonlDecoder {
  private buffer = "";
  push(chunk: string | Buffer): RequestMessage[] {
    this.buffer += typeof chunk === "string" ? chunk : chunk.toString("utf8");
    if (byteLength(this.buffer) > MAX_MESSAGE_BYTES * 2) {
      throw new ProtocolError("Buffered data exceeds maximum size", "MESSAGE_TOO_LARGE");
    }
    const messages: RequestMessage[] = [];
    let newline = this.buffer.indexOf("\n");
    while (newline >= 0) {
      const line = this.buffer.slice(0, newline);
      this.buffer = this.buffer.slice(newline + 1);
      if (line.trim() !== "") messages.push(decodeRequestLine(line));
      newline = this.buffer.indexOf("\n");
    }
    return messages;
  }
}