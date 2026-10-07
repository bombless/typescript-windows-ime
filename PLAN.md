# TypeScript Windows IME — Implementation Plan

## 1. Project goal

Build a Windows TSF input method whose native C++ layer is intentionally thin and whose input-method logic lives in Node.js/TypeScript.

The architecture is:

```text
Windows TSF
    │
    ▼
C++ Native Adapter DLL
    │
    │ Named Pipe (JSONL)
    ▼
Node.js / TypeScript IME Core
    │
    ├─ input state machine
    ├─ composition logic
    ├─ candidate generation
    ├─ dictionary / configuration
    └─ future Go/AI engine integration
```

The native DLL owns Windows-specific responsibilities. TypeScript owns product logic.

## 2. Design principles

1. Keep COM/TSF code as small as possible.
2. Do not put IME business logic into C++.
3. Use a versioned, explicit IPC protocol between C++ and Node.js.
4. Node.js failure must never crash or wedge TSF.
5. The native layer must fail open: if the Node process is unavailable, normal keyboard input continues.
6. Composition and candidate operations remain native because they require TSF context interfaces.
7. Keep installation/registration separate from development and test harnesses.
8. Build an end-to-end vertical slice before adding sophisticated IME behavior.

## 3. Repository layout

```text
typescript-windows-ime/
├─ README.md
├─ PLAN.md
├─ package.json
├─ tsconfig.json
├─ src/
│  ├─ index.ts
│  ├─ protocol/
│  │  ├─ messages.ts
│  │  └─ codec.ts
│  ├─ ime/
│  │  ├─ state.ts
│  │  ├─ reducer.ts
│  │  ├─ candidates.ts
│  │  └─ engine.ts
│  └─ pipe/
│     └─ server.ts
├─ native/
│  ├─ GoIMETextService.h
│  ├─ GoIMETextService.cpp
│  ├─ GoIMEKeyEventSink.cpp
│  ├─ PipeBridge.h
│  ├─ PipeBridge.cpp
│  ├─ GoIMEClassFactory.cpp
│  ├─ GoIMEExports.cpp
│  ├─ GoIMEDiagnostics.cpp
│  └─ build.ps1
├─ test/
│  ├─ protocol.test.ts
│  ├─ state.test.ts
│  └─ integration/
├─ scripts/
│  ├─ dev-start.ps1
│  ├─ dev-stop.ps1
│  └─ smoke-test.ps1
└─ docs/
   ├─ protocol.md
   ├─ tsf.md
   └─ troubleshooting.md
```

## 4. Phase 0 — project bootstrap

### Deliverables

- Initialize Node.js + TypeScript project.
- Add strict TypeScript configuration.
- Add test runner.
- Add a minimal CLI entry point.
- Add README and this plan.
- Keep the native project independently buildable.

### Acceptance criteria

- `npm install` succeeds.
- TypeScript compiles with strict mode.
- Tests can run without Windows TSF.
- Native DLL build remains independent from Node tooling.

## 5. Phase 1 — IPC protocol

Use a local Windows Named Pipe with JSON Lines framing.

Initial pipe name:

```text
\\.\pipe\TypeScriptWindowsIME
```

Every request has a numeric `id` and `type`.

Initial messages:

```json
{"id":1,"type":"hello","protocol":1}
{"id":2,"type":"keyDown","vk":65,"scanCode":30,"key":"A","modifiers":0}
{"id":3,"type":"keyUp","vk":65,"scanCode":30,"key":"A","modifiers":0}
{"id":4,"type":"reset"}
```

Initial response:

```json
{
  "id": 2,
  "consume": true,
  "composition": "a",
  "candidates": []
}
```

### Protocol rules

- UTF-8 JSONL.
- One message per line.
- Unknown message types return a structured error.
- Protocol version is negotiated during `hello`.
- Request IDs are echoed.
- Messages must have bounded size.
- Malformed messages never crash the Node process.
- Native side treats pipe disconnect as a recoverable condition.

## 6. Phase 2 — TypeScript IME core

Start deliberately simple.

### State

```text
Idle
  ↓ keyDown(A-Z)
Composing("a")
  ↓ keyDown
Composing("ab")
  ↓ Backspace
Composing("a")
  ↓ Space / Enter
Commit
  ↓
Idle
```

### First behavior

- A-Z append to composition.
- Backspace removes the last character.
- Escape resets composition.
- Space commits the current composition for the first prototype.
- Enter commits the current composition.
- Non-consumed keys return `consume:false`.

Do not implement real Chinese conversion yet.

## 7. Phase 3 — C++ TSF adapter

Port the existing native concepts into this project, but keep them minimal.

### COM responsibilities

- `DllGetClassObject`.
- `IClassFactory`.
- `ITfTextInputProcessor`.
- `ITfKeyEventSink`.
- Correct `QueryInterface` / `AddRef` / `Release`.
- `Activate` / `Deactivate`.
- `ITfKeystrokeMgr::AdviseKeyEventSink` / `UnadviseKeyEventSink`.

### Key-event flow

```text
OnTestKeyDown
    ↓
Ask Node whether the key is consumed
    ↓
OnKeyDown
    ↓
Forward key event to Node
```

The C++ adapter must not implement candidate-selection or linguistic logic.

## 8. Phase 4 — PipeBridge

Implement a native `PipeBridge` abstraction.

Responsibilities:

- Connect to the Node server.
- Send JSONL messages.
- Receive responses.
- Match response IDs.
- Detect disconnects.
- Reconnect safely.
- Apply timeouts.
- Avoid blocking the TSF callback indefinitely.

### Failure behavior

If Node is unavailable or times out:

```text
consume = false
```

unless a future explicit fail-safe policy says otherwise.

The user must retain normal keyboard input.

## 9. Phase 5 — TSF composition bridge

Once key events work end-to-end, add native composition handling.

C++ responsibilities:

- Obtain the active `ITfContext`.
- Start/update/end `ITfComposition`.
- Set composition text.
- Set selection.
- Commit final text.

Node responsibilities:

- Decide composition text.
- Decide when text is committed.
- Produce candidate lists.

Example:

```text
KeyDown('n')
  → Node: composition="n"
  → C++: update TSF composition

KeyDown('i')
  → Node: composition="ni"
  → C++: update TSF composition

Space
  → Node: commit="你"
  → C++: end composition / commit
```

## 10. Phase 6 — candidate protocol

Add candidate messages only after basic composition works.

Suggested response shape:

```json
{
  "id": 42,
  "consume": true,
  "composition": "ni",
  "candidates": [
    {"text":"你","annotation":""},
    {"text":"尼","annotation":""},
    {"text":"呢","annotation":""}
  ],
  "selectedCandidate": 0
}
```

Later add:

- Up/down candidate selection.
- Number-key selection.
- Page navigation.
- Candidate window positioning.
- Candidate annotations.

## 11. Phase 7 — Node engine abstraction

The TypeScript core should not directly depend on one dictionary or AI backend.

Define:

```ts
interface ImeEngine {
  processKey(event: KeyEvent, state: ImeState): EngineResult;
}
```

Possible implementations:

```text
SimpleEngine
DictionaryEngine
GoEngineAdapter
AiEngineAdapter
```

This keeps the Windows TSF layer completely independent from future engine choices.

## 12. Phase 8 — robustness

Test:

- Node not started.
- Node crashes while composing.
- Pipe disconnects.
- Pipe reconnects.
- Malformed JSON.
- Oversized message.
- Slow Node response.
- Rapid key presses.
- TSF activation/deactivation cycles.
- Repeated COM creation/destruction.
- Explorer / Notepad / browser text fields.

Important invariant:

> A Node.js failure must degrade to ordinary keyboard behavior rather than breaking Windows text input.

## 13. Phase 9 — registration and installation

Only after the development harness is stable:

- Decide final CLSID/profile registration.
- Register native DLL.
- Register TSF text service/profile.
- Add install/uninstall scripts.
- Keep development registration separate from production registration.

Do not mix installation work into the early IPC/IME implementation.

## 14. Phase 10 — end-to-end acceptance test

The first meaningful milestone is:

```text
Windows TSF
  → C++ ITfKeyEventSink
  → Named Pipe
  → TypeScript
  → state machine
  → Named Pipe
  → C++
  → TSF composition
  → visible text
```

Acceptance scenario:

1. Start Node server.
2. Activate GoIME through TSF.
3. Focus a normal Windows text field.
4. Press `a`.
5. Observe Node receives `keyDown`.
6. Node returns composition `a`.
7. TSF displays `a`.
8. Press Backspace.
9. Composition becomes empty.
10. Press Space/Enter to commit.
11. Stop Node.
12. Verify ordinary keyboard input still works.

## 15. Development order

Implement in this exact order:

```text
1. Repository + TypeScript bootstrap
2. Protocol types + JSONL codec
3. Node pipe server
4. TypeScript state machine
5. Native PipeBridge
6. Native TSF KeyEventSink
7. End-to-end key event round trip
8. Native TSF composition bridge
9. Commit behavior
10. Candidate protocol
11. Candidate UI
12. Real IME engine
13. Robustness testing
14. Production registration/installer
```

## 16. Explicit non-goals for the first milestone

Do NOT implement initially:

- AI inference.
- Cloud services.
- Complex Chinese segmentation.
- Dictionary learning.
- Full candidate UI.
- Installer automation.
- Multiple simultaneous Node workers.
- Cross-platform support.

The first goal is simply a reliable TSF ↔ C++ ↔ Named Pipe ↔ TypeScript round trip.

## 17. Current starting point

The previous native prototype has already demonstrated that the COM class factory and `ITfTextInputProcessor`/`ITfKeyEventSink` interfaces can be implemented in native C++. The new project should reuse the proven concepts but keep its own codebase isolated from the previous GoIME project.

The current Windows COM test registration may temporarily point the existing CLSID at the native DLL during development. Production installation must not be changed until the end-to-end design is validated.

## 18. Immediate next task

Create the project skeleton and implement **Phase 0 + Phase 1** first:

1. `package.json`
2. `tsconfig.json`
3. `src/protocol/messages.ts`
4. `src/protocol/codec.ts`
5. `src/pipe/server.ts`
6. basic protocol tests
7. README with development commands

Then implement the native `PipeBridge` and perform the first C++ ↔ TypeScript round trip before adding more TSF behavior.
