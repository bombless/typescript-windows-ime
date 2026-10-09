export const PROTOCOL_VERSION = 3;
export const MAX_MESSAGE_BYTES = 16 * 1024;

// Every message carries the session that owns it. TSF activates one text
// service per application process and the Host multiplexes all of them over a
// single Node connection, so `id` alone is not unique: two apps both number
// their first request 1. The Host routes by `session`, and Node keeps one
// composition state per session so apps cannot corrupt each other.
export interface SessionScoped { session: number; }

export interface HelloMessage extends SessionScoped { id: number; type: "hello"; protocol: number; }
export interface QueryMessage extends SessionScoped { id: number; type: "query"; composition: string; }
export interface TestKeyMessage extends SessionScoped {
  id: number; type: "testKeyDown"; vk: number; scanCode: number; key: string; modifiers: number;
}
export interface KeyMessage extends SessionScoped {
  id: number; type: "keyDown" | "keyUp"; vk: number; scanCode: number; key: string; modifiers: number;
}
export interface ResetMessage extends SessionScoped { id: number; type: "reset"; }
export interface ShowCandidatesMessage extends SessionScoped {
  id: number; type: "showCandidates"; candidates: string[]; selection: number;
  caret: { left: number; top: number; right: number; bottom: number }; dpi: number;
}
export interface HideCandidatesMessage extends SessionScoped { id: number; type: "hideCandidates"; }
export type RequestMessage = HelloMessage | QueryMessage | TestKeyMessage | KeyMessage | ResetMessage | ShowCandidatesMessage | HideCandidatesMessage;
export interface Candidate { text: string; index: number; annotation?: string; }
export interface ResponseMessage extends SessionScoped {
  id: number; consume: boolean; composition?: string; commit?: string;
  candidates?: Candidate[]; selectedCandidate?: number; error?: { code: string; message: string };
}

export function isRequestMessage(value: unknown): value is RequestMessage {
  if (!value || typeof value !== "object") return false;
  const message = value as Record<string, unknown>;
  if (!Number.isSafeInteger(message.id) || (message.id as number) < 0) return false;
  // A missing session would silently merge two applications into one IME
  // state, so it is rejected rather than defaulted.
  if (!Number.isSafeInteger(message.session) || (message.session as number) < 0) return false;
  if (message.type === "hello") return Number.isSafeInteger(message.protocol);
  if (message.type === "query") return typeof message.composition === "string";
  if (message.type === "testKeyDown") {
    return Number.isInteger(message.vk) && Number.isInteger(message.scanCode)
      && typeof message.key === "string" && typeof message.modifiers === "number";
  }
  if (message.type === "reset" || message.type === "hideCandidates") return true;
  if (message.type === "showCandidates") {
    const caret = message.caret as Record<string, unknown> | undefined;
    return Array.isArray(message.candidates) && message.candidates.every((candidate) => typeof candidate === "string")
      && Number.isSafeInteger(message.selection) && (message.selection as number) >= 0
      && Number.isSafeInteger(message.dpi) && (message.dpi as number) > 0
      && !!caret && ["left", "top", "right", "bottom"].every((key) => Number.isFinite(caret[key]));
  }
  if (message.type === "keyDown" || message.type === "keyUp") {
    return Number.isInteger(message.vk) && Number.isInteger(message.scanCode)
      && typeof message.key === "string" && typeof message.modifiers === "number";
  }
  return false;
}