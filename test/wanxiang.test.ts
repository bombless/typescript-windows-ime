import { describe, expect, it } from "vitest";
import { getCandidates } from "../src/ime/candidates.js";

describe("Wanxiang dictionary integration", () => {
  it("loads tone-marked multi-syllable names as ordinary full pinyin", () => {
    expect(getCandidates("abao").map((candidate) => candidate.text)).toContain("阿宝");
    expect(getCandidates("abian").map((candidate) => candidate.text)).toContain("阿扁");
  });

  it("maps tone-marked ü to the v key", () => {
    expect(getCandidates("lv").map((candidate) => candidate.text)).toContain("绿");
  });
});