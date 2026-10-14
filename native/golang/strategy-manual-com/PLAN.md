# Strategy B: Hand-written Go COM ABI and TSF interfaces

## Goal
Implement the TSF COM server directly in Go without depending on a general-purpose Go COM implementation framework.

## Core idea
- Define only the COM interfaces and vtables required by this project.
- Implement `IUnknown`, `QueryInterface`, reference counting, class factory behavior, and required TSF interfaces with explicit ABI layouts.
- Use generated or carefully verified Windows declarations for GUIDs, HRESULTs, and method signatures; never guess layouts or calling conventions.
- Keep all input-method policy and state in Go.

## Work packages
1. [x] Inventory the initial interfaces from the sibling adapter and Windows SDK headers. Continue the inventory before adding more interfaces.
2. [partial] Write ABI conformance tests for GUIDs, vtable order, pointer sizes, calling convention, HRESULTs, and reference-count transitions. GUID byte-order tests now cover `IUnknown`, `IClassFactory`, `ITfTextInputProcessor`, and the project CLSID; HRESULT bit-pattern tests were added. Callback ABI and foreign-host lifetime remain unproven.
3. [ ] Implement the minimal COM class factory and `ITfTextInputProcessor`.
4. [ ] Add `ITfKeyEventSink` and key-event registration.
5. [ ] Add edit-session-based composition and text commit.
6. [ ] Run activation and typing tests on every supported architecture; document unsupported architectures.

## Continuation checkpoint (2026-10-09)

### Completed in this checkpoint

Created the initial ABI-conformance package in this directory:

- `go.mod`: standalone module `typescript-windows-ime-strategy-manual-com` (Go 1.23 language level).
- `abi_windows.go`: `HRESULT`, `GUID`, initial IIDs/CLSID, interface vtable slot counts, atomic `RefCount`, GUID byte encoding, and runtime pointer-size helper.
- `abi_windows_test.go`: GUID byte-order tests, vtable slot-count tests, reference-count transition/underflow test, and amd64 pointer-size test.
- `README.md`: source references, scope limitations, and local test command.

### Continuation work (2026-10-09)

- Re-read this plan, README, ABI source/tests, and sibling `adapter/TsfGoAdapter.cpp` before editing; current worktree changes were preserved.
- SDK inspection found and corrected an important IID transcription error: `ITfTextInputProcessor` in `um/msctf.h` is `AA80E7F7-2021-11D2-93E0-0060B067B86E` (not `AA80E801-...`). Updated the declaration and expected GUID byte sequence.
- Added GUID byte checks for `IID_IUnknown` and `IID_IClassFactory`, and HRESULT bit-pattern checks for `S_OK`, `E_POINTER`, `E_NOINTERFACE`, `CLASS_E_NOAGGREGATION`, `CLASS_E_CLASSNOTAVAILABLE`, and `E_OUTOFMEMORY`.
- Verified SDK declarations show `IUnknown` has `QueryInterface`, `AddRef`, `Release`; `IClassFactory` appends `CreateInstance`, `LockServer`; `ITfTextInputProcessor` appends `Activate`, `Deactivate`. SDK uses `STDMETHODCALLTYPE` for interface methods and `WINAPI` for DLL entry points.
- Installed toolchain is `go1.27.0 windows/amd64`, `CGO_ENABLED=1`. `go doc cmd/cgo` states that calling C function pointers from Go is unsupported and describes restrictions on retaining Go pointers. This does not by itself prove a pure-Go COM vtable is impossible, but the callback/export ABI and lifetime path are not yet validated; do not implement guessed vtable callbacks. Next step: run a minimal isolated callback/export ABI experiment (outside any COM host), then decide whether a pure-Go vtable is technically viable or whether the plan must stop at a documented blocker and compare Strategy A / `strategy-thin-cpp`.
- Validation after these edits (from `native/golang/strategy-manual-com`): `gofmt -w abi_windows.go abi_windows_test.go` PASS; `go test ./...` PASS; `go vet ./...` PASS; `go test -race ./...` PASS; `GOARCH=386 go test -c` PASS; `GOARCH=arm64 go test -c` PASS. Cross-architecture binaries were compile-checked only, not executed; only Windows amd64 runtime tests have been run. A first command attempt from the repository root failed because the Go files are in this subdirectory; rerunning from the module directory passed.
- Work-package status remains #2 partial; #3 is intentionally not started because the callable-vtable/export ABI and object lifetime path are not yet proven. Next actionable task is an isolated exported-callback ABI experiment and explicit comparison against Strategy A and `strategy-thin-cpp`; do not register or claim TSF activation.

Local Windows environment observed: Windows 10/11 build family `10.0.26200`, `amd64`; Go toolchain reported `go1.27.0 windows/amd64`; Windows SDK headers found at `C:\Program Files (x86)\Windows Kits\10\Include\10.0.22000.0`. `um/unknwn.h` was inspected for `IClassFactory`; `um/msctf.h` was located for TSF declarations. Existing adapter source `native/golang/adapter/TsfGoAdapter.cpp` was inspected for the project CLSID and current activation expectations.

Commands run from this strategy directory and results:

```powershell
go test ./...       # PASS
go vet ./...        # PASS
go test -race ./... # PASS
```

Note: an initial `go test ./...` from the repository root failed because the root is not a Go module; rerunning from this strategy directory passed. This was a working-directory issue, not a code-test failure.

### Important current limitation

This is only an ABI declaration/test slice. **There is no COM DLL/server implementation yet**: no callable COM vtables, no exported `DllGetClassObject` / `DllCanUnloadNow`, no registered class factory, and no real TSF activation test. Do not claim pure-Go COM activation is working and do not register this prototype as an input method. The `RefCount` is a unit-test model, not yet wired to a COM object's destruction lifecycle.

### Next-context instructions — continue from here

1. Re-read this file, `README.md`, `abi_windows.go`, `abi_windows_test.go`, and `native/golang/adapter/TsfGoAdapter.cpp` before editing. Preserve unrelated pre-existing working-tree changes; do not reset, revert, or commit them.
2. Inspect the exact SDK declarations, GUID definitions, `STDMETHODCALLTYPE`/`WINAPI` conventions, and vtable ordering for `IUnknown`, `IClassFactory`, and `ITfTextInputProcessor`. Add source notes and tests for every ABI-critical assumption. Verify GUID bytes against SDK declarations rather than relying only on copied literals.
3. Research and experimentally validate Go's Windows callback/export ABI and lifetime rules for a DLL loaded by a foreign COM host. Do not invent a calling convention or assume that a Go `//export` function alone provides a valid COM object/vtable. Determine whether pure Go can safely provide the required callable function pointers and keep Go objects/runtime state alive across callbacks.
4. Implement the smallest viable activation experiment only if the ABI path is supported: `IUnknown` (`QueryInterface`, `AddRef`, `Release`), `IClassFactory` (`CreateInstance`, `LockServer`), and `ITfTextInputProcessor` (`Activate`, `Deactivate`); reject aggregation and unsupported IIDs correctly; define object/server lifetime accounting. Keep the existing CLSID `5D8C1D62-9F73-4B4C-A3D2-1E7B8A9C6401` aligned with the sibling adapter unless an intentional project-wide change is justified.
5. Add Windows integration/smoke tests for class-object lookup, supported and unsupported `QueryInterface`, null output pointers, aggregation rejection, `AddRef`/`Release`, activation failure, deactivation, and unload accounting. Run tests in a separate test host where possible; a broken vtable can crash the process.
6. If callback ABI or runtime constraints require a native shim, stop and document a minimal reproducible experiment and the exact limitation. Compare with Strategy A and the existing `strategy-thin-cpp` option; do not quietly add C++ while still describing the result as pure Go.
7. Only after activation and teardown pass in a real TSF host should work proceed to `ITfKeyEventSink`, key-event registration, edit sessions, composition, and text commit. Candidate logic is out of scope until the activation/teardown milestone is stable.
8. Re-run `gofmt`, `go test ./...`, `go vet ./...`, and `go test -race ./...` from this directory. Also attempt Windows x86/arm64 compile/tests only when toolchain/host support permits; clearly label untested architectures.
9. Update this checkpoint and the work-package checkboxes with exact commands/results, any remaining blockers, and the next actionable step. Do not claim real application typing support until it has been observed and recorded.

### Continuation result (2026-10-09): native callback ABI spike

- Re-read this plan, `README.md`, `abi_windows.go`, `abi_windows_test.go`, and `native/golang/adapter/TsfGoAdapter.cpp`; inspected the sibling Strategy A (`strategy-go-com`) and Strategy C (`strategy-thin-cpp`) plans and execution notes. Existing unrelated worktree changes were preserved; nothing was reset, staged, or committed.
- Confirmed the SDK declarations in Windows SDK `10.0.22000.0`: `um/unknwn.h` declares `IClassFactory` with `CreateInstance` then `LockServer` after `IUnknown`; `um/msctf.h` around line 10500 declares `ITfTextInputProcessor : public IUnknown`, with `Activate(ITfThreadMgr*, TfClientId)` then `Deactivate()`. These interface methods use `STDMETHODCALLTYPE`; DLL entry points use `WINAPI`. The correct SDK IID remains `AA80E7F7-2021-11D2-93E0-0060B067B86E`; Strategy A's current `PLAN.md`/`main.go` contain the stale `AA80E801` value and must not be copied as authoritative.
- Added `callback-experiment/` as a standalone, Windows-only test. `syscall.NewCallback` produced a callable native thunk: C invoked it 10,000 times, Go GC was forced, and one further invocation succeeded. Result: `PASS: C called syscall.NewCallback thunk 10001 times; result=0x123b; calls=10001; pointer-size=8` (`go run .`, exit 0; Go 1.27.0 windows/amd64, cgo enabled). The experiment uses only scalar arguments/results and does not retain Go pointers in C.
- This validates one callback mechanism on Windows amd64 only; it does **not** prove COM vtable layout, a foreign COM host's lifetime behavior, multithreaded/reentrant calls, DLL unload safety, x86/arm64 behavior, or TSF activation. No COM DLL was registered and no system IME state was changed.
- Strategy comparison: Strategy A (`strategy-go-com`) currently builds a Go shared DLL and reaches its class factory, but its recorded `CreateInstance(IID_ITfTextInputProcessor)` smoke test fails with `E_NOINTERFACE`; it also uses `syscall.NewCallback`, so it does not yet establish a working COM object. Strategy C (`strategy-thin-cpp`) has a working native adapter/typed Go bridge smoke test, but its checkpoint explicitly says real TSF key-sink registration, edit sessions, and visible typing/commit are still absent. Strategy B remains the pure-Go-ABI investigation; neither alternative is a fully working IME yet.
- Validation from `native/golang/strategy-manual-com`: `gofmt -w abi_windows.go abi_windows_test.go callback-experiment\main.go` PASS; `go test ./...` PASS; `go vet ./...` PASS; `go test -race ./...` PASS; callback experiment PASS. The cross-architecture compile checks from the previous checkpoint remain compile-only and were not rerun in this continuation.
- Work package #2 remains partial and #3 remains not started. Next actionable step: extend the isolated harness to call Go callbacks through a COM-shaped `IUnknown` vtable with native-allocated object/vtable memory and verify `QueryInterface` output initialization plus `AddRef`/`Release` transitions. Only after that harness proves pointer ownership and lifetime boundaries should the minimal class-factory/TSF activation experiment begin. Do not implement key events or claim TSF activation before the native host smoke test passes.

## Acceptance criteria
- The Go implementation loads and behaves as a COM in-process server without a C++ implementation layer.
- Interface layouts and callback signatures are checked against authoritative Windows declarations.
- TSF lifecycle and key handling work in a real application, not only a mock harness.
- Automated tests cover reference counts, unsupported interfaces, activation failure, and deactivation.

## Main risks
- Manual vtable and ABI work is easy to get subtly wrong and can crash the host process.
- Go runtime and callback lifetime constraints may make parts of the server impractical without a native shim.
- Maintenance burden grows as more TSF interfaces are required.

## First experiment
Implement only `IUnknown`, `IClassFactory`, and the smallest viable `ITfTextInputProcessor` activation path. Do not implement candidate logic until activation and teardown are stable.

## Scope boundaries
- No C++ source in the target implementation; if a C shim becomes necessary, record the precise limitation and compare against Strategy A.
- Keep ABI declarations minimal and sourced; avoid copying a broad Windows binding surface into the project.

### Continuation result (2026-10-09): COM-shaped IUnknown vtable spike

- Extended `callback-experiment/bridge.c` to allocate the object and three-slot vtable in native memory, storing Go `syscall.NewCallback` thunks for `QueryInterface`, `AddRef`, and `Release`. Calls are made from C through the vtable, not by directly calling the Go callbacks.
- `callback-experiment/main.go` verifies successful `QueryInterface(IID_IUnknown)`, output pointer identity, unsupported IID returning `E_NOINTERFACE` with output initialized to null, null output returning `E_POINTER`, `AddRef`/`Release` count transitions, and callback dispatch after forced GC. Result on Windows amd64: `PASS: scalar callback=10001 calls; COM-shaped IUnknown vtable checks passed; vtable-callbacks=7; refs=2; pointer-size=8`.
- Native C owns the object/vtable allocations and keeps only native callback addresses and native pointers; Go pointers are not stored in native memory. Pointer dereference and native refcount mutation are performed by C helper functions so `go vet` remains clean.
- Validation from `native/golang/strategy-manual-com`: `gofmt -w callback-experiment/main.go` PASS; `go test ./...` PASS; `go vet ./...` PASS; `go test -race ./...` PASS; `go run .` in `callback-experiment` PASS; `GOARCH=386 go test -c` PASS; `GOARCH=arm64 go test -c` PASS. Cross-architecture outputs were compile-checked only and not executed.
- This is a stronger ABI spike, but still not proof of full COM safety: the harness manually frees native memory and does not tie a zero COM refcount to destruction; it does not test foreign-host loading, concurrent/reentrant callbacks, DLL unload, class factory behavior, TSF activation, or real typing. No DLL was registered and no system IME state changed.
- Work package #2 remains partial; #3 remains intentionally not started. Next step: test callback concurrency/reentrancy and explicit lifetime rooting/unload assumptions in an isolated native host, then decide whether a minimal class factory is justified. If those constraints cannot be established without a native implementation layer, document the blocker and compare Strategy A with `strategy-thin-cpp`; do not claim pure-Go COM activation yet.

### Next-session handoff (2026-10-09)

Start the next context by reading this `PLAN.md` from top to bottom, then inspect `callback-experiment/README.md`, `callback-experiment/main.go`, and `callback-experiment/bridge.c`. Continue only in `native/golang/strategy-manual-com`; preserve all existing and unrelated worktree changes. Do not register the prototype, modify system IME settings, stage, or commit unless explicitly requested.

#### Current verified baseline

- Windows amd64 / Go 1.27.0 / cgo enabled.
- The isolated callback experiment passes: native C calls Go `syscall.NewCallback` thunks through a C-allocated, three-slot `IUnknown`-shaped vtable. It checks `QueryInterface(IID_IUnknown)`, output pointer identity, unsupported IID (`E_NOINTERFACE`) with output cleared, null output (`E_POINTER`), `AddRef`/`Release` return/refcount transitions, and dispatch after forced GC.
- Expected run output: `PASS: scalar callback=10001 calls; COM-shaped IUnknown vtable checks passed; vtable-callbacks=7; refs=2; pointer-size=8`.
- From this module directory, `go test ./...`, `go vet ./...`, `go test -race ./...`, and the callback experiment pass. `GOARCH=386 go test -c` and `GOARCH=arm64 go test -c` compile successfully but have not been executed on those architectures.
- The harness is not a COM server. C manually owns/frees object and vtable memory; zero reference count is not connected to destruction. No foreign COM host, concurrency/reentrancy, DLL unload, class factory, TSF activation, or real typing test has passed.

#### Tasks for the next session

1. Re-run the baseline commands above from this directory before edits and record any environment differences.
2. Design and implement an isolated native-host stress test for callback reentrancy and concurrent calls. Keep callbacks and native memory alive for the full test; do not call `Release` into object destruction and then dereference the freed object. Explicitly state whether `syscall.NewCallback` callback bodies may be entered concurrently and which parts of the harness are thread-safe. Do not treat `go test -race` alone as proof about native callback entry.
3. Investigate callback thunk lifetime and DLL/runtime teardown using authoritative Go/Windows documentation and a minimal reproducible test. Establish who owns the callback function pointer, when it becomes invalid, and whether a foreign COM host can call it during shutdown. Do not unload a DLL while a callback could still be in flight.
4. Re-check `go doc syscall.NewCallback`, Go runtime/cgo restrictions, and Windows COM rules for `STDMETHODCALLTYPE`, `IUnknown`, and object lifetime. Distinguish documented guarantees from observed behavior on this one machine.
5. Update this plan with exact commands, results, and unresolved constraints. If concurrency/lifetime/unload safety cannot be justified, stop before implementing `IClassFactory`; document the precise blocker and compare Strategy A (`native/golang/strategy-go-com`) with Strategy C (`native/golang/strategy-thin-cpp`). Do not quietly introduce C++ into the Strategy B target while calling it pure Go.
6. Only if the ABI and lifetime boundary is adequately established, implement the smallest class-factory/activation spike: `IUnknown`, `IClassFactory`, and `ITfTextInputProcessor`; test supported/unsupported IIDs, null pointers, aggregation rejection, refcount transitions, activation/deactivation, and unload accounting in a separate test host. Do not proceed to `ITfKeyEventSink`, composition, or text commit until activation and teardown pass.
7. Finish with `gofmt`, `go test ./...`, `go vet ./...`, `go test -race ./...`, and applicable architecture compile checks. Clearly label compile-only versus runtime-tested architectures. Update this handoff with the next actionable task so another context can continue without rediscovery.

#### Guardrails

- Never claim real COM/TSF activation or input support from the current vtable mock.
- Never infer ABI correctness solely from a successful callback invocation; verify calling convention, argument layout, ownership, threading, and teardown independently.
- Preserve unrelated dirty/untracked files. Do not reset, revert, stage, or commit changes.
- No registry changes or input method installation during isolated experiments.

### Continuation result (2026-10-09): native callback concurrency and reentrancy

- Re-ran baseline from `native/golang/strategy-manual-com` with Go `go1.27.0 windows/amd64`, `CGO_ENABLED=1`: `go test ./...` PASS; `go vet ./...` PASS; `go test -race ./...` PASS; `go run .` in `callback-experiment` PASS. An initial baseline invocation from the wrong working directory failed because it was outside the Go module; the correctly scoped rerun passed.
- Checked installed `go doc syscall.NewCallback`: it specifies a `stdcall` function pointer, uintptr-sized arguments/results constraints, a limited callback count, and explicitly says callback memory is never released. The installed documentation does not establish a complete COM DLL unload protocol or guarantee safe teardown while foreign callers may still be in flight.
- Extended `callback-experiment/bridge.c` with native `CreateThread` callers and `callback-experiment/main.go` with atomic Go counters. Eight native Windows threads each invoked the same scalar callback 10,000 times (80,000 concurrent calls); all return values were correct. The full scalar callback count was 90,001 including the original 10,001 calls.
- Added a bounded 64-level same-thread reentrancy test in which a Go callback re-enters its own `syscall.NewCallback` thunk through the C helper; result was 64 as expected. These are observed tests on Windows amd64, not proof of all foreign-host behavior.
- Updated `callback-experiment/README.md` to record test coverage and explicit limitations. No registry changes, IME installation, staging, or commits were performed.
- Validation after edits: `gofmt -w callback-experiment\\main.go` PASS; `go test ./...` PASS; `go vet ./...` PASS; `go test -race ./...` PASS; `go run .` PASS with output `PASS: scalar callback=90001 calls including 8x10000 concurrent calls; COM-shaped IUnknown vtable checks passed; vtable-callbacks=7; refs=2; pointer-size=8`.
- Remaining blockers: the harness does not model DLL unload, in-flight callback shutdown, COM apartment/marshaling, foreign COM host ownership, or refcount-driven destruction. The callback thunk allocation is documented as never released, but this alone does not prove that a Go DLL/runtime can safely unload while external code might call a thunk. Do not implement or register the class factory yet.
- Next actionable step: investigate Go Windows DLL/runtime lifetime behavior and COM in-process server unload requirements from authoritative source/docs; create a minimal host/DLL lifetime experiment only if it can be safely isolated. If a safe unload/lifetime contract cannot be established, record the precise blocker and recommend Strategy A versus Strategy C rather than claiming Strategy B can provide a production COM server. Keep work package #2 partial and #3 not started.

### Next-session handoff (2026-10-09, latest)

Continue from this exact directory: `D:\\mcp-agent-workspace\\typescript-windows-ime\\native\\golang\\strategy-manual-com`.

**Read first:** this entire `PLAN.md`, then `callback-experiment/README.md`, `callback-experiment/main.go`, and `callback-experiment/bridge.c`. Also inspect `abi_windows.go`, `abi_windows_test.go`, sibling `native/golang/strategy-go-com`, and `native/golang/strategy-thin-cpp` plans/checkpoints before deciding the implementation direction. Preserve all pre-existing modified/untracked files; do not reset, revert, stage, or commit.

**Latest verified baseline (Windows amd64, Go 1.27.0, CGO enabled):**

```powershell
go test ./...       # PASS (run from strategy-manual-com)
go vet ./...        # PASS
go test -race ./... # PASS
Push-Location callback-experiment
go run .            # PASS
Pop-Location
```

Latest callback experiment output:

```text
PASS: scalar callback=90001 calls including 8x10000 concurrent calls; COM-shaped IUnknown vtable checks passed; vtable-callbacks=7; refs=2; pointer-size=8
```

It tests a scalar `syscall.NewCallback` thunk; C invokes a C-allocated `IUnknown`-shaped vtable with `QueryInterface`, `AddRef`, and `Release`; supported/unsupported IID behavior; output-pointer initialization; null output; reference-count transitions; dispatch after GC; 80,000 concurrent calls from eight native Windows threads; and 64-level same-thread reentrancy. Go callback counters are atomic. Native workers are joined before the harness returns.

**Interpretation boundary:** all runtime observations are Windows amd64 only. This is still a mock harness, not a COM DLL. It does not test a foreign COM host, DLL unload or shutdown with callbacks in flight, apartment/marshaling rules, refcount-driven object destruction, class factory behavior, TSF activation, or real typing. `go doc syscall.NewCallback` says callback thunk memory is never released and documents stdcall/uintptr constraints; it does not establish a safe COM DLL unload contract. `go test -race` is not proof of safety for arbitrary native memory or foreign-host interactions. Cross-architecture x86/arm64 checks, where mentioned earlier, are compile-only and not runtime tests.

**Next actions, in order:**

1. Re-run the baseline commands above from the module directory and record exact results.
2. Read authoritative Go Windows callback, `c-shared`/DLL runtime, cgo pointer, and Windows COM in-process server/unload documentation. Separate documented guarantees from local observations; in particular determine whether a Go runtime-bearing DLL can meet COM unload semantics while preventing calls into invalid callback/runtime state.
3. If justified and safely isolatable, build a separate minimal host/DLL lifetime test that never unloads a module while a callback may be in flight. Do not register the class or alter IME settings.
4. If the lifetime/unload contract cannot be established, stop before implementing `IClassFactory`; write the exact blocker and a grounded Strategy A (`strategy-go-com`) vs Strategy C (`strategy-thin-cpp`) recommendation. Do not silently add C++ to Strategy B while calling it pure Go.
5. Only if ABI, threading, ownership, and teardown are sufficiently justified, implement a minimal class-factory/`ITfTextInputProcessor` activation spike in an isolated test host. Test class-object lookup, supported/unsupported `QueryInterface`, null outputs, aggregation rejection, `AddRef`/`Release`, activation/deactivation, and unload accounting. No `ITfKeyEventSink` or composition work until activation and teardown are proven.
6. Run `gofmt`, `go test ./...`, `go vet ./...`, `go test -race ./...`, plus architecture compile checks only where toolchain support permits. Label compile-only architectures explicitly.
7. Update this file with exact commands/results and the next handoff. Keep package #2 partial and #3 not started unless the acceptance evidence genuinely changes.

**Guardrails:** no registry edits or input method installation; no staging or commits; do not claim TSF activation or typing support based on the vtable harness.

### Lifetime harness follow-up (2026-10-09)

- Found the pre-existing `lifetime-experiment/` harness and reviewed its README, `main.go`, and `host.c` before running it. Rebuilt `LifetimeExperiment.dll` with `go build -buildmode=c-shared -o LifetimeExperiment.dll .`, rebuilt the native host with `clang .\host.c -o .\LifetimeHost.exe`, then ran ` .\LifetimeHost.exe (Join-Path (Get-Location) 'LifetimeExperiment.dll')` from that directory.
- Result: PASS — `PASS: loaded Go DLL; DllCanUnloadNow=S_FALSE; callback=10001 calls; post-GC call passed; module intentionally left loaded`.
- This confirms the local host can load the Go DLL, resolve the three experiment exports, observe `DllCanUnloadNow == S_FALSE`, invoke the fixed callback 10,000 times, force Go GC, and invoke the callback again while the DLL remains loaded. The host intentionally does not call `FreeLibrary`.
- It does not establish that COM will always honor the unload policy, that a COM server's object/refcount lifetime is correct, or that a Go DLL can safely be unloaded/reloaded. The lifetime experiment has no `DllGetClassObject` and is not a COM server. Therefore the prior stop-before-class-factory decision remains unchanged.
- No registration, IME changes, staging, or commits were performed. Next recommended implementation path remains Strategy C; keep Strategy B as a documented pure-Go ABI investigation unless a supportable host/runtime lifetime contract is established.

### Continuation result (2026-10-09): resumed via Strategy C lifecycle hardening

- Followed the documented decision to pause Strategy B before `IClassFactory` and continue the existing Strategy C (`native/golang/strategy-thin-cpp`) path. Re-read the Strategy C plan and inspected `TsfGoAdapter.cpp`, `AdapterSmoke.cpp`, and `bridge.go`; no system registration or IME settings were changed.
- Hardened `ITfTextInputProcessor::Activate`: null thread-manager input now returns `E_POINTER` before loading the Go core or mutating activation state; repeated activation while thread manager/key manager/session state is already active returns `E_UNEXPECTED`, avoiding accidental client-ID overwrite and duplicate resource acquisition. The client ID is assigned only after these precondition checks.
- Updated `AdapterSmoke.cpp` to explicitly load the Go core for direct ABI tests because the null-activation path now correctly exits before dependency loading. The smoke host intentionally keeps the Go runtime DLL loaded rather than attempting an unsupported unload/reload cycle.
- First build after the activation change exposed a stale smoke-test assumption (`GetModuleHandle` expected failed activation to have loaded the core); the test failed with Windows error 126. Corrected the harness to `LoadLibraryW(corePath)` and reran the complete validation successfully.
- Validation from the repository root / Windows amd64, Go 1.27.0: `native/golang/build-tip.ps1` PASS (x64 PE, four COM exports, `AdapterSmoke.exe` PASS); from `native/golang`, `go test ./...` PASS, `go test -race ./...` PASS, `go vet ./...` PASS; root `npm test` PASS (2 files, 15 tests); `npm run build` PASS; `git diff --check` PASS.
- Limits unchanged: the smoke test calls the Go ABI directly and only checks `Activate(nullptr)` rejection; it does not create a real TSF thread manager, prove successful `AdviseKeyEventSink`, or observe visible composition/commit in an application. No TIP registration, install, staging, or commit was performed. Existing unrelated dirty/untracked files were preserved.
- Next action: add a focused native test seam or obtain an explicitly approved real TSF host to verify successful activation/sink advising, then validate one printable key through actual TSF callback -> Go ABI -> valid edit session -> visible text. Keep Strategy B paused and do not claim the product is a usable IME until that end-to-end acceptance gate passes.

## New-context handoff — execute next from this plan (2026-10-09)

**Important routing decision:** this file documents Strategy B (pure-Go COM ABI research), but Strategy B is paused before `IClassFactory` because the Go DLL/callback lifetime and COM unload contract remain unproven. The active implementation path is Strategy C, `D:\\mcp-agent-workspace\\typescript-windows-ime\\native\\golang\\strategy-thin-cpp`. Do not restart Strategy B work unless new authoritative evidence resolves its lifetime blocker.

### Read first

1. This `PLAN.md` (especially the latest callback/lifetime findings and this handoff).
2. `D:\\mcp-agent-workspace\\typescript-windows-ime\\native\\golang\\strategy-thin-cpp\\PLAN.md`.
3. `D:\\mcp-agent-workspace\\typescript-windows-ime\\native\\golang\\adapter\\TsfGoAdapter.cpp` and `AdapterSmoke.cpp`.
4. `D:\\mcp-agent-workspace\\typescript-windows-ime\\native\\golang\\bridge.go` and `D:\\mcp-agent-workspace\\typescript-windows-ime\\native\\golang\\build-tip.ps1`.
5. Inspect `git status` before edits. Preserve all pre-existing modified and untracked files; do not reset, revert, stage, or commit.

### Latest verified Strategy C baseline

Run from `D:\\mcp-agent-workspace\\typescript-windows-ime`:

```powershell
.\\native\\golang\\build-tip.ps1
Push-Location native/golang
go test ./...
go test -race ./...
go vet ./...
Pop-Location
npm test
npm run build
git diff --check
```

All passed on Windows amd64 / Go 1.27.0 in the latest recorded run. Native smoke reported x64 PE, four COM exports, and `AdapterSmoke.exe` PASS. Root tests reported 15/15 passing. Re-run before further edits and record any difference.

### Next implementation task

The next milestone is **real successful TSF activation and visible text**, not more direct ABI-only testing:

1. Add a focused native test seam, or use a real TSF host only if explicitly approved; do not install/register the TIP as part of this task.
2. Verify successful `ITfTextInputProcessor::Activate`, `ITfKeystrokeMgr::AdviseKeyEventSink`, and matching `UnadviseKeyEventSink` during deactivation, including failure rollback and repeated activation/deactivation behavior.
3. Exercise one printable key through the actual TSF key callback -> Go ABI -> `RequestEditSession` -> composition/text update -> visible text in a real text control. Record observable evidence and exact host/app used.
4. Add automated regression coverage for activation preconditions and lifecycle rollback. The current smoke only verifies null-thread-manager rejection and directly invokes the Go ABI; it does **not** prove successful TSF activation or visible typing.
5. Re-run every baseline command above. Update both this file and `strategy-thin-cpp\\PLAN.md` with exact results, remaining limits, and the next handoff.

### Guardrails

- Do not run `native/install-user-golang.ps1`, register the TIP, or alter system IME settings without explicit user approval.
- Do not claim this is a usable IME until real TSF activation and visible composition/commit are demonstrated.
- Do not work on Strategy B's `IClassFactory` in parallel with Strategy C unless the user explicitly changes direction.
- No staging, commits, or destructive cleanup. Preserve the existing dirty/untracked worktree.

### New-context execution record — 2026-10-09 (Strategy C resumed)

- Followed the latest handoff: Strategy B remains paused before `IClassFactory`; active work continued only in `native/golang/strategy-thin-cpp`.
- Initial `git status --short --branch`: `## master...github/master`, ` M PLAN.md`, ` M README.md`, ` M native/cleanup-com.ps1`, `?? native/golang/`, `?? native/install-user-golang.ps1`. Preserved all state; no reset, clean, staging, or commit.
- Read this full plan and the Strategy C plan plus adapter/bridge/build files. Re-ran baseline with explicit absolute `Set-Location` after an initial wrong-directory attempt: `native/golang/build-tip.ps1` PASS; `go test ./...` PASS; `go test -race ./...` PASS; `go vet ./...` PASS; root `npm test` PASS (15/15); `npm run build` PASS; `git diff --check` PASS.
- Strategy C change: added an `ITfCompositionSink` implementation and passed it to `ITfContextComposition::StartComposition` instead of `nullptr`; termination callback clears the matching composition/context references. This follows the local reference implementation's sink pattern. Post-change full validation passed with the same results: `build-tip.ps1`, all three Go checks, root npm tests/build, and `git diff --check`.
- Important limit: fake `ITfThreadMgr` activation and direct ABI smoke do not establish real TSF host activation, `RequestEditSession` success, or visible text. No input method registration/installation or settings changes were made. No Git staging/commit occurred.
- Next: continue in Strategy C by adding a focused fake-context/edit-session seam for `RequestEditSession`, `StartComposition`/sink, text update, commit, and failure rollback; harden valid-session cleanup on deactivation/focus changes. A real text-field test requires explicit approval for any necessary registration and must not be attempted otherwise.
