import { describe, expect, it } from "vitest";
import {
  encodedGrammarLength,
  encodeGrammarText,
  OctagramGrammar,
} from "../src/ime/grammar.js";

function makeGramFixture(): Buffer {
  const arraySize = 256;
  const array = Buffer.alloc(arraySize * 4);
  const setUnit = (index: number, value: number) => array.writeUInt32LE(value >>> 0, index * 4);

  // Darts path "a" -> "b", with a leaf value of 150000 (15.0 after scaling).
  setUnit(0, 1 << 10);
  setUnit(96, (101 << 10) | 0x61);
  setUnit(103, (9 << 10) | 0x100 | 0x62);
  setUnit(110, 150000);

  const file = Buffer.alloc(44 + array.length);
  file.write("Rime::Grammar/1.0", 0, "ascii");
  file.writeUInt32LE(0, 32);
  file.writeUInt32LE(arraySize, 36);
  file.writeInt32LE(4, 40);
  array.copy(file, 44);
  return file;
}

describe("RIME Octagram grammar reader", () => {
  it("uses the upstream compact encoding for common Han characters", () => {
    expect([...encodeGrammarText("你")]).toEqual([0x8f, 0x60]);
    expect([...encodeGrammarText("一")]).toEqual([0xe1, 0x8e]);
    expect([...encodeGrammarText("𠀀")]).toEqual([0xe3, 0xc0, 0xc0, 0xc0]);
    expect(encodedGrammarLength(encodeGrammarText("你好"))).toBe(2);
  });

  it("reads a Rime::Grammar header and traverses a Darts double array", () => {
    const grammar = OctagramGrammar.fromBuffer(makeGramFixture(), {
      collocationMinLength: 2,
      collocationPenalty: -14,
    });
    expect(grammar.lookupEncoded(Buffer.from("a"), Buffer.from("b"))).toEqual([
      { value: 150000, length: 1 },
    ]);
    expect(grammar.query("a", "b")).toBe(1);
  });

  it("rejects unknown or truncated model files", () => {
    expect(() => OctagramGrammar.fromBuffer(Buffer.alloc(44))).toThrow(/format/i);
    const invalid = makeGramFixture();
    invalid.write("Not a grammar", 0, "ascii");
    expect(() => OctagramGrammar.fromBuffer(invalid)).toThrow(/format/i);
  });
});
