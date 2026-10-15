# Repository Guidelines

## Project Structure & Module Organization

The TypeScript runtime lives in src/: ime/ contains state, reduction, candidates, and engine logic; protocol/ defines JSONL messages and codecs; and pipe/ contains the named-pipe server. Tests are in test/ and use matching *.test.ts names. The Windows-native TSF/COM adapter and pipe bridge are in native/; its PowerShell scripts build, register, unregister, and clean up the adapter. The pinyin dictionary is under data/. Generated JavaScript goes to dist/ and should not be edited manually.

## Build, Test, and Development Commands

Use Node.js 20+ on Windows:

- npm install — install dependencies.
- npm test — run the Vitest suite once.
- npm run test:watch — run Vitest interactively while developing.
- npm run build — compile strict TypeScript to dist/.
- npm run dev — run the TypeScript server through tsx.
- npm start — run the compiled server.
- .\native\build.ps1 — build the native DLL/host; registration scripts require elevated 64-bit PowerShell.

For an end-to-end run, start the native host and Node process separately as described in README.md; components communicate with UTF-8 JSON Lines over Windows named pipes.

## Coding Style and Naming Conventions

Follow strict TypeScript settings: ES modules, two-space indentation, semicolons, and double quotes. Use camelCase for variables/functions, PascalCase for classes/types, and descriptive message discriminators such as keyDown. Keep IME business logic in TypeScript; limit C++ changes to TSF/COM and IPC responsibilities. Preserve explicit .js extensions in TypeScript imports. Run npm run build before submitting changes.

## Architecture and Responsibility Boundaries

**Core project rules — three layers with separate responsibilities:**

- **C++ is the Windows safety and integration layer.** It provides the OS-facing safeguards: TSF/COM lifecycle and object-lifetime safety, key-event consumption boundaries, context/focus handling, safe text insertion, IPC boundary checks, and explicit failure logging/recovery. C++ must enforce platform and process safety, but must not become a second owner of pinyin composition, candidate ranking, or application behavior.
- **Go is an independent native implementation of the C++ TSF/COM path, not merely a host/IPC bridge.** The Go implementation must provide its own COM objects and implement the corresponding Windows TSF integration and lifecycle rather than relying on the C++ component to perform TSF operations. Reproduce the C++ path's externally observable integration behavior, including COM activation/lifetime, TSF callbacks and key-sink lifecycle, focus/context handling, edit-session and text-application behavior, IPC framing/correlation, error handling, logging, and recovery, as applicable to the implementation. Keep the TypeScript-facing protocol compatible so the Go and C++ native paths can serve as alternative implementations. Do not use Go as a second implementation of IME business rules. Document and test implementation parity explicitly; an architectural goal or passing Go unit tests alone does not establish full TSF/COM parity.
- **TypeScript owns application/IME runtime logic.** It is the authority for logical composition state, key-to-state transitions, candidate generation/ranking, selection, commit/cancel semantics, and protocol-level IME decisions. C++ and Go forward events and apply authoritative responses; they must not independently reinterpret or duplicate these decisions.
- **Keep contracts explicit across all three layers.** TypeScript defines what the IME should do; C++ and Go are alternative native implementations that independently provide the Windows safety and TSF/COM execution guarantees. Any cross-layer behavior change must update relevant protocol contracts and tests, and must not create divergent business logic.

### TypeScript responsibilities (`src/`)

- `src/ime/state.ts`: define the logical IME state, including composition text and selected candidate. Add state here when the engine needs to own it.
- `src/ime/reducer.ts`: implement deterministic key-to-state transitions and the `consume` decision. Backspace/Delete, Escape, navigation, candidate selection, and commit semantics belong here. Do not mutate IME business state in C++.
- `src/ime/engine.ts`: coordinate reduction, candidate generation, and protocol responses.
- `src/ime/candidates.ts`: generate and rank candidates from logical composition; keep dictionary and conversion policy out of native code.
  - Candidate matching order matters: prefer exact dictionary-code matches first. For a one-letter query, prefer single-Han-character candidates; if the exact code has none, search codes beginning with that letter for single-character candidates before falling back to multi-character entries.
  - For a multi-letter incomplete query, check real full-pinyin dictionary codes that start with the entire query before trying arbitrary composition splits. This lets inputs such as `xiangm` find `xiangmu` → `项目` instead of composing unrelated syllables.
  - Dictionary-generated initials aliases must be marked with `isInitials`. They may support abbreviated input, but must not be treated as full syllables inside a longer composition; filter them out of composition splitting and full-pinyin-prefix matching to avoid noise such as `lianx` → `里啊那些`.
  - Keep these rules covered by regression tests for one-letter input (`x`), incomplete pinyin (`xiangm`), and alias-splitting regressions (`lianx`). When debugging candidate ranking, set `IME_CANDIDATE_TRACE=1` to log matching mode, prefix matches, and composition split/weight details.
- `src/pipe/` and `src/protocol/`: own JSONL message handling, validation, and protocol types. Keep requests/responses explicit and compatible with the native bridge.
- Maintain state per TSF session. A key event must update only the session named in its message.

### Native responsibilities (`native/`)

- Both native paths must implement Windows integration independently: TSF activation/deactivation, their own COM interfaces/objects, key-event sink registration, context/edit-session lifecycle, focus/context handling, and safe insertion/commit of text through TSF APIs. The Go path must not depend on the C++ COM server to perform these operations.
- Translate native virtual keys and modifier state into protocol messages, and relay the TypeScript response. Do not independently edit the pinyin string or decide which candidate should be selected.
- Implement TSF's test-key phase (`OnTestKeyDown`) as a **side-effect-free consumption query**. It may ask TypeScript `testKeyDown` whether a key would be consumed, but must not mutate composition, commit text, or apply an edit during this phase.
- Implement the actual key-down phase (`OnKeyDown`) by sending `keyDown` and applying the returned response only when appropriate. The logical state transition happens once, in TypeScript, in this phase—not in both phases.
- `ApplyResponse` and edit-session code are presentation/OS integration, not a second IME engine. They apply the authoritative composition/commit and candidate data returned by TypeScript, manage native TSF object lifetimes, and report failures. Do not silently invent a different composition to recover from an error; log it and use an explicit reset/resynchronization path.
- Keep native candidate-window rendering, caret anchoring, DPI handling, and resource management in C++ where they require Windows APIs. Candidate contents and selected index must come from TypeScript.
- Native key-name translation may map `VK_BACK` to `Backspace` and `VK_DELETE` to `Delete`; it must not implement their editing semantics.

### Key Consumption and Editing Contract

- `testKeyDown` predicts whether the corresponding key-down should be eaten; it must be consistent with the actual `keyDown` decision for the same session/state. It must not advance state. If prediction depends on state, both paths must use the same TypeScript reducer semantics.
- `keyDown` is the authoritative state-changing event. A consumed editing key must not also reach the application as a normal key, or the application may delete unrelated document text. An unconsumed key must pass through normally.
- While a composition is active, Backspace removes the character immediately before the logical composition caret; Delete removes the character at the caret. If the product intentionally supports only end-of-buffer editing, document that invariant and make the caret behavior consistent. Do not assume `slice(1)` is correct for Delete without defining the caret model.
- When composition is empty, Backspace/Delete should normally be unconsumed so the target application can handle document editing. Escape, commit, and navigation behavior must likewise be specified in the TypeScript reducer and tested.
- A response's `composition`, optional `commit`, `candidates`, `selectedCandidate`, and `consume` must describe one coherent post-event result. Native code must not apply the same edit twice. Empty composition, commit, cancellation, focus changes, and TSF-terminated compositions must keep TypeScript state and native TSF state synchronized.
- Keep IPC request/response correlation and per-session routing correct. If IPC or a TSF edit session fails, log enough context to diagnose it and explicitly recover/resynchronize; do not leave stale native composition objects or silently desynchronize the two sides.

### Change and Test Rules

- For any IME behavior change, update TypeScript logic and focused tests first. Add reducer/engine tests for both `testKeyDown` prediction and `keyDown` mutation, including empty and non-empty composition, Backspace, Delete, Escape, commit, and session isolation as applicable.
- Keep native tests focused on the adapter contract: virtual-key translation, TSF consumption, protocol forwarding, and edit-session application. Native tests must not become an alternate specification for pinyin editing semantics.
- For regressions reported in a real Windows app, inspect logs across all three boundaries: TSF test-key result, Node request/response, and native `ApplyResponse`/edit-session result. A passing reducer test alone does not prove that the displayed TSF composition is correct.
- Run `npm test` and `npm run build` for TypeScript changes. For native changes, also run the relevant native build/mock tests and verify behavior in a real TSF-enabled application when available. Do not remove TSF key sinks or `ApplyResponse` as a shortcut for moving business logic into TypeScript.

## Testing Guidelines

Vitest is the test framework. Add focused tests in test/ using matching *.test.ts names. Protocol tests should cover malformed and fragmented JSONL as well as round trips; IME tests should cover state transitions and consumption. Run npm test and keep tests independent of Windows TSF where possible.

## Commit & Pull Request Guidelines

Recent commits use short imperative or action-oriented subjects, often concise Chinese, such as 修复 and 接入小狼毫. Keep commits focused and explain the user-visible or architectural effect. Pull requests should describe behavior changes, list validation commands and native prerequisites, link related issues when applicable, and include screenshots or reproduction steps for Windows/TSF-visible changes.

## Security & Configuration Tips

Treat named-pipe input as untrusted: preserve validation and bounded message handling. Do not commit credentials, generated binaries, or installed DLL copies. If a stale COM/TSF process holds a DLL, use native\cleanup-com.ps1 before rebuilding.
