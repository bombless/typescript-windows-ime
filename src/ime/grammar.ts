import { existsSync, readFileSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";

export interface OctagramConfig {
  collocationMaxLength: number;
  collocationMinLength: number;
  collocationPenalty: number;
  nonCollocationPenalty: number;
  weakCollocationPenalty: number;
  rearPenalty: number;
}

export interface GramMatch {
  value: number;
  length: number;
}

export const DEFAULT_OCTAGRAM_CONFIG: OctagramConfig = {
  collocationMaxLength: 6,
  collocationMinLength: 3,
  collocationPenalty: -14,
  nonCollocationPenalty: -6,
  weakCollocationPenalty: -100,
  rearPenalty: -20,
};

export function encodeGrammarText(text: string): Buffer {
  const output: number[] = [];
  for (const character of text) {
    let codePoint = character.codePointAt(0)! >>> 0;
    if (codePoint < 0x80) {
      output.push(codePoint === 0 ? 0xe0 : codePoint);
    } else if (codePoint >= 0x4000 && codePoint < 0xa000) {
      if ((codePoint & 0xff) === 0) {
        output.push(0xe1, (codePoint >>> 8) + 0x40);
      } else {
        output.push((codePoint >>> 8) + 0x40, codePoint & 0xff);
      }
    } else {
      let bits = 32;
      while (bits > 0 && (codePoint & 0xfe000000) === 0) {
        bits -= 7;
        codePoint = (codePoint << 7) >>> 0;
      }
      let bytesToEncode = Math.ceil(bits / 7);
      output.push(0xe0 | bytesToEncode);
      while (bytesToEncode > 0) {
        bytesToEncode -= 1;
        output.push(((codePoint >>> 25) & 0x7f) | 0x80);
      }
    }
  }
  return Buffer.from(output);
}

function nextEncodedCharacterOffset(bytes: Buffer, offset: number): number {
  const first = bytes[offset]!;
  if ((first & 0x80) === 0) return offset + 1;
  if ((first & 0xf0) === 0xe0) return offset + (first & 0x0f) + 1;
  return offset + 2;
}

export function encodedGrammarLength(bytes: Buffer): number {
  let count = 0;
  for (let offset = 0; offset < bytes.length; count += 1) {
    const next = nextEncodedCharacterOffset(bytes, offset);
    if (next <= offset || next > bytes.length) {
      throw new Error("Invalid Octagram encoded Unicode sequence.");
    }
    offset = next;
  }
  return count;
}

function unitOffset(unit: number): number {
  return ((unit >>> 10) << ((unit & 0x200) >>> 6)) >>> 0;
}

function unitLabel(unit: number): number {
  return (unit & 0x800000ff) >>> 0;
}

function unitHasLeaf(unit: number): boolean {
  return (unit & 0x100) !== 0;
}

function unitValue(unit: number): number {
  return unit & 0x7fffffff;
}

export class OctagramGrammar {
  private constructor(
    private readonly file: Buffer,
    private readonly arrayOffset: number,
    private readonly arraySize: number,
    private readonly config: OctagramConfig,
  ) {}

  static fromBuffer(file: Buffer, config: Partial<OctagramConfig> = {}): OctagramGrammar {
    const metadataSize = 44;
    if (file.length < metadataSize) throw new Error("Octagram model is shorter than its metadata header.");

    const format = file.subarray(0, 32).toString("ascii").split("\0", 1)[0] ?? "";
    if (!format.startsWith("Rime::Grammar/")) {
      throw new Error("Unsupported Octagram model format: " + (format || "(empty)"));
    }

    const arraySize = file.readUInt32LE(36);
    const relativeArrayOffset = file.readInt32LE(40);
    const arrayOffset = 40 + relativeArrayOffset;
    if (arrayOffset < metadataSize || arrayOffset % 4 !== 0 ||
        arraySize < 256 || arraySize > Math.floor((file.length - arrayOffset) / 4)) {
      throw new Error("Octagram model contains an invalid Darts double-array range.");
    }

    return new OctagramGrammar(file, arrayOffset, arraySize, {
      ...DEFAULT_OCTAGRAM_CONFIG,
      ...config,
    });
  }

  static load(path: string, config: Partial<OctagramConfig> = {}): OctagramGrammar {
    return OctagramGrammar.fromBuffer(readFileSync(path), config);
  }

  private readUnit(index: number): number {
    if (!Number.isInteger(index) || index < 0 || index >= this.arraySize) {
      throw new Error("Octagram Darts index is out of bounds: " + index);
    }
    return this.file.readUInt32LE(this.arrayOffset + index * 4);
  }

  private traverse(encodedContext: Buffer): number | undefined {
    let nodePosition = 0;
    let unit = this.readUnit(nodePosition);
    for (const byte of encodedContext) {
      const nextPosition = (nodePosition ^ unitOffset(unit) ^ byte) >>> 0;
      if (nextPosition >= this.arraySize) return undefined;
      unit = this.readUnit(nextPosition);
      if (unitLabel(unit) !== byte) return undefined;
      nodePosition = nextPosition;
    }
    return nodePosition;
  }

  private commonPrefixSearch(encodedWord: Buffer, startNode: number): GramMatch[] {
    let unit = this.readUnit(startNode);
    let nodePosition = (startNode ^ unitOffset(unit)) >>> 0;
    const results: GramMatch[] = [];

    for (let index = 0; index < encodedWord.length; index += 1) {
      nodePosition = (nodePosition ^ encodedWord[index]!) >>> 0;
      if (nodePosition >= this.arraySize) break;
      unit = this.readUnit(nodePosition);
      if (unitLabel(unit) !== encodedWord[index]) break;

      nodePosition = (nodePosition ^ unitOffset(unit)) >>> 0;
      if (unitHasLeaf(unit)) {
        results.push({
          value: unitValue(this.readUnit(nodePosition)),
          length: index + 1,
        });
        if (results.length === 8) break;
      }
    }
    return results;
  }

  lookupEncoded(context: Buffer, word: Buffer): GramMatch[] {
    const nodePosition = this.traverse(context);
    if (nodePosition === undefined) return [];
    return this.commonPrefixSearch(word, nodePosition);
  }

  query(context: string, word: string, isRear = false): number {
    if (!context || !word) return this.config.nonCollocationPenalty;

    const maxCharacters = Math.min(8, Math.max(0, this.config.collocationMaxLength - 1));
    if (maxCharacters === 0) return this.config.nonCollocationPenalty;

    const contextCharacters = Array.from(context).slice(-maxCharacters);
    const wordCharacters = Array.from(word);
    const wordQueryCharacters = wordCharacters.slice(0, maxCharacters);
    const wordQuery = encodeGrammarText(wordQueryCharacters.join(""));
    let result = this.config.nonCollocationPenalty;

    for (let start = 0; start < contextCharacters.length; start += 1) {
      const contextSuffix = encodeGrammarText(contextCharacters.slice(start).join(""));
      const contextLength = contextCharacters.length - start;
      const matches = this.lookupEncoded(contextSuffix, wordQuery);
      for (const match of matches) {
        const matchLength = encodedGrammarLength(wordQuery.subarray(0, match.length));
        const collocationLength = contextLength + matchLength;
        const wholeQueryMatch = start === 0 && match.length === wordQuery.length;
        const penalty = collocationLength >= this.config.collocationMinLength || wholeQueryMatch
          ? this.config.collocationPenalty
          : this.config.weakCollocationPenalty;
        result = Math.max(result, match.value / 10_000 + penalty);
      }
    }

    if (isRear && wordQueryCharacters.length === wordCharacters.length) {
      const rearMatches = this.lookupEncoded(wordQuery, Buffer.from("$", "ascii"));
      if (rearMatches.length > 0) {
        result = Math.max(result, rearMatches[0]!.value / 10_000 + this.config.rearPenalty);
      }
    }
    return result;
  }
}

let cachedGrammar: OctagramGrammar | null | undefined;
let grammarLoadPromise: Promise<void> | undefined;

export function preloadWanxiangGrammar(): Promise<void> {
  if (cachedGrammar !== undefined) return Promise.resolve();
  if (grammarLoadPromise) return grammarLoadPromise;
  const modelPath = process.env.WANXIANG_GRAMMAR_PATH ??
    fileURLToPath(new URL("../../data/wanxiang-lts-zh-hans.gram", import.meta.url));
  if (!existsSync(modelPath)) {
    cachedGrammar = null;
    return Promise.resolve();
  }
  grammarLoadPromise = readFile(modelPath).then((file) => {
    cachedGrammar = OctagramGrammar.fromBuffer(file);
    console.info("[ime-grammar] loaded Wanxiang Octagram model: " + modelPath);
  }).catch((error: unknown) => {
    cachedGrammar = null;
    console.error("[ime-grammar] failed to load Wanxiang Octagram model:", error);
  });
  return grammarLoadPromise;
}

function getDefaultGrammar(): OctagramGrammar | undefined {
  if (cachedGrammar === undefined) {
    void preloadWanxiangGrammar();
    return undefined;
  }
  return cachedGrammar ?? undefined;
}

export function getGrammarTransitionScore(context: string, word: string, isRear: boolean): number {
  if (!context || !word) return 0;
  return getDefaultGrammar()?.query(context, word, isRear) ?? 0;
}
