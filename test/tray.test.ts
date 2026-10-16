import { describe, expect, it } from "vitest";
import { SimpleEngine } from "../src/ime/engine.js";
import { initialImeState } from "../src/ime/state.js";

describe("client-owned tray presentation", () => {
  it("returns visibility and appearance policy with the authoritative input mode", () => {
    const engine = new SimpleEngine();
    const result = engine.processKey({
      id: 1,
      session: 7,
      type: "keyDown",
      vk: 65,
      scanCode: 30,
      key: "A",
      modifiers: 0,
    }, initialImeState());

    expect(result.response.tray).toEqual({
      visible: true,
      text: "中",
      tooltip: "TypeScript Windows IME — 中文模式",
      background: "#be2d2d",
      foreground: "#ffffff",
    });
  });
});