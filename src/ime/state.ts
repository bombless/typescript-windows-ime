export interface ImeState { composition: string; selectedCandidate: number; }
export const initialImeState = (): ImeState => ({ composition: "", selectedCandidate: 0 });