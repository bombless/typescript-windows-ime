import type { KeyMessage } from "../protocol/messages.js";
import { getCandidates } from "./candidates.js";
import type { ImeState } from "./state.js";

export interface Reduction { state: ImeState; consume: boolean; commit?: string; }

export function reduceKey(state: ImeState, event: KeyMessage): Reduction {
  if (event.type !== "keyDown") return { state, consume: false };
  const key = event.key;
  const mode = state.mode ?? "chinese";
  // A standalone Shift key-down toggles the input mode. TSF mirrors the
  // returned mode into its compartment; modifier combinations are not toggles.
  if (key === "Shift" && (event.modifiers & (2 | 4 | 8)) === 0) {
    return {
      state: { composition: "", selectedCandidate: 0, mode: mode === "chinese" ? "english" : "chinese" },
      consume: true,
    };
  }
  // English mode passes ordinary typing to the focused application unchanged.
  if (mode === "english") return { state, consume: false };
  // Modifier shortcuts belong to Windows/the focused application, not the
  // IME. Native modifier bits are Shift=1, Ctrl=2, Alt=4. Keep this policy in
  // TypeScript so testKeyDown and keyDown agree and shortcuts never mutate state.
  if ((event.modifiers & (2 | 4 | 8)) !== 0 || ((event.modifiers & 1) !== 0 && !/^[A-Za-z]$/.test(key))) {
    return { state, consume: false };
  }
  const candidates = getCandidates(state.composition);
  if (/^[A-Za-z]$/.test(key)) return { state: { composition: state.composition + key.toLowerCase(), selectedCandidate: 0 }, consume: true };
  if (key === "Backspace") {
    if (!state.composition) return { state, consume: false };
    return { state: { composition: state.composition.slice(0, -1), selectedCandidate: 0 }, consume: true };
  }
  if (key === "Delete") {
    if (!state.composition) return { state, consume: false };
    return { state: { composition: state.composition.slice(1), selectedCandidate: 0 }, consume: true };
  }
  if (key === "Escape") {
    if (!state.composition) return { state, consume: false };
    return { state: { composition: "", selectedCandidate: 0 }, consume: true };
  }
  if (["ArrowUp", "ArrowLeft", "ArrowDown", "ArrowRight"].includes(key)) {
    if (!state.composition || candidates.length === 0) return { state, consume: false };
    const delta = key === "ArrowUp" || key === "ArrowLeft" ? -1 : 1;
    const selectedCandidate = Math.max(0, Math.min(candidates.length - 1, state.selectedCandidate + delta));
    return { state: { ...state, selectedCandidate }, consume: true };
  }
  if (/^[1-9]$/.test(key) && state.composition) {
    const index = Number(key) - 1;
    if (index >= candidates.length) return { state, consume: false };
    return { state: { composition: "", selectedCandidate: 0 }, consume: true, commit: candidates[index]!.text };
  }
  if (key === " " || key === "Spacebar" || key === "Enter") {
    if (!state.composition) return { state, consume: false };
    return { state: { composition: "", selectedCandidate: 0 }, consume: true,
      commit: candidates[state.selectedCandidate]?.text ?? candidates[0]?.text ?? state.composition };
  }
  return { state, consume: false };
}