import type { KeyMessage, ResponseMessage } from "../protocol/messages.js";
import { getCandidates } from "./candidates.js";
import { reduceKey } from "./reducer.js";
import type { ImeState } from "./state.js";

export interface ImeEngine {
  processKey(event: KeyMessage, state: ImeState): { state: ImeState; response: ResponseMessage };
}

export class SimpleEngine implements ImeEngine {
  processKey(event: KeyMessage, state: ImeState) {
    const result = reduceKey(state, event);
    const response: ResponseMessage = {
      id: event.id,
      consume: result.consume,
      composition: result.state.composition,
      candidates: getCandidates(),
      ...(result.commit !== undefined ? { commit: result.commit } : {}),
    };
    return { state: result.state, response };
  }
}