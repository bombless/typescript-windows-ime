import { describe, expect, it } from "vitest";
import { decodeRequestLine, encodeMessage, JsonlDecoder, ProtocolError } from "../src/protocol/codec.js";

describe("JSONL protocol", () => {
  it("round-trips a key event", () => {
    const message = { id: 2, type: "keyDown" as const, vk: 65, scanCode: 30, key: "A", modifiers: 0 };
    expect(decodeRequestLine(encodeMessage(message).trim())).toEqual(message);
  });
  it("round-trips a query with Unicode and empty composition", () => {
    const unicode = { id: 3, type: "query" as const, composition: "你好" };
    expect(decodeRequestLine(encodeMessage(unicode).trim())).toEqual(unicode);
    const empty = { id: 4, type: "query" as const, composition: "" };
    expect(decodeRequestLine(encodeMessage(empty).trim())).toEqual(empty);
  });
  it("handles fragmented and batched input", () => {
    const decoder = new JsonlDecoder();
    expect(decoder.push('{"id":1,"type":"reset"}\n{"id":2,"type":"hello","pro')).toHaveLength(1);
    expect(decoder.push('tocol":1}\n')).toHaveLength(1);
  });
  it("rejects malformed JSON", () => {
    expect(() => decodeRequestLine("{bad")).toThrowError(ProtocolError);
  });
  it("rejects unknown message types", () => {
    expect(() => decodeRequestLine('{"id":1,"type":"wat"}')).toThrowError(ProtocolError);
  });
  it("rejects a query without a string composition", () => {
    expect(() => decodeRequestLine('{"id":1,"type":"query","composition":123}')).toThrowError(ProtocolError);
  });
});