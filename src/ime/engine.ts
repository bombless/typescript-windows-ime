import type { KeyMessage, ResponseMessage, TestKeyMessage } from "../protocol/messages.js";
import { getCandidates } from "./candidates.js";
import { reduceKey } from "./reducer.js";
import type { ImeState } from "./state.js";

export interface ImeEngine {
  processKey(event: KeyMessage, state: ImeState): { state: ImeState; response: ResponseMessage };
  testKey(event: KeyMessage | TestKeyMessage, state: ImeState): { consume: boolean };
}

export class SimpleEngine implements ImeEngine {
  testKey(event: KeyMessage | TestKeyMessage, state: ImeState) {
    const keyEvent: KeyMessage = event.type === "testKeyDown" ? { ...event, type: "keyDown" } : event;
    return { consume: reduceKey(state, keyEvent).consume };
  }

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