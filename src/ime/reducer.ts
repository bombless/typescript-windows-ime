import type { KeyMessage } from "../protocol/messages.js";
import { getCandidates } from "./candidates.js";
import type { ImeState } from "./state.js";

export interface Reduction { state: ImeState; consume: boolean; commit?: string; }

export function reduceKey(state: ImeState, event: KeyMessage): Reduction {
  if (event.type !== "keyDown") return { state, consume: false };
  const key = event.key;
  let mode = state.mode ?? "chinese";
  // A standalone Shift key-down toggles the input mode. TSF mirrors the
  // returned mode into its compartment; modifier combinations are not toggles.
  if (key === "Shift" && (event.modifiers & (2 | 4 | 8)) === 0) {
    return {
      state: { composition: "", selectedCandidate: 0, mode: mode === "chinese" ? "english" : "chinese", shiftToggleOrigin: mode },
      consume: true,
    };
  }
  // Shift is reported as a standalone key before the key it modifies. If
  // that next key is a shifted symbol (for example ':' from Shift+;), the
  // Shift press was not a mode-toggle tap: restore the original mode.
  if (state.shiftToggleOrigin) {
    const { shiftToggleOrigin, ...rest } = state;
    if ((event.modifiers & 1) !== 0 && !/^[A-Za-z]$/.test(key)) {
      state = { ...rest, mode: shiftToggleOrigin };
      mode = shiftToggleOrigin;
    } else {
      state = rest;
    }
  }
  // English mode passes ordinary typing to the focused application unchanged.
  if (mode === "english") return { state, consume: false };
  // Modifier shortcuts belong to Windows/the focused application, not the
  // IME. Native modifier bits are Shift=1, Ctrl=2, Alt=4. Keep this policy in
  // TypeScript so testKeyDown and keyDown agree and shortcuts never mutate state.
  if ((event.modifiers & (2 | 4 | 8)) !== 0 ||
      ((event.modifiers & 1) !== 0 && !/^[A-Za-z]$/.test(key) && event.vk !== 0xba)) {
    return { state, consume: false };
  }
  // Resolve punctuation from the virtual-key code in Node.js. The native TSF
  // layer forwards key events; it does not implement input-method behavior.
  const punctuationByVk: Record<number, string> = { 0xbc: "，", 0xbe: "。", 0xdc: "、", 0xe2: "、" };
  const punctuation = (event.vk === 0xba && (event.modifiers & 1) !== 0)
    ? "："
    : ((event.modifiers & 1) === 0 ? punctuationByVk[event.vk] : undefined);
  if (punctuation) {
    return { state: { composition: "", selectedCandidate: 0 }, consume: true, commit: punctuation };
  }
  const candidates = getCandidates(state.composition);
  if (/^[A-Za-z]$/.test(key)) return { state: { ...state, composition: state.composition + key.toLowerCase(), selectedCandidate: 0, ...(state.candidatePage !== undefined ? { candidatePage: 0 } : {}) }, consume: true };
  if (key === "Backspace") {
    if (!state.composition) return { state, consume: false };
    return { state: { ...state, composition: state.composition.slice(0, -1), selectedCandidate: 0, ...(state.candidatePage !== undefined ? { candidatePage: 0 } : {}) }, consume: true };
  }
  if (key === "Delete") {
    if (!state.composition) return { state, consume: false };
    return { state: { ...state, composition: state.composition.slice(1), selectedCandidate: 0, ...(state.candidatePage !== undefined ? { candidatePage: 0 } : {}) }, consume: true };
  }
  if (key === "Escape") {
    if (!state.composition) return { state, consume: false };
    return { state: { composition: "", selectedCandidate: 0 }, consume: true };
  }
  // Minus moves to the previous page; equals moves forward.
  if (["-", "="].includes(key)) {
    if (!state.composition || candidates.length <= 9) return { state, consume: false };
    const pageCount = Math.ceil(candidates.length / 9);
    const currentPage = state.candidatePage ?? 0;
    const delta = key === "-" ? -1 : 1;
    const candidatePage = Math.max(0, Math.min(pageCount - 1, currentPage + delta));
    if (candidatePage === currentPage) return { state, consume: false };
    return { state: { ...state, candidatePage, selectedCandidate: 0 }, consume: true };
  }
  if (["ArrowUp", "ArrowLeft", "ArrowDown", "ArrowRight"].includes(key)) {
    if (!state.composition || candidates.length === 0) return { state, consume: false };
    const delta = key === "ArrowUp" || key === "ArrowLeft" ? -1 : 1;
    const pageCandidates = candidates.slice((state.candidatePage ?? 0) * 9, (state.candidatePage ?? 0) * 9 + 9);
    const selectedCandidate = Math.max(0, Math.min(pageCandidates.length - 1, state.selectedCandidate + delta));
    return { state: { ...state, selectedCandidate }, consume: true };
  }
  if (/^[1-9]$/.test(key) && state.composition) {
    const index = (state.candidatePage ?? 0) * 9 + Number(key) - 1;
    if (index >= candidates.length) return { state, consume: false };
    return { state: { composition: "", selectedCandidate: 0 }, consume: true, commit: candidates[index]!.text };
  }
  if (key === " " || key === "Spacebar" || key === "Enter") {
    if (!state.composition) return { state, consume: false };
    return { state: { composition: "", selectedCandidate: 0 }, consume: true,
      commit: candidates[(state.candidatePage ?? 0) * 9 + state.selectedCandidate]?.text ?? candidates[(state.candidatePage ?? 0) * 9]?.text ?? state.composition };
  }
  return { state, consume: false };
}