import type { KeyMessage } from "../protocol/messages.js";
import { getCandidates } from "./candidates.js";
import type { ImeState } from "./state.js";

export interface Reduction { state: ImeState; consume: boolean; commit?: string; }

export function reduceKey(state: ImeState, event: KeyMessage): Reduction {
  if (event.type !== "keyDown") return { state, consume: false };
  const key = event.key;
  const candidates = getCandidates(state.composition);
  if (/^[A-Za-z]$/.test(key)) return { state: { composition: state.composition + key.toLowerCase(), selectedCandidate: 0 }, consume: true };
  if (key === "Backspace") {
    if (!state.composition) return { state, consume: false };
    return { state: { composition: state.composition.slice(0, -1), selectedCandidate: 0 }, consume: true };
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