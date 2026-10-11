import { describe, expect, it } from "vitest";
import { initialImeState } from "../src/ime/state.js";
import { reduceKey } from "../src/ime/reducer.js";
import { getCandidates } from "../src/ime/candidates.js";

describe("initial key behavior", () => {
  it.each([
    [0xbc, ",", "，"],
    [0xbe, ".", "。"],
    [0xdc, "\\", "、"],
    [0xe2, "\\", "、"],
  ])("commits Chinese punctuation for VK %s", (vk, key, expected) => {
    expect(reduceKey(initialImeState(), {
      id: 900, session: 1, type: "keyDown", vk, scanCode: 0, key, modifiers: 0,
    })).toEqual({
      state: { composition: "", selectedCandidate: 0 },
      consume: true,
      commit: expected,
    });
  });

  it("passes punctuation through in English mode", () => {
    const state = { composition: "", selectedCandidate: 0, mode: "english" as const };
    expect(reduceKey(state, {
      id: 901, session: 1, type: "keyDown", vk: 0xbc, scanCode: 0, key: ",", modifiers: 0,
    })).toEqual({ state, consume: false });
  });

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
  it("handles Delete by removing the character at the start of the pinyin composition", () => {
    const key = (state: { composition: string; selectedCandidate: number }, id: number, key: string) =>
      reduceKey(state, { id, session: 1, type: "keyDown", vk: key === "Delete" ? 0x2e : key.toUpperCase().charCodeAt(0), scanCode: 0, key, modifiers: 0 });

    const w = key(initialImeState(), 1, "w");
    const cleared = key(w.state, 2, "Delete");
    expect(cleared).toEqual({ state: { composition: "", selectedCandidate: 0 }, consume: true });
    expect(key(cleared.state, 3, "o").state.composition).toBe("o");

    const wo = key(key(initialImeState(), 4, "w").state, 5, "o");
    expect(wo.state.composition).toBe("wo");
    expect(key(wo.state, 6, "Delete").state.composition).toBe("o");
  });

  it("regresses mixed character entry, Delete, and Backspace sequences", () => {
    const key = (state: { composition: string; selectedCandidate: number }, id: number, name: string) =>
      reduceKey(state, {
        id, session: 1, type: "keyDown",
        vk: name === "Backspace" ? 0x08 : name === "Delete" ? 0x2e : name.toUpperCase().charCodeAt(0),
        scanCode: 0, key: name, modifiers: 0,
      });

    const run = (sequence: string[], expected: string) => {
      let state = initialImeState();
      let id = 1;
      for (const name of sequence) {
        const result = key(state, id++, name);
        expect(result.consume, `key ${name} in sequence ${sequence.join(" ")}`).toBe(true);
        state = result.state;
      }
      expect(state.composition, sequence.join(" ")).toBe(expected);
    };

    // <a><delete><b> => b
    run(["a", "Delete", "b"], "b");
    // <a><b><delete> => b
    run(["a", "b", "Delete"], "b");
    // <a><b><backspace> => a
    run(["a", "b", "Backspace"], "a");
  });

  it("does not consume Backspace or Delete when the composition is empty", () => {
    const empty = initialImeState();
    for (const [id, name, vk] of [[40, "Backspace", 0x08], [41, "Delete", 0x2e]] as const) {
      expect(reduceKey(empty, {
        id, session: 1, type: "keyDown", vk, scanCode: 0, key: name, modifiers: 0,
      })).toEqual({ state: empty, consume: false });
    }
  });

  it("tests consumption without mutating the core state", async () => {
    const { SimpleEngine } = await import("../src/ime/engine.js");
    const engine = new SimpleEngine();
    const state = { composition: "", selectedCandidate: 0 };
    expect(engine.testKey({ id: 20, session: 1, type: "keyDown", vk: 65, scanCode: 30, key: "A", modifiers: 0 }, state))
      .toEqual({ consume: true });
    expect(state).toEqual({ composition: "", selectedCandidate: 0 });
  });

  it("toggles Chinese/English with a standalone Shift press and passes English typing through", async () => {
    const { SimpleEngine } = await import("../src/ime/engine.js");
    const engine = new SimpleEngine();
    const toggle = (state: ReturnType<typeof initialImeState>, id: number) => engine.processKey({
      id, session: 1, type: "keyDown", vk: 0x10, scanCode: 0x2a, key: "Shift", modifiers: 1,
    }, state);

    const toEnglish = toggle(initialImeState(), 200);
    expect(toEnglish.response.consume).toBe(true);
    expect(toEnglish.response.mode).toBe("english");
    expect(toEnglish.state).toEqual({ composition: "", selectedCandidate: 0, mode: "english" });
    expect(engine.testKey({ id: 201, session: 1, type: "testKeyDown", vk: 0x10, scanCode: 0x2a, key: "Shift", modifiers: 1 }, toEnglish.state))
      .toEqual({ consume: true });

    const letter = engine.processKey({ id: 202, session: 1, type: "keyDown", vk: 65, scanCode: 30, key: "A", modifiers: 0 }, toEnglish.state);
    expect(letter.response.consume).toBe(false);
    expect(letter.state).toEqual(toEnglish.state);

    const toChinese = toggle(toEnglish.state, 203);
    expect(toChinese.response.consume).toBe(true);
    expect(toChinese.response.mode).toBe("chinese");
    expect(engine.processKey({ id: 204, session: 1, type: "keyDown", vk: 65, scanCode: 30, key: "A", modifiers: 0 }, toChinese.state).state.composition)
      .toBe("a");
  });

  it("does not consume unrelated keys or keyUp", () => {
    const state = { composition: "a", selectedCandidate: 0 };
    expect(reduceKey(state, { id: 6, session: 1, type: "keyDown", vk: 112, scanCode: 59, key: "F1", modifiers: 0 }))
      .toEqual({ state, consume: false });
    expect(reduceKey(state, { id: 7, session: 1, type: "keyUp", vk: 65, scanCode: 30, key: "A", modifiers: 0 }))
      .toEqual({ state, consume: false });
  });
  it("passes Ctrl/Alt shortcuts and Ctrl+Space through without changing composition", async () => {
    const { SimpleEngine } = await import("../src/ime/engine.js");
    const engine = new SimpleEngine();
    const state = { composition: "ni", selectedCandidate: 1 };
    const shortcuts = [
      { id: 70, vk: 0x56, key: "V", modifiers: 2 }, // Ctrl+V
      { id: 71, vk: 0x41, key: "A", modifiers: 2 }, // Ctrl+A
      { id: 72, vk: 0x20, key: " ", modifiers: 2 }, // Ctrl+Space
      { id: 73, vk: 0x20, key: " ", modifiers: 1 }, // Shift+Space is not the toggle
      { id: 74, vk: 0x09, key: "Tab", modifiers: 4 }, // Alt+Tab
      { id: 75, vk: 0x20, key: " ", modifiers: 8 }, // Win+Space
    ];

    for (const shortcut of shortcuts) {
      const event = { ...shortcut, session: 1, scanCode: 0, type: "keyDown" as const };
      expect(engine.testKey({ ...event, type: "testKeyDown" }, state), shortcut.key).toEqual({ consume: false });
      const result = engine.processKey(event, state);
      expect(result.response.consume, shortcut.key).toBe(false);
      expect(result.state, shortcut.key).toEqual(state);
    }
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
  it("pages candidates with minus/equals and returns the active page", async () => {
    const { SimpleEngine } = await import("../src/ime/engine.js");
    const engine = new SimpleEngine();
    const all = getCandidates("ni");
    if (all.length <= 9) return;
    const event = (id: number, key: string, vk: number) => ({
      id, session: 1, type: "keyDown" as const, vk, scanCode: 0, key, modifiers: 0,
    });
    const initial = { composition: "ni", selectedCandidate: 0 };
    const next = engine.processKey(event(300, "=", 0xbb), initial);
    expect(next.response.consume).toBe(true);
    expect(next.state.candidatePage).toBe(1);
    expect(next.response.candidates?.map((candidate) => candidate.text))
      .toEqual(all.slice(9, 18).map((candidate) => candidate.text));

    const previous = engine.processKey(event(301, "-", 0xbd), next.state);
    expect(previous.response.consume).toBe(true);
    expect(previous.state.candidatePage).toBe(0);
    expect(previous.response.candidates?.map((candidate) => candidate.text))
      .toEqual(all.slice(0, 9).map((candidate) => candidate.text));
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
