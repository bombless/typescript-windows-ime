export interface ImeState { composition: string; }
export const initialImeState = (): ImeState => ({ composition: "" });