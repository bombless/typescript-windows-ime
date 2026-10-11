export type InputMode = "chinese" | "english";
export interface ImeState { composition: string; selectedCandidate: number; candidatePage?: number; mode?: InputMode; }
export const initialImeState = (): ImeState => ({ composition: "", selectedCandidate: 0 });