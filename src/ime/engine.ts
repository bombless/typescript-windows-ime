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
    const candidates = result.commit !== undefined
      ? getCandidates(state.composition)
      : getCandidates(result.state.composition);
    const response: ResponseMessage = {
      id: event.id,
      consume: result.consume,
      composition: result.state.composition,
      candidates,
      ...(result.commit !== undefined ? { commit: candidates[0]?.text ?? result.commit } : {}),
    };
    return { state: result.state, response };
  }
}