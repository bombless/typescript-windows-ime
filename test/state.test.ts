import { describe, expect, it } from "vitest";
import { initialImeState } from "../src/ime/state.js";
import { reduceKey } from "../src/ime/reducer.js";

describe("initial key behavior", () => {
  it("consumes alphabetic keyDown events", () => {
    expect(reduceKey(initialImeState(), { id: 1, session: 1, type: "keyDown", vk: 65, scanCode: 30, key: "A", modifiers: 0 }))
      .toEqual({ state: { composition: "a", selectedCandidate: 0 }, consume: true });
  });
  it("supports append, backspace, escape, and commit", () => {
    let state = initialImeState();
    state = reduceKey(state, { id: 1, session: 1, type: "keyDown", vk: 65, scanCode: 30, key: "A", modifiers: 0 }).state;
    state = reduceKey(state, { id: 2, session: 1, type: "keyDown", vk: 66, scanCode: 48, key: "B", modifiers: 0 }).state;
    expect(state).toEqual({ composition: "ab", selectedCandidate: 0 });
    const backspace = reduceKey(state, { id: 3, session: 1, type: "keyDown", vk: 8, scanCode: 14, key: "Backspace", modifiers: 0 });
    expect(backspace.state).toEqual({ composition: "a", selectedCandidate: 0 });
    const commit = reduceKey(backspace.state, { id: 4, session: 1, type: "keyDown", vk: 13, scanCode: 28, key: "Enter", modifiers: 0 });
    expect(commit).toEqual({ state: { composition: "", selectedCandidate: 0 }, consume: true, commit: "啊" });
    expect(reduceKey({ composition: "abc", selectedCandidate: 0 }, { id: 5, session: 1, type: "keyDown", vk: 27, scanCode: 1, key: "Escape", modifiers: 0 }))
      .toEqual({ state: { composition: "", selectedCandidate: 0 }, consume: true });
  });
  it("tests consumption without mutating the core state", async () => {
    const { SimpleEngine } = await import("../src/ime/engine.js");
    const engine = new SimpleEngine();
    const state = { composition: "", selectedCandidate: 0 };
    expect(engine.testKey({ id: 20, session: 1, type: "keyDown", vk: 65, scanCode: 30, key: "A", modifiers: 0 }, state))
      .toEqual({ consume: true });
    expect(state).toEqual({ composition: "", selectedCandidate: 0 });
  });

  it("does not consume unrelated keys or keyUp", () => {
    const state = { composition: "a", selectedCandidate: 0 };
    expect(reduceKey(state, { id: 6, session: 1, type: "keyDown", vk: 112, scanCode: 59, key: "F1", modifiers: 0 }))
      .toEqual({ state, consume: false });
    expect(reduceKey(state, { id: 7, session: 1, type: "keyUp", vk: 65, scanCode: 30, key: "A", modifiers: 0 }))
      .toEqual({ state, consume: false });
  });
  it("commits the top Rime candidate instead of raw pinyin", async () => {
    const { SimpleEngine } = await import("../src/ime/engine.js");
    const engine = new SimpleEngine();
    let state = initialImeState();
    for (const [id, key] of [[8, "n"], [9, "i"], [10, "h"], [11, "a"], [12, "o"]] as const) {
      state = engine.processKey({ id, session: 1, type: "keyDown", vk: key.charCodeAt(0), scanCode: 0, key, modifiers: 0 }, state).state;
    }
    const result = engine.processKey({ id: 13, session: 1, type: "keyDown", vk: 32, scanCode: 0, key: " ", modifiers: 0 }, state);
    expect(result.response.candidates?.[0]?.text).toBe("你好");
    expect(result.response.commit).toBe("你好");
    expect(result.state).toEqual({ composition: "", selectedCandidate: 0 });
  });
  it("moves the candidate highlight with arrow keys and commits the chosen number", async () => {
    const { SimpleEngine } = await import("../src/ime/engine.js");
    const engine = new SimpleEngine();
    let state = initialImeState();
    for (const [id, key] of [[30, "n"], [31, "i"]] as const) {
      state = engine.processKey({ id, session: 1, type: "keyDown", vk: key.charCodeAt(0), scanCode: 0, key, modifiers: 0 }, state).state;
    }
    const candidates = engine.processKey({ id: 32, session: 1, type: "keyDown", vk: 40, scanCode: 0, key: "ArrowDown", modifiers: 0 }, state);
    expect(candidates.state.selectedCandidate).toBe(1);
    expect(candidates.response.selectedCandidate).toBe(1);
    const selected = candidates.response.candidates?.[1]?.text;
    const committed = engine.processKey({ id: 33, session: 1, type: "keyDown", vk: 50, scanCode: 0, key: "2", modifiers: 0 }, candidates.state);
    expect(committed.response.commit).toBe(selected);
    expect(committed.state).toEqual({ composition: "", selectedCandidate: 0 });
  });
  it("keeps concurrent sessions from corrupting each other's composition", async () => {
    const { SimpleEngine } = await import("../src/ime/engine.js");
    const engine = new SimpleEngine();
    const sessions = new Map([
      [1, initialImeState()],
      [2, initialImeState()],
    ]);
    const type = (session: number, key: string) => {
      const result = engine.processKey(
        { id: 1, session, type: "keyDown", vk: key.charCodeAt(0), scanCode: 0, key, modifiers: 0 },
        sessions.get(session)!,
      );
      sessions.set(session, result.state);
      return result.response.consume;
    };

    expect(type(1, "n")).toBe(true);
    expect(type(2, "h")).toBe(true);
    // Interleaved typing must not append across sessions.
    expect(type(1, "i")).toBe(true);
    expect(sessions.get(1)!.composition).toBe("ni");
    expect(sessions.get(2)!.composition).toBe("h");
  });
});
