# Strategy A: Go COM server using `go-com`

## Goal
Replace the C++ TSF adapter with a Go implementation of the TSF COM server, using a Go COM framework where its implementation model fits.

## Current status (2026-10-09)

**In progress — base COM activation and native negative-path/lifetime smoke tests pass. The prototype is not yet usable as an IME because real TSF activation, key events, edit sessions, and text commit remain unimplemented. Registration integration is allowed after the P0 transaction/rollback review.**

### Environment and dependency
- Windows x64; Go toolchain available.
- Local HTTP proxy `http://127.0.0.1:7897` works. Requests through it to `https://proxy.golang.org/` succeeded.
- Downloaded and pinned `github.com/zzl/go-com@v1.5.0` in this isolated strategy module; transitive module source is cached locally.
- `go test github.com/zzl/go-com/com/...` passed (packages compile; upstream packages have no tests).
- DLL build uses the available LLVM-MinGW toolchain. In PowerShell, set `$env:CGO_LDFLAGS='-lole32 -luuid'` before building.

### Files in this strategy directory
- `main.go`: Go COM DLL prototype and COM DLL entry points; Strategy A uses its isolated CLSID `{A4C7D921-6B35-4E8A-91F2-7D0B6C3E5A14}` and declares a minimal `ITfTextInputProcessor` vtable.
- `smoke.cpp`: native harness covering wrong CLSID, unsupported factory/object IID, aggregation rejection, null output pointers, and 100 object lifetimes with five Activate/Deactivate cycles each.
- `go.mod`, `go.sum`: pinned experimental module dependencies.
- `EXECUTION.md`: detailed execution log and acceptance checklist.

### Verified so far
- The Go DLL builds successfully with `go build -buildmode=c-shared -o TypeScriptWindowsImeGoCom.dll .` when `CGO_LDFLAGS=-lole32 -luuid` is set.
- PE export inspection confirms `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, and `DllUnregisterServer` exports.
- The existing Go module tests passed.
- The existing `native/golang/build-tip.ps1` baseline smoke test passed before this experiment; it exercises the existing C++ adapter, not the Go-native DLL.

### Current status / blocker
The earlier `E_NOINTERFACE` blocker was resolved by correcting the `ITfTextInputProcessor` IID and delegating interface dispatch to `go-com`. The rebuilt harness now passes negative-path checks (wrong CLSID, unsupported factory/object IID, aggregation rejection, and null output pointers) and 100 create/release iterations with five Activate/Deactivate cycles per object. This verifies only the prototype's direct COM smoke path, not activation by a real TSF host.

This strategy experiment has not registered or installed `TypeScriptWindowsImeGoCom.dll`. The registration exports are present, but `DllRegisterServer`/`DllUnregisterServer` still return `E_NOTIMPL`; complete registration logic and rollback tests are not implemented yet. **Read-only environment inspection found pre-existing machine-wide registration for a different Go CLSID**, pointing to `native\\install\\TypeScriptWindowsImeGo-20261009-122003044.dll`, plus an existing HKLM TSF TIP/profile tree. The existing C++ adapter is also registered to its own timestamped DLL. These pre-existing registrations were not changed. Do not infer that the current strategy DLL is registered or that the current host is disposable. Existing C++ adapter and unrelated pre-existing worktree changes have been left intact.

## Prioritized roadmap — source of truth (2026-10-09)

Follow these priorities in order. Do not start lower-priority work while a higher-priority prerequisite is open. Code implementation and non-mutating validation may proceed. Registration/install is not a blanket-prohibited activity; once the registration transaction and rollback are reviewed, any integration run must use the Strategy A CLSID/profile identity, be idempotent, and preserve unrelated or pre-existing registry/TSF metadata.

### P0 — Finish registration transaction correctness and tests (current task)

1. Introduce injectable seams around the **actual** COM registry and TSF profile/category operations used by production registration code; avoid a standalone test-only transaction model that production code does not call.
2. Add deterministic tests for every registration failure point, reverse-order rollback, preservation of the original failure HRESULT, reporting of cleanup failures, invalid/null/truncated module paths, and retry-safe unregistration when some of this strategy's metadata is already absent.
3. Prove preservation of pre-existing COM values and TSF-only/orphan metadata. Roll back only state positively created by this invocation; do not delete or unregister a category/profile merely because an operation failed.
4. Keep unit-test execution non-mutating by using mocks/fakes or a test harness. Separately, after reviewing the production registration path and rollback behavior, integration validation may invoke the registration exports for this strategy's isolated CLSID/profile; verify idempotence and preserve unrelated/pre-existing COM values and TSF-only/orphan metadata. Do not use `regsvr32` as a shortcut around the code path under test.
5. Run `gofmt`, `go test ./...`, `go vet ./...`, and `install.ps1 -BuildOnly`; retain native direct-COM smoke coverage. Explicitly label tests as compile/smoke-only where no assertions exercise registration behavior.
6. Review the diff and update `EXECUTION.md` with commands, results, gaps, and DLL hash. Enable `install.ps1 -Install` only when the production registration path and rollback have passed the P0 exit criteria and the script targets only this strategy's isolated identity.

**P0 exit criteria:** production-used seams and meaningful failure-injection tests exist; rollback order and HRESULT behavior are asserted; pre-existing metadata is preserved; partial-state unregistration is demonstrably retry-safe; safe build/smoke validation passes. Until all criteria pass, Task 0 is incomplete.

### P1 — Register on this workstation and observe TSF activation

1. Review the registration code and the exact Strategy A keys it creates before registering.
2. Register on this development workstation. A disposable VM, snapshot, or checkpoint is not required. Registration only adds or updates this strategy's own COM class and TSF profile metadata, and running it again must succeed without changing those results.
3. Use the Strategy A CLSID `{A4C7D921-6B35-4E8A-91F2-7D0B6C3E5A14}`, profile GUID `{E8B1C4D7-2A63-49F0-B5D8-1C7E3A9F6240}`, display name `strategy-go-com`, and the exact DLL path. Before and after, record only those keys so other COM classes and TIP trees can be shown unchanged.
4. Call the strategy registration export, confirm the Strategy A metadata, then unregister. A second register and a second unregister must both succeed. Rollback is that unregister, not a VM restore.

**P1 exit criteria:** the Strategy A registry metadata is created idempotently and removed idempotently, unrelated registrations are unchanged, and TSF discovery/activation of this DLL is observed when a local text host is available. Direct `GoComSmoke.exe` success does not by itself prove TSF activation.

### P2 — Implement and verify `ITfKeyEventSink` (blocked on P1)

Implement the SDK-verified interface and advise/unadvise lifecycle only after P1 passes. Add native QI/vtable/HRESULT/output-pointer tests, repeated activation tests, failed advise/unadvise rollback, and guards against callbacks after deactivation. Keep reducer integration out of this stage.

### P3 — Connect key events to the existing Go reducer/protocol (blocked on P2)

Route keystrokes into the existing Go reducer/protocol without rewriting the TypeScript engine or candidate dictionary. Verify focus changes, navigation, candidate selection, backspace, and escape before adding text insertion.

### P4 — Implement TSF edit-session text commit (blocked on P3)

Use the correct `ITfEditSession`/context request path; never mutate application text directly from a key callback. Test commit/cancel, stale contexts, request failures, and selection/caret behavior.

### P5 — End-to-end lifecycle hardening and acceptance (blocked on P1–P4)

Test real text-field input, COM apartment/thread assumptions, reference counts/unload behavior, repeated activation/deactivation, failed-activation rollback, and callback-after-deactivation prevention. Record evidence in `EXECUTION.md` and update acceptance checkboxes only when the corresponding behavior is actually verified.

### Task 1 — Review registration implementation, then validate real TSF activation

After Task 0 implementation and rollback tests are complete, review the idempotent registration plan. Registration and real TSF activation are separate milestones; direct COM smoke tests are not evidence of TSF activation. Neither milestone requires a disposable VM.

1. Inspect the existing TSF registration/profile and activation requirements in the Windows SDK and existing C++ adapter, read-only. Confirm the COM class/profile metadata and compare the new implementation with the SDK API contract.
2. Keep `install.ps1 -Install` blocked until code review and P0 transaction/rollback tests pass. After that, registration runs on this workstation and touches only the Strategy A identity.
3. Document the minimum integration experiment, expected observations, failure diagnostics, and unregister rollback. Registration tests must target only the Strategy A identity and must verify idempotence and preservation of unrelated metadata.
4. After the registration implementation and rollback pass review, register and unregister on this workstation. Capture the Strategy A keys and verify unregister removes only those keys.
5. Do not claim activation success from `GoComSmoke.exe`; direct COM calls do not prove TSF instantiation, activation, or correct COM apartment/thread behavior.

#### Task 1 findings and activation spike proposal (2026-10-09; review required)

**Superseded:** the disposable-VM requirement recorded below is withdrawn. Registration is an idempotent registry update for the Strategy A identity on this workstation, and unregister is the rollback. The machine-inspection notes remain historical context, not a block on that registry change.

Read-only inspection findings:
- The Go prototype's class ID is `{5D8C1D62-9F73-4B4C-A3D2-1E7B8A9C6401}`; the registered `HKCR\\CLSID\\{...}\\InprocServer32` default currently points to `native\\install\\TypeScriptWindowsImeGo-20261009-122003044.dll`, with `ThreadingModel=Apartment`. That path exists. It is not the strategy directory's `TypeScriptWindowsImeGoCom.dll`.
- The machine has an existing `HKLM\\SOFTWARE\\Microsoft\\CTF\\TIP\\{5D8C...}` tree, keyboard category metadata, language-profile subkeys for 0x0409, 0x0411, and 0x0804, and an `Enable=1` value at the TIP root. The profile GUID found in those keys is `{D47E2A19-6C85-4D31-9B70-3F2A8C5E6102}`.
- The existing C++ TIP has a separate CLSID `{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}`, separate registered timestamped DLL, and profile GUID `{C0A3B5B1-3C52-4E8E-A8A7-1F2B3D4C5E60}`.
- The existing `native\\install-user-golang.ps1` elevates, copies binaries into `native\\install`, runs 64-bit `regsvr32 /s`, writes HKLM COM class values, and writes HKCU profile Enable values. It is not an acceptable no-side-effect test path. The Go prototype's own `DllRegisterServer`/`DllUnregisterServer` are still `E_NOTIMPL`.
- The source C++ adapter registers the COM server with `ThreadingModel=Apartment`, calls `ITfInputProcessorProfiles::Register`, adds language profiles for 0x0409/0x0804/0x0411, and registers `GUID_TFCAT_TIP_KEYBOARD`. TSF activation therefore depends on registration/profile discovery as well as a working class factory; direct harness calls alone bypass that discovery/host path.

Registration experiment on this workstation:
1. Build `TypeScriptWindowsImeGoCom.dll` and record its path and SHA-256. Use only the Strategy A CLSID and profile GUID; do not reuse the existing Go or C++ TIP identities.
2. Record the Strategy A COM and TSF keys. They should be absent before the first registration. Do not export or modify unrelated CLSID or TIP trees.
3. Call this strategy's registration export. Confirm the COM class, `ThreadingModel=Apartment`, module path, language profiles `0x0409` / `0x0804` / `0x0411`, display name `strategy-go-com`, and keyboard category. Call it again and confirm the same metadata and a successful HRESULT.
4. Optionally launch a local 64-bit text host such as Notepad and record whether TSF loads this DLL and calls `Activate` / `Deactivate`. Typing remains out of scope until the key sink and edit session exist. `GoComSmoke.exe` does not prove that TSF loaded the DLL.
5. Call the unregister export. Confirm the Strategy A keys are gone and other input methods remain. Call unregister again and confirm it still succeeds.

Failure diagnostics to collect (do not “fix forward” by changing system settings):
- Profile absent: inspect the Strategy A TIP CLSID, profile GUID, language ID, keyboard category, and per-user Enable state; distinguish registration discovery from COM activation.
- COM class/load failure: capture HRESULT/Win32 error, COM class key and InprocServer32 path, file existence/hash/architecture, dependent DLL load errors, and host bitness/registry view.
- DLL loads but Activate is not observed: capture TSF/COM ETW or debugger breakpoints at `DllGetClassObject`, `IClassFactory::CreateInstance`, and `ITfTextInputProcessor::Activate`; verify interface IID/vtable and process/thread/apartment.
- Activate returns failure or host crashes/hangs: capture exception dump, HRESULT, stack, module path, callback arguments, and logs; do not add key-event functionality until base activation is explained.
- No text appears: this is expected with the current stub and is not an activation failure; do not interpret it as proof of broken class registration.

Rollback:
- Switch away from the Strategy A profile and close test hosts before unregistering. Do not unregister the existing Go or C++ TIP CLSIDs.
- Run this strategy's unregister export. It may remove only the Strategy A COM class, profile, category, and per-user Enable entries. Verify the other DLL paths and input methods are unchanged, then run unregister once more to confirm it is idempotent.
- If a Strategy A value remains, delete only that value and record the before/after keys. Do not restore a VM snapshot and do not restart ctfmon or change unrelated language settings.

**Review gate:** Task 1 no longer waits for a disposable VM. Registration may run on this workstation after the P0 transaction and rollback review, limited to the Strategy A identity. Keep Tasks 2–4 blocked until TSF activation of this DLL is observed.

### Installer scaffold and isolated identity (2026-10-09)

- Assigned Strategy A its own COM CLSID `{A4C7D921-6B35-4E8A-91F2-7D0B6C3E5A14}`, profile GUID `{E8B1C4D7-2A63-49F0-B5D8-1C7E3A9F6240}`, and display name `strategy-go-com`. Updated `main.go` and `smoke.cpp` to use the new CLSID; existing Go/C++ registrations were not touched.
- Added `install.ps1`. Default mode builds the local DLL, rebuilds the native smoke harness if `clang++` is available, and runs direct-COM smoke checks; `-BuildOnly` does the same without registration. `-Install` deliberately fails closed because `DllRegisterServer`/`DllUnregisterServer` still return `E_NOTIMPL` and real TSF activation is unverified. Do not bypass this gate with manual registry edits or `regsvr32`.
- The script is not yet a completed production installer: COM, profile, and keyboard-category registration and rollback are implemented and covered by non-mutating transaction tests. Enabling `-Install` still requires the idempotent Strategy A registry run on this workstation. A disposable VM is not required. The activation callback remains a stub and cannot input text.
- Validation: `install.ps1 -BuildOnly` passed after rebuilding both DLL and smoke harness; negative-path COM checks and 100 object lifetimes × 5 Activate/Deactivate cycles passed. No registry or input-method settings were changed.

### Task 2 — Implement the key event sink (blocked on activation-path review)

1. Only after the activation path and base interface behavior are established, implement `ITfKeyEventSink` using IID `AA80E7F5-2021-11D2-93E0-0060B067B86E` and the SDK-verified vtable order.
2. Acquire `ITfKeystrokeMgr` (`AA80E7F0-2021-11D2-93E0-0060B067B86E`) through the actual thread manager, advise the sink during activation, and unadvise during deactivation or failed-activation rollback.
3. Test each method's HRESULT and `BOOL*` handling, repeated activation, focus changes, failed advise/unadvise, and prevention of callbacks after deactivation. Add native QI/vtable tests before attempting a real client test.
4. Keep key-event behavior isolated from the TypeScript engine until the COM callback lifecycle is proven.

### Task 3 — Connect the reducer and implement text commit (blocked on Task 2)

1. Route key events into the existing Go reducer/protocol; do not rewrite the TypeScript engine or candidate dictionary.
2. Implement text insertion/commit only through the appropriate TSF edit-session/context request mechanism (`ITfEditSession`). Do not modify application text directly from a key callback.
3. Test navigation, candidate selection, backspace, escape, focus changes, commit/cancel, stale contexts, and request failures.

### Task 4 — Lifecycle hardening and acceptance (blocked on Tasks 1–3)

- Add diagnostics and tests for COM apartment/thread assumptions, reference counts, repeated activation/deactivation, failed-activation rollback, unload behavior, and callback-after-deactivation prevention.
- Verify in a real Windows text field that TSF activates the Go COM server without the C++ adapter and that typing, navigation, selection, backspace, escape, and commit work end to end.
- Update this plan and `EXECUTION.md` after each stage with commands, observed results, and any unresolved risks. Do not rerun already-passing smoke checks unless a change affects them.

### Safety boundaries

- Registration and TSF profile integration tests are permitted after review of the exact mutation scope and rollback. Use only Strategy A's isolated CLSID/profile identity; verify idempotence, preserve unrelated/pre-existing metadata, and confirm cleanup behavior. Do not use `regsvr32` to bypass the tested registration path.
- Keep `DllRegisterServer` and `DllUnregisterServer` returning `E_NOTIMPL` until registration/rollback is implemented, tested, and reviewed.
- Do not modify the existing C++ adapter or revert unrelated dirty worktree files.
- Generated DLLs and harnesses remain experimental artifacts until real TSF activation and end-to-end acceptance are verified.

### Acceptance criteria still outstanding

- [x] Native negative-path HRESULT/output-pointer smoke checks pass.
- [x] 100 object lifetimes × 5 Activate/Deactivate cycles pass in the direct COM harness.
- [x] ABI/framework review and output-slot hardening are documented; `go test ./...`, `go vet ./...`, native smoke, and x64/export inspection pass.
- [ ] Reviewed, safe real-TSF activation test plan exists.
- [ ] Real Windows TSF activation works without the C++ adapter.
- [ ] `ITfKeyEventSink` callbacks and advise/unadvise lifecycle work in a real TSF client.
- [ ] Typing, navigation, candidate selection, backspace, escape, focus changes, and text commit work in a real Windows text field.
- [ ] Failed activation rolls back cleanly; stale callbacks and post-deactivation object use are prevented.
- [ ] COM + TSF registration implementation and rollback are complete and code-reviewed; integration tests prove idempotence and preserve unrelated/pre-existing registry and TSF metadata.

### New-context handoff — next session (2026-10-09)

**Start here:** follow the **Prioritized roadmap — source of truth** above. Current work is P0: add production-used injectable seams and meaningful transaction tests. ABI/framework review and output-pointer fix are complete; do not repeat passing base smoke work unless a change breaks it. Registration integration can follow code review and successful P0 rollback tests, using only the isolated Strategy A identity and verifying baseline preservation.

#### Latest verified state
- `smoke.cpp` checks factory `IUnknown` QI, each TIP object's `IUnknown` QI, and the `IUnknown` → `ITfTextInputProcessor` query round-trip.
- ABI review completed against the installed Windows SDK `msctf.h` and pinned `github.com/zzl/go-com@v1.5.0`: `ITfTextInputProcessor` IID is `AA80E7F7-2021-11D2-93E0-0060B067B86E`, with `Activate(ITfThreadMgr*, TfClientId)` then `Deactivate()` after `IUnknown`; `ITfKeyEventSink` IID is `AA80E7F5-2021-11D2-93E0-0060B067B86E`, with `OnSetFocus`, `OnTestKeyDown`, `OnTestKeyUp`, `OnKeyDown`, `OnKeyUp`, `OnPreservedKey` in that order. `AA80E7F0` is `ITfKeystrokeMgr`, not the event sink.
- The Go TIP vtable order matches the SDK, but its callbacks intentionally ignore the activation arguments and return `S_OK`; this is only a direct-call smoke stub, not proof that TSF can activate it.
- Hardened failure behavior: class factory `CreateInstance` clears output and guards a null IID; `DllGetClassObject` clears output before validating CLSID/IID. The smoke harness now starts unsupported factory-IID output with a non-null sentinel and asserts it is cleared.
- Latest rebuild passed: `clang++ -std=c++17 -municode .\smoke.cpp -o .\GoComSmoke.exe -lole32 -luuid`; `$env:CGO_LDFLAGS='-lole32 -luuid'; go build -buildmode=c-shared -o .\TypeScriptWindowsImeGoCom.dll .`; then run the harness against the DLL.
- Harness passed all negative-path checks and 100 object lifetimes × 5 Activate/Deactivate cycles. `go test ./...` and `go vet ./...` passed; this module currently has no Go test files. `llvm-readobj --file-headers --coff-exports .\TypeScriptWindowsImeGoCom.dll` confirms x64/AMD64 and all four expected exports.
- Latest detailed results are in `EXECUTION.md`. No system registration/install or IME configuration change was performed.

#### Current handoff
The ordered work, dependencies, and exit criteria are maintained in the **Prioritized roadmap — source of truth** above. Do not maintain a second task ordering here. At handoff, report the current P0 subtask, validation performed, and any blockers; only mark P0 complete when its listed exit criteria are satisfied.

#### Suggested first commands
```powershell
Set-Location D:\mcp-agent-workspace\typescript-windows-ime\native\golang\strategy-go-com
Get-Content .\PLAN.md
Get-Content .\EXECUTION.md
Get-Content .\main.go
Get-Content .\smoke.cpp
$env:CGO_LDFLAGS='-lole32 -luuid'
go build -buildmode=c-shared -o .\TypeScriptWindowsImeGoCom.dll .
.\GoComSmoke.exe (Join-Path (Get-Location) 'TypeScriptWindowsImeGoCom.dll')
go test ./...
go vet ./...
```

#### Do not do in the next session
- Do not run `regsvr32`, register/install the experimental DLL, or modify system IME settings.
- Do not change the existing C++ adapter or revert unrelated dirty files.
- Do not treat direct COM smoke success as proof of real TSF activation.
- Do not begin key-event or text-commit implementation until the interface ABI and activation path are established.

## Goal and architecture
- Evaluate `github.com/zzl/go-com` for COM objects, interface query, class factories, and callbacks from Go.
- Implement `ITfTextInputProcessor` and `ITfKeyEventSink` in Go.
- Keep key reduction, composition state, candidate selection, and commit decisions in Go.
- Reuse the existing TypeScript host/protocol where practical; do not add a C++ forwarding layer unless an ABI limitation requires it.

## Work packages
1. Inspect TSF registration, CLSID, threading model, activation/deactivation, and DLL export requirements. (Initial inspection done; revisit after QI works.)
2. Build a minimal Go COM DLL prototype that Windows can load and activate. (DLL builds and exports are verified; COM activation is blocked on QI.)
3. Implement and register `ITfKeyEventSink`; verify key-down and key-up callbacks in a real TSF client. (Not started.)
4. Connect the event sink to the Go key reducer and existing candidate/protocol flow. (Not started.)
5. Implement text commit through the appropriate TSF edit-session mechanism. (Not started.)
6. Complete and review COM + TSF registration/rollback first; then register idempotently on this workstation, implement key events and text commit, and add lifecycle/focus/error tests. (Non-mutating registration tests exist; live registry idempotence, activation, and input behavior are unverified.)

## Acceptance criteria
- [ ] Windows activates the Go COM server without the existing C++ adapter.
- [ ] TSF can query and invoke required interfaces with correct HRESULTs and lifetime behavior.
- [ ] Typing, navigation, candidate selection, backspace, escape, and commit work in a real Windows text field.
- [ ] No Go callback or COM object is used after deactivation; failures are diagnosable.
- [ ] COM + TSF registration/install implementation and rollback are complete and reviewed before any mutating activation spike.

## Main risks
- The framework may not cover all TSF interface/vtable and COM server requirements.
- DLL exports, Go runtime initialization, callback ABI, and COM apartment/thread rules need proof on the target Windows architecture.
- Unit tests alone are insufficient; real TSF activation is required.
- The current `E_NOINTERFACE` failure may be due to IID dispatch, object/interface pointer layout, or framework QI conventions; determine which with source inspection and focused tests before adding more interfaces.

## Scope boundaries
- Do not rewrite the TypeScript engine or candidate dictionary during the first prototype.
- Do not change the current adapter until the activation spike passes.
- Keep experimental code isolated in this strategy directory or a dedicated Go module.
- Do not register the prototype or modify system IME state while COM activation tests fail.

## First experiment
First implement and test the complete, reversible COM + TSF registration path on this workstation. No disposable VM or checkpoint is required. Any integration run must be idempotent, scoped to Strategy A's isolated identity, and include verification that unrelated registrations and TSF metadata remain unchanged. Rollback is unregistering that identity. Stop and reassess if this cannot be done without an unmanaged shim.

### Execution update — registration rollback review (2026-10-09)

- Reviewed the current registration failure path and removed the unconditional outer TSF cleanup after a failed registration attempt. Profile rollback now runs locally only after `ITfInputProcessorProfiles::Register` succeeded and a later language-profile/category step failed; the outer rollback deletes only the exact CLSID subtree that was confirmed absent before creation.
- `go test ./...`, `go vet ./...`, and `install.ps1 -BuildOnly` passed after the change. The native non-mutating smoke harness still passes its negative-path checks and 100 object lifetimes × 5 Activate/Deactivate cycles.
- Fixed another COM registry transaction gap: failure to write the first/default CLSID value now flows through exact-subtree rollback (the helper may have created the key before the value write failed). A truncated module path now returns `ERROR_INSUFFICIENT_BUFFER` deterministically.
- Revalidated with `go test ./...`, `go vet ./...`, and `install.ps1 -BuildOnly`; the direct-COM negative-path and 100 × 5 lifecycle smoke checks passed. DLL SHA-256: `78C7656E514995964011082B75E99771479CA91AFEDA8AA6C0F73ECE978A261A`.
- Task 0 is **not complete**: injectable failure-step seams/tests and targeted validation of pre-existing TSF metadata preservation and rollback HRESULT handling remain outstanding. `install.ps1 -Install` remains blocked. No registration export, `regsvr32`, registry/profile mutation, or input-method change was performed.

### Registration code review follow-up — current session (2026-10-09)

- Re-read `main.go`, `install.ps1`, and `EXECUTION.md`, and checked the worktree before proceeding. Existing unrelated modifications in root `PLAN.md`, `README.md`, and `native/cleanup-com.ps1` were left untouched.
- Read-only assertions passed for the isolated Strategy A CLSID/profile identity, module-path lookup, `ThreadingModel=Apartment`, language IDs 0x0409/0x0804/0x0411, keyboard category registration, refusing an existing COM CLSID subtree, and the fail-closed `-Install` gate.
- Identified three unresolved transaction issues that must be addressed before Task 0 can be reviewed as complete: (1) TSF operations are hard-coded, so tests cannot inject a failure at each actual registration step; (2) rollback paths ignore some cleanup HRESULTs, so incomplete rollback is not surfaced; (3) unregistration's behavior when TSF metadata is already partially absent has not been shown to be idempotent and retry-safe.
- Re-ran `go test ./...` and `go vet ./...`; both exited successfully, but the module currently has no Go test files, so this is compile/static validation rather than transaction test coverage.
- No registration exports, `regsvr32`, registry writes, TSF profile operations, or input-method settings changes were performed. The next implementation step is to introduce injectable registration operations around the actual COM/TSF calls and test step ordering, original-HRESULT preservation, cleanup failures, and retry behavior without invoking mutating exports on this workstation.

### Rollback HRESULT diagnostics follow-up — current session (2026-10-09)

- Updated `StrategyRegisterProfiles` to emit `OutputDebugStringW` diagnostics when profile rollback manager creation or `ITfInputProcessorProfiles::Unregister` fails. Diagnostics include both the original registration HRESULT and the cleanup HRESULT; the original registration failure remains the function's return value.
- Preserved TSF-only orphan metadata conservatively: a failed `RegisterCategory` call does not prove this invocation created the category, so the code does not blindly unregister that category. Profile rollback remains limited to the profile class this invocation successfully registered.
- Validation passed after the change: `gofmt`, `go test ./...`, `go vet ./...`, and `install.ps1 -BuildOnly`; native direct-COM smoke passed negative HRESULT/output-pointer checks and 100 object lifetimes × 5 Activate/Deactivate cycles. DLL SHA-256: `1C8A28DE36C273821484D10CB83F924B0971455C4ABBA42A38D7A2FF94247120`.
- Task 0 remains incomplete because no injectable failure-step tests exist yet and partial-TSF-metadata retry behavior is unproven. No registration export or system mutation was performed; `-Install` remains blocked.

### Current continuation — COM rollback diagnostics (2026-10-09)

- Added diagnostics for failure to delete the exact Strategy A COM CLSID subtree during registration rollback. The cleanup HRESULT is logged alongside the original registration HRESULT; the original HRESULT remains the API return value.
- `gofmt`, `go test ./...`, `go vet ./...`, and `install.ps1 -BuildOnly` passed. Native direct-COM smoke passed negative HRESULT/output-pointer checks and 100 object lifetimes × 5 Activate/Deactivate cycles. DLL SHA-256: `6145833E121913E77200C5545DE572A621DC9626D4D7E52E04407ABD12817AC2`.
- This does not close P0: actual production-used injectable operation seams and deterministic transaction tests are still missing; `go test` reports `[no test files]`. Continue with P0 only. Do not invoke registration exports, `regsvr32`, or mutate system registry/TSF state on this workstation; `install.ps1 -Install` remains blocked.

### New-context handoff — latest continuation (2026-10-09)

**Resume at P0; do not restart the roadmap or repeat baseline checks without a code change.** This continuation did not modify `main.go` or add tests. It only reran the safe baseline checks and confirmed the P0 gap remains open.

- `go test ./...`: exit 0, output `? strategy-go-com-experiment [no test files]` — no transaction assertions exist yet.
- `go vet ./...`: exit 0.
- `install.ps1 -BuildOnly`: exit 0; rebuilt the DLL and native harness. Smoke output: `PASS: negative HRESULT/output-pointer cases; 100 object lifetimes x 5 Activate/Deactivate cycles`.
- DLL SHA-256 from this build: `0386FEB818AF70A3EB6FF6A6F5B9173D6AD0A7D15BBF2A2A29660BB84676B3DB`.
- No registration, `regsvr32`, registry/TSF mutation, or input-method setting change was performed. `install.ps1 -Install` must remain blocked.

#### First implementation task in the next context
1. Read `main.go`, `smoke.cpp`, `install.ps1`, and the latest `EXECUTION.md` before editing; inspect `git status` and preserve unrelated dirty files.
2. Refactor the **production registration path itself** to use injectable operations for registry writes/deletes and TSF profile/category calls. Prefer a small operation table/function-pointer seam that the real code calls; tests must invoke the same transaction coordinator as production, not a duplicated model.
3. Add a non-mutating native test harness or other deterministic test target with fake operations. Assert each failure point, reverse-order cleanup, preservation of the original HRESULT when cleanup also fails, explicit cleanup-failure diagnostics, refusal to overwrite pre-existing COM state, preservation of TSF-only/orphan metadata, and safe retries after partial unregistration. Cover null/invalid/truncated module paths.
4. Run focused transaction tests first, then `gofmt`, `go test ./...`, `go vet ./...`, and `install.ps1 -BuildOnly`. Record exact commands/results and the new DLL hash in `EXECUTION.md` and summarize them here. Clearly distinguish transaction assertions from direct-COM smoke coverage.
5. Review the full diff and update P0 status only if every P0 exit criterion in the roadmap is evidenced. Otherwise document the remaining failing/missing cases and keep the install gate closed.

**Hard boundary:** do not run `DllRegisterServer`/`DllUnregisterServer`, `regsvr32`, or any system registry/TSF/input-method mutation on this workstation. Do not implement key-event sink or text commit while P0 is open; real TSF activation remains a separate gated milestone.

### New-context handoff — registration transaction coordinator (2026-10-09)

**Resume at P0.** This continuation added a production-used transaction coordinator and an initial deterministic native test, but P0 is still open. Read the latest `EXECUTION.md` and inspect the current files/status before further edits; do not rely on older handoff notes above where they say no test seam exists.

#### Changes now present
- `registration_txn.h`: `StrategyRunRegisterTransaction` coordinates class probing, three COM registry value writes, TSF profile registration, and exact Strategy A CLSID subtree rollback. Production registration calls this coordinator through callbacks.
- `main.go`: `StrategyRegisterAll` now delegates to the shared coordinator; the production callbacks use the isolated Strategy A CLSID/profile identity and report COM subtree cleanup failure while preserving the original HRESULT.
- `registration_txn_test.cpp`: deterministic fake-operation harness exercises failure injection at each of the five transaction stages, verifies rollback invocation after mutation-stage failures, checks original HRESULT preservation and cleanup-failure diagnostics, refuses a simulated pre-existing CLSID without rollback, and verifies successful completion without cleanup.
- `EXECUTION.md`: latest execution details and current gaps appended.

#### Latest validation observed
- `gofmt -w main.go`: exit 0.
- `go test ./...`: exit 0, reports `[no test files]`; this is compile validation, not registration test coverage.
- `go vet ./...`: exit 0.
- `install.ps1 -BuildOnly`: exit 0. Native direct-COM smoke passed negative HRESULT/output-pointer cases and 100 object lifetimes × 5 Activate/Deactivate cycles.
- Compiled and ran `registration_txn_test.cpp` with `clang++ -std=c++17`; output: `PASS: registration transaction failure injection, rollback, and HRESULT preservation`.
- DLL SHA-256: `B77F479BA8085B992A4B00A760106D2644FB5E6581EE20AE612E6DD98399175F`.
- The cgo build reports C23-extension warnings for unnamed callback parameters; build and tests still exit successfully.

#### P0 gaps that remain — do not mark complete
1. The transaction test currently checks that rollback is invoked and is the last call, but does not fully assert a specified reverse-order cleanup sequence for independently created COM and TSF artifacts.
2. TSF profile/category work is still represented as a single high-level callback. Individual actual `ITfInputProcessorProfiles` and `ITfCategoryMgr` calls are not injectable/tested independently.
3. Preservation of TSF-only/orphan metadata and cleanup behavior after failures at each TSF sub-step are not proven.
4. Null/invalid/truncated module path cases are not yet covered by focused tests. Confirm the current production path validation and add deterministic assertions.
5. Retry-safe unregistration for partially absent TSF metadata remains unproven.
6. Review the test harness carefully: fake tests are evidence for the shared transaction coordinator's decisions, not evidence that every Win32/TSF side effect has been integration-tested.

#### Next actions
1. Inspect `main.go`, `registration_txn.h`, `registration_txn_test.cpp`, `install.ps1`, and `EXECUTION.md`; inspect git status and preserve unrelated dirty files.
2. Extend production-used injection seams down to the actual TSF profile/category operations, tracking which state this invocation successfully created. Never remove a pre-existing category/profile or TSF-only/orphan metadata based solely on a failed HRESULT.
3. Add tests for cleanup ordering, every TSF sub-step failure, original HRESULT preservation when multiple cleanup operations fail, cleanup diagnostic contents, existing COM/TSF metadata preservation, null/invalid/truncated module paths, and repeated unregistration after partial cleanup.
4. Run the focused native test first, then `gofmt`, `go test ./...`, `go vet ./...`, and `install.ps1 -BuildOnly`; update `EXECUTION.md` and this plan with exact results and DLL hash.
5. Only close P0 when every roadmap exit criterion is evidenced. Keep `install.ps1 -Install` blocked until the registration implementation and rollback are fully reviewed.

**Safety boundary recorded for that session:** no registration exports, `regsvr32`, or system registry/TSF/input-method mutation had been performed. The disposable-VM requirement in this paragraph is superseded by the workstation registration policy in the prioritized roadmap.
### New-context handoff — P0 path-validation continuation (2026-10-09)

- Production module-path validation now uses the same helper as the native tests; focused tests cover null, empty, zero-capacity, valid, and truncated path cases, plus registration stage order and rollback triggering.
- Latest safe validation passed: native `RegistrationTxnTest.exe`, `gofmt -w main.go`, `go test ./...` (no Go test files), `go vet ./...`, and `install.ps1 -BuildOnly`; direct COM smoke passed. DLL SHA-256: `B9500351D071A423333B623A63CDC8218AED02583C388CBB1068F1E66457AE07`.
- Continue P0 only. Next priority: replace the high-level TSF callback with production-used injectable seams for each actual `ITfInputProcessorProfiles` and `ITfCategoryMgr` operation, tracking only metadata positively created by this invocation. Add failure injection for every TSF sub-step, reverse-order cleanup assertions, preservation of original HRESULT and cleanup diagnostics, orphan-metadata preservation, and retry-safe partial unregistration tests.
- Keep `install.ps1 -Install` blocked. Do not call registration exports, `regsvr32`, or mutate registry/TSF/input-method state on this workstation. Do not start key-event or text-commit work while P0 remains open.

### P0 non-mutating transaction criteria met (2026-10-09)

`registration_txn.h` is the production coordinator. `main.go` supplies one callback per COM write and per `Register`, `AddLanguageProfile`, `EnableLanguageProfile`, `RegisterCategory`, and the matching unregister/delete calls. `registration_txn_test.cpp` drives that coordinator with fakes: every failure step, reverse-order cleanup, original HRESULT plus cleanup diagnostics, orphan preservation, path rejection, and retry-safe unregistration of already-absent category, profile, and COM metadata. Both harness runs and both `install.ps1 -BuildOnly` runs passed. Details and both DLL SHA-256 values are in `EXECUTION.md`. The on-disk DLL hash is `4AE67DB7BAD2EBCD6880AB0A309995E4F978AC15DED0145641B0B90943457494`.

This does not by itself enable `install.ps1 -Install`: live idempotence is still unproven. The next registration run is an idempotent Strategy A registry update on this workstation, followed by unregister as rollback. No disposable VM is required. Do not register any other CLSID or TIP. P1 activation, key-event, and text-commit work remain blocked until that registration is observed.
