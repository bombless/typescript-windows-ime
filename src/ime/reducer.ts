import type { KeyMessage } from "../protocol/messages.js";
import type { ImeState } from "./state.js";

export interface Reduction { state: ImeState; consume: boolean; commit?: string; }

export function reduceKey(state: ImeState, event: KeyMessage): Reduction {
  if (event.type !== "keyDown") return { state, consume: false };
  const key = event.key;
  if (/^[A-Za-z]$/.test(key)) return { state: { composition: state.composition + key.toLowerCase() }, consume: true };
  if (key === "Backspace") {
    if (!state.composition) return { state, consume: false };
    return { state: { composition: state.composition.slice(0, -1) }, consume: true };
  }
  if (key === "Escape") {
    if (!state.composition) return { state, consume: false };
    return { state: { composition: "" }, consume: true };
  }
  if (key === " " || key === "Spacebar" || key === "Enter") {
    if (!state.composition) return { state, consume: false };
    return { state: { composition: "" }, consume: true, commit: state.composition };
  }
  return { state, consume: false };
}