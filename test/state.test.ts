import { describe, expect, it } from "vitest";
import { initialImeState } from "../src/ime/state.js";
import { reduceKey } from "../src/ime/reducer.js";

describe("initial key behavior", () => {
  it("consumes alphabetic keyDown events", () => {
    expect(reduceKey(initialImeState(), { id: 1, type: "keyDown", vk: 65, scanCode: 30, key: "A", modifiers: 0 }))
      .toEqual({ state: { composition: "a" }, consume: true });
  });
  it("supports append, backspace, escape, and commit", () => {
    let state = initialImeState();
    state = reduceKey(state, { id: 1, type: "keyDown", vk: 65, scanCode: 30, key: "A", modifiers: 0 }).state;
    state = reduceKey(state, { id: 2, type: "keyDown", vk: 66, scanCode: 48, key: "B", modifiers: 0 }).state;
    expect(state).toEqual({ composition: "ab" });
    const backspace = reduceKey(state, { id: 3, type: "keyDown", vk: 8, scanCode: 14, key: "Backspace", modifiers: 0 });
    expect(backspace.state).toEqual({ composition: "a" });
    const commit = reduceKey(backspace.state, { id: 4, type: "keyDown", vk: 13, scanCode: 28, key: "Enter", modifiers: 0 });
    expect(commit).toEqual({ state: { composition: "" }, consume: true, commit: "a" });
    expect(reduceKey({ composition: "abc" }, { id: 5, type: "keyDown", vk: 27, scanCode: 1, key: "Escape", modifiers: 0 }))
      .toEqual({ state: { composition: "" }, consume: true });
  });
  it("does not consume unrelated keys or keyUp", () => {
    const state = { composition: "a" };
    expect(reduceKey(state, { id: 6, type: "keyDown", vk: 37, scanCode: 75, key: "ArrowLeft", modifiers: 0 }))
      .toEqual({ state, consume: false });
    expect(reduceKey(state, { id: 7, type: "keyUp", vk: 65, scanCode: 30, key: "A", modifiers: 0 }))
      .toEqual({ state, consume: false });
  });
});