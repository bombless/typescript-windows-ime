export const PROTOCOL_VERSION = 1;
export const MAX_MESSAGE_BYTES = 16 * 1024;

export interface HelloMessage { id: number; type: "hello"; protocol: number; }
export interface QueryMessage { id: number; type: "query"; composition: string; }
export interface TestKeyMessage {
  id: number; type: "testKeyDown"; vk: number; scanCode: number; key: string; modifiers: number;
}
export interface KeyMessage {
  id: number; type: "keyDown" | "keyUp"; vk: number; scanCode: number; key: string; modifiers: number;
}
export interface ResetMessage { id: number; type: "reset"; }
export type RequestMessage = HelloMessage | QueryMessage | TestKeyMessage | KeyMessage | ResetMessage;
export interface Candidate { text: string; index: number; annotation?: string; }
export interface ResponseMessage {
  id: number; consume: boolean; composition?: string; commit?: string;
  candidates?: Candidate[]; selectedCandidate?: number; error?: { code: string; message: string };
}

export function isRequestMessage(value: unknown): value is RequestMessage {
  if (!value || typeof value !== "object") return false;
  const message = value as Record<string, unknown>;
  if (!Number.isSafeInteger(message.id) || (message.id as number) < 0) return false;
  if (message.type === "hello") return Number.isSafeInteger(message.protocol);
  if (message.type === "query") return typeof message.composition === "string";
  if (message.type === "testKeyDown") {
    return Number.isInteger(message.vk) && Number.isInteger(message.scanCode)
      && typeof message.key === "string" && typeof message.modifiers === "number";
  }
  if (message.type === "reset") return true;
  if (message.type === "keyDown" || message.type === "keyUp") {
    return Number.isInteger(message.vk) && Number.isInteger(message.scanCode)
      && typeof message.key === "string" && typeof message.modifiers === "number";
  }
  return false;
}