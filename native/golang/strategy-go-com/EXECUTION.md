# Strategy A execution log: Go COM server using go-com

## Current result (2026-10-09)

Status: **framework downloaded; DLL builds; base COM activation smoke test passes after correcting the interface IID. Real TSF integration remains untested.**

### Proxy and dependency

- Local proxy `http://127.0.0.1:7897` is listening and works.
- `curl.exe --proxy http://127.0.0.1:7897 -I https://proxy.golang.org/` returned HTTP 200.
- `go list -m -versions github.com/zzl/go-com` succeeded and reported versions through v1.5.0.
- Pinned `github.com/zzl/go-com@v1.5.0` and its dependencies were downloaded into the Go module cache.
- `go test github.com/zzl/go-com/com/...` passed (packages compile; no package tests are defined).

### Isolated prototype added

- `main.go`: Go shared-DLL entry points using go-com's class-factory object model, a minimal `ITfTextInputProcessor` interface/vtable, and deliberately disabled registration entry points.
- `smoke.cpp`: native harness that loads the DLL and calls `DllGetClassObject`, `IClassFactory::CreateInstance`, `Activate`, `Deactivate`, and `Release`.
- `go.mod` / `go.sum`: pinned experiment dependency metadata.
- Build command requires `CGO_LDFLAGS=-lole32 -luuid` with the available LLVM-MinGW toolchain.

### Verified

- `go build -buildmode=c-shared -o TypeScriptWindowsImeGoCom.dll .` succeeds when `CGO_LDFLAGS=-lole32 -luuid` is set.
- PE export inspection confirms `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, and `DllUnregisterServer` are exported.
- Existing main Go module tests and existing C++ adapter smoke test passed earlier as baseline checks.

### Fix and verification (2026-10-09)

- The failing IID logged by the native harness was `AA80E7F7-2021-11D2-93E0-0060B067B86E`; the prototype had incorrectly declared `AA80E801-2021-11D2-93E0-0060B067B86E`.
- Corrected the IID and changed the implementation to delegate QueryInterface dispatch and pointer assignment to `go-com`'s `IUnknownImpl.QueryInterface` contract, retaining null-output initialization and `E_POINTER` handling.
- Rebuilt successfully with `CGO_LDFLAGS=-lole32 -luuid`.
- Ran `GoComSmoke.exe TypeScriptWindowsImeGoCom.dll` ten consecutive times. Every run passed `CreateInstance`, `Activate`, `Deactivate`, and `Release`.
- `go test ./...` passed (the strategy module currently has no test files).
- `dumpbin.exe` was not available on PATH. Verified the PE export table with `llvm-readobj --coff-exports`; it lists `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, and `DllUnregisterServer`. Registration exports remain intentionally disabled (`E_NOTIMPL`).

### Native negative-path and lifecycle coverage (2026-10-09)

- Expanded `smoke.cpp` to assert `CLASS_E_CLASSNOTAVAILABLE` for a wrong CLSID; `E_POINTER` for null output pointers; `E_NOINTERFACE` for unsupported factory/object IIDs; and `CLASS_E_NOAGGREGATION` for aggregation attempts. Failed calls are checked for null output pointers where applicable.
- Rebuilt the harness with `clang++ -std=c++17 -municode smoke.cpp -o GoComSmoke.exe -lole32 -luuid` and rebuilt the DLL with `CGO_LDFLAGS='-lole32 -luuid'`.
- Smoke test passed: 100 object creation/release iterations, each with five `Activate`/`Deactivate` cycles (500 lifecycle cycles total), in addition to negative-path checks.
- `go test ./...` passed (strategy module has no test files).
- `llvm-readobj --coff-exports TypeScriptWindowsImeGoCom.dll` confirmed x86-64 PE and all four expected COM exports: `DllCanUnloadNow`, `DllGetClassObject`, `DllRegisterServer`, `DllUnregisterServer`.

### COM ABI review and failure-path hardening (2026-10-09)

- Reviewed the installed Windows SDK `msctf.h` declarations and the pinned `github.com/zzl/go-com@v1.5.0` source (`com/impl.go`, `com/comimpl/IClassFactory.go`).
- Confirmed `ITfTextInputProcessor` IID `AA80E7F7-2021-11D2-93E0-0060B067B86E`; vtable methods after `IUnknown` are `Activate(ITfThreadMgr*, TfClientId)` then `Deactivate()`, both `STDMETHODCALLTYPE`/HRESULT. The current prototype vtable order matches, but its callback deliberately ignores the actual activation arguments and is only a smoke stub.
- Confirmed `ITfKeyEventSink` IID `AA80E7F5-2021-11D2-93E0-0060B067B86E`; method order after `IUnknown`: `OnSetFocus(BOOL)`, `OnTestKeyDown(ITfContext*, WPARAM, LPARAM, BOOL*)`, `OnTestKeyUp(...)`, `OnKeyDown(...)`, `OnKeyUp(...)`, `OnPreservedKey(ITfContext*, REFGUID, BOOL*)`. No key sink has been implemented yet. Note: `AA80E7F0` is `ITfKeystrokeMgr`, not `ITfKeyEventSink`.
- `go-com`'s generic `IUnknownImpl.QueryInterface` dereferences the IID pointer and does not itself initialize the output slot before returning `E_NOINTERFACE`; the TIP override already guards null IID and clears output, but the class-factory path needed explicit protection.
- Hardened `classFactoryImpl.CreateInstance` to return `E_POINTER` for a null IID after clearing a valid output slot. Hardened `DllGetClassObject` to clear a valid output slot before validating CLSID/IID, and to return `E_POINTER` for null CLSID/IID.
- Updated `smoke.cpp` to initialize the unsupported-factory-IID output with a non-null sentinel, proving the failure path clears it rather than passing accidentally because the slot began null.
- Rebuilt the harness and DLL. Native smoke passed: negative HRESULT/output checks plus 100 object lifetimes × 5 Activate/Deactivate cycles. `go test ./...` and `go vet ./...` passed. `llvm-readobj --file-headers --coff-exports` confirmed x86-64 PE and the four expected exports.
- No registration, installation, TSF profile change, or system IME configuration change was performed. The DLL and harness are generated experimental build outputs only.

### COM ABI review and failure-path hardening (2026-10-09)

- Reviewed the installed Windows SDK `msctf.h` declarations and the pinned `github.com/zzl/go-com@v1.5.0` source (`com/impl.go`, `com/comimpl/IClassFactory.go`).
- Confirmed `ITfTextInputProcessor` IID `AA80E7F7-2021-11D2-93E0-0060B067B86E`; vtable methods after `IUnknown` are `Activate(ITfThreadMgr*, TfClientId)` then `Deactivate()`, both `STDMETHODCALLTYPE`/HRESULT. The current prototype vtable order matches, but its callback deliberately ignores the actual activation arguments and is only a smoke stub.
- Confirmed `ITfKeyEventSink` IID `AA80E7F5-2021-11D2-93E0-0060B067B86E`; method order after `IUnknown`: `OnSetFocus(BOOL)`, `OnTestKeyDown(ITfContext*, WPARAM, LPARAM, BOOL*)`, `OnTestKeyUp(...)`, `OnKeyDown(...)`, `OnKeyUp(...)`, `OnPreservedKey(ITfContext*, REFGUID, BOOL*)`. No key sink has been implemented yet. Note: `AA80E7F0` is `ITfKeystrokeMgr`, not `ITfKeyEventSink`.
- `go-com`'s generic `IUnknownImpl.QueryInterface` dereferences the IID pointer and does not itself initialize the output slot before returning `E_NOINTERFACE`; the TIP override already guards null IID and clears output, but the class-factory path needed explicit protection.
- Hardened `classFactoryImpl.CreateInstance` to return `E_POINTER` for a null IID after clearing a valid output slot. Hardened `DllGetClassObject` to clear a valid output slot before validating CLSID/IID, and to return `E_POINTER` for null CLSID/IID.
- Updated `smoke.cpp` to initialize the unsupported-factory-IID output with a non-null sentinel, proving the failure path clears it rather than passing accidentally because the slot began null.
- Rebuilt the harness and DLL. Native smoke passed: negative HRESULT/output checks plus 100 object lifetimes × 5 Activate/Deactivate cycles. `go test ./...` and `go vet ./...` passed. `llvm-readobj --file-headers --coff-exports` confirmed x86-64 PE and the four expected exports.
- No registration, installation, TSF profile change, or system IME configuration change was performed. The DLL and harness are generated experimental build outputs only.

### Remaining work / safety boundary

- Broader COM reference-count/threading tests and real TSF host activation remain outstanding.
- `ITfKeyEventSink`, edit sessions, composition, text commit, and real text-field typing behavior remain unimplemented/unverified.
- Do not register or use the prototype as an IME. No system registration was performed and the existing adapter was not modified.

### Re-verification and COM identity check (2026-10-09)

- Reviewed the tracked worktree diff before changing the isolated experiment; pre-existing edits in root `PLAN.md`, `README.md`, and `native/cleanup-com.ps1` were left intact.
- Extended `smoke.cpp` to verify `IClassFactory::QueryInterface(IID_IUnknown)` and each TIP object's `QueryInterface(IID_IUnknown)` plus an `IUnknown` → `ITfTextInputProcessor` query round trip.
- Rebuilt the native harness and Go DLL; the expanded smoke suite passed, including both identity checks, all negative-path HRESULT/output assertions, and 100 object lifetimes × 5 Activate/Deactivate cycles.
- `go test ./...` and `go vet ./...` passed (no Go test files in this strategy module); `llvm-readobj --file-headers --coff-exports` confirmed x86-64 and all four expected COM exports.
- This still validates only direct COM calls from the native harness. Real TSF-host activation, `ITfKeyEventSink`, edit sessions, composition, and text commit remain unimplemented/unverified. Registration remains disabled and was not attempted.

### Real TSF activation path review — read-only stage (2026-10-09)

Status: **proposal documented; stopped at the registration/activation authorization gate.** No registry writes, registration/unregistration, install script, TSF profile change, input-method change, or real TSF activation attempt was performed.

Read-only inspection performed:
- Read `PLAN.md` handoff and this execution log first, then reviewed `main.go`, the existing C++ adapter's registration code in `native/TsIme.cpp`, and `native/install-user-golang.ps1` / `native/install-user.ps1`.
- Checked Windows computer metadata: Lenovo 21CX, Windows 10.0.26200 x64; `HypervisorPresent=True` is not proof of a disposable VM or restorable snapshot. No verified disposable VM/checkpoint was available from the current environment evidence. The current host is not approved for the spike.
- Read-only registry checks found the Go CLSID `{5D8C1D62-9F73-4B4C-A3D2-1E7B8A9C6401}` already mapped to `D:\\mcp-agent-workspace\\typescript-windows-ime\\native\\install\\TypeScriptWindowsImeGo-20261009-122003044.dll` with `ThreadingModel=Apartment`; that DLL exists. This is not the isolated strategy DLL `TypeScriptWindowsImeGoCom.dll`.
- The existing Go CLSID has an HKLM `SOFTWARE\\Microsoft\\CTF\\TIP` tree, keyboard category metadata, profile `{D47E2A19-6C85-4D31-9B70-3F2A8C5E6102}` subkeys for language IDs 0x0409, 0x0411, and 0x0804, and an `Enable=1` value at the TIP root. The existing C++ TIP has its own CLSID `{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}`, registered timestamped DLL, and profile `{C0A3B5B1-3C52-4E8E-A8A7-1F2B3D4C5E60}`. These existing entries were not changed.
- `install-user-golang.ps1` elevates, copies the built DLL to a timestamped install path, calls 64-bit `regsvr32 /s`, writes HKLM COM class values, and writes HKCU language-profile Enable values. It is not a read-only path. The experimental strategy DLL still returns `E_NOTIMPL` from both registration exports.
- The C++ adapter's registration code uses `ITfInputProcessorProfiles::Register`, `AddLanguageProfile` for 0x0409/0x0804/0x0411, and `ITfCategoryMgr::RegisterCategory(..., GUID_TFCAT_TIP_KEYBOARD, ...)`. TSF discovery requires registration/profile metadata; calling the class factory directly bypasses that integration.

Proposal recorded in `PLAN.md`:
- Run only in a dedicated Windows x64 VM with a verified clean snapshot and console recovery, after separate review/authorization. Prefer a unique test CLSID/profile GUID to avoid collision with the existing registered Go TIP. Do not use this workstation or assume per-user registration is harmless.
- Capture baseline registry exports, profile/input-method state, DLL paths/hashes, Windows build/architecture, and checkpoint before mutation.
- After approval only, use a 64-bit TSF-aware text host and collect the exact loaded module path/hash, process bitness, profile selection, TSF-originated `Activate` arguments/HRESULT, `Deactivate` observation, debugger/ETW evidence, logs, and screenshots.
- Pass only if TSF itself discovers the intended profile, loads the exact test DLL, calls `ITfTextInputProcessor::Activate` with non-null thread manager and a plausible client ID, receives `S_OK`, and deactivates cleanly. No typing/text-commit claim is in scope for the current stub.
- Failure diagnostics distinguish profile discovery, COM class/DLL loading, missing activation callback, and activation crash/failure. No ad-hoc configuration changes to “fix” a failure.
- Rollback prefers restoring the entire VM checkpoint. Any in-guest cleanup must be separately approved and limited to exact test-only CLSID/profile/category/Enable entries; preserve pre-existing values, never unregister the existing Go/C++ TIPs, and verify DLL paths/hashes and input-method state after cleanup.

Tests/validation for this documentation-only stage:
- No build or native smoke tests rerun because no source code or binary changed and prior direct-COM tests already pass; avoid duplicating those checks.
- Read-only registry inspection and source review completed; the evidence above was recorded in `PLAN.md` and this log.
- Focused PowerShell documentation assertions passed (8/8): proposal heading, activation pass criteria, failure diagnostics, rollback, review gate, execution entry, no-mutation statement, and checkpoint rollback are all present. No build or native smoke tests were rerun because this stage changed documentation only.
- Git status review confirmed the pre-existing root `PLAN.md`, `README.md`, `native/cleanup-com.ps1`, `native/install-user-golang.ps1`, and `native/golang/` worktree entries remain present; no unrelated files were reverted. The strategy directory is untracked as part of the pre-existing workspace state, so the standard tracked-file diff does not show its file contents.
- The next action is human review/authorization of the VM-only proposal, not registration or key-event implementation.

### Strategy A fresh identity and installer scaffold (2026-10-09)

- Added the unique Strategy A display name `TypeScript Windows IME Go Strategy A`, CLSID `{A4C7D921-6B35-4E8A-91F2-7D0B6C3E5A14}`, and profile GUID `{E8B1C4D7-2A63-49F0-B5D8-1C7E3A9F6240}`. Updated the Go COM class identity and native smoke harness together; did not modify or unregister either existing Go/C++ TIP.
- Added `install.ps1`. Default mode is non-registering build/validation; `-BuildOnly` is an explicit no-registration mode. `-Install` is fail-closed and reports the current registration exports return `E_NOTIMPL`; it does not invoke `regsvr32` or write registry/profile state.
- First build-only run surfaced a stale smoke executable still using the old CLSID. Updated the script to rebuild `smoke.cpp` with `clang++` before running it, then reran successfully. This avoids a false failure after identity changes.
- Validation command: `& 'D:\mcp-agent-workspace\typescript-windows-ime\native\golang\strategy-go-com\install.ps1' -BuildOnly` — exit code 0. DLL built; SHA-256 `29FBF5622B519DA9AA614AE1E001507D723934277145F6B5F429BC047E4FAD16`; smoke passed negative HRESULT/output-pointer cases and 100 object lifetimes × 5 Activate/Deactivate cycles.
- No installation, COM registration, registry writes, TSF profile changes, or input-method configuration changes were performed. This script is a safe scaffold, not a complete installer; actual installation remains blocked until registration APIs and rollback are implemented/reviewed and real TSF activation is tested in a disposable VM.

### Input method display-name change (2026-10-09)

- Changed the Strategy A scaffold's intended display name from `TypeScript Windows IME Go Strategy A` to `strategy-go-com` in `install.ps1` and the current name entry in `PLAN.md`.
- This is a source/configuration rename only. The installer still fails closed for `-Install`, and this did not rename or modify any already-registered input method. No registration or system input-method settings were changed.

### Registration implementation milestone (2026-10-09)

- Implemented `DllRegisterServer` and `DllUnregisterServer` for the isolated Strategy A CLSID/profile. The helper obtains the DLL's actual module path, refuses to overwrite an existing exact CLSID key, writes the COM `InprocServer32` path and `ThreadingModel=Apartment`, calls `ITfInputProcessorProfiles` to register and enable the language profiles 0x0409/0x0804/0x0411 under `strategy-go-com`, and registers the `GUID_TFCAT_TIP_KEYBOARD` category. Unregistration targets only this strategy's CLSID/profile/category.
- Registration rollback removes the strategy's profile/category metadata and exact CLSID subtree after a registration-step failure. Repeated unregistration returns `S_OK` when the strategy's CLSID key is already absent. This still needs a mutation test on a disposable VM to validate actual Windows TSF API behavior and edge cases; no mutation test was run here.
- Updated `PLAN.md` so registration implementation and rollback review precede real activation, key events, and text commit. Updated `install.ps1`'s fail-closed message; `-Install` remains blocked until disposable-VM review is approved.
- Validation: `install.ps1 -BuildOnly` passed after the registration code was added; the DLL built with SHA-256 `047694221F3882DC32AAE770D617860622B05178D7142EAD0EC7E2CAF4759F4F`, and the existing non-mutating direct-COM harness passed negative HRESULT/output-pointer cases and 100 object lifetimes × 5 Activate/Deactivate cycles. No registration export was called; no registry/profile/input-method state was changed.

### Registration rollback review follow-up (2026-10-09)

- Reviewed the existing registration failure path against Task 0's transaction requirement. Found that the outer failure handler called broad TSF cleanup even when `ITfInputProcessorProfiles::Register` had failed, which could affect orphaned/pre-existing TSF metadata for the same CLSID.
- Changed `StrategyRegisterProfiles` to perform local rollback only after its `Register` call succeeds, and to undo the profile registration after a later language-profile or keyboard-category failure. Removed the outer unconditional TSF cleanup; the outer handler now deletes only the exact COM CLSID subtree that this call preflighted as absent and began creating.
- Non-mutating validation after the change: `go test ./...` and `go vet ./...` exited successfully; `install.ps1 -BuildOnly` rebuilt the DLL and native harness and passed all existing direct-COM negative-path and lifecycle checks. DLL SHA-256: `2CFEB5D1FCC78F387EF0DC8C3A4DA88E6C0EC1AB4B3DCEF215A3E1C82298F1C7`.
- No registration exports, `regsvr32`, registry writes, TSF profile operations, or input-method setting changes were performed.
- Remaining Task 0 gaps: injectable failure-step seams/tests are not implemented; preservation behavior for pre-existing TSF-only metadata and exact HRESULT behavior during rollback still need code review and targeted tests. The `-Install` gate remains closed. Do not claim registration/rollback complete or execute a mutation test on this workstation.

### Registration transaction follow-up (2026-10-09)

- Fixed a COM registry transaction gap: if writing the first/default CLSID value fails after `RegCreateKeyExW` has created the key, control now reaches the exact-subtree rollback path rather than returning early and leaving a partial key behind.
- Made module-path truncation report `HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)` instead of depending on a possibly stale `GetLastError()` value.
- Validation after the change: `go test ./...` and `go vet ./...` exited successfully; `install.ps1 -BuildOnly` rebuilt the DLL and native harness, then passed the existing direct-COM negative-path and lifecycle smoke checks. DLL SHA-256: `78C7656E514995964011082B75E99771479CA91AFEDA8AA6C0F73ECE978A261A`.
- No registration exports, `regsvr32`, registry writes, TSF profile calls, or input-method settings changes were performed. Task 0 remains incomplete pending injectable failure tests and review of TSF metadata preservation/rollback failures.

### Registration code review follow-up — current session (2026-10-09)

- Read-only assertions passed for the Strategy A CLSID/profile identity, actual module-path lookup, `ThreadingModel=Apartment`, language IDs 0x0409/0x0804/0x0411, keyboard category registration, existing-COM-subtree refusal, and the `-Install` fail-closed guard.
- Source review found that registration operations are still hard-coded, so no tests can inject failures into each actual TSF/COM step. Some rollback calls ignore cleanup HRESULTs, and unregistration behavior when TSF metadata is partially absent has not been shown to be idempotent/retry-safe.
- `go test ./...` and `go vet ./...` both exited 0. The Go module currently has no test files, so these results do not validate transaction ordering or rollback behavior.
- No source behavior or generated binary was changed in this review. No registration export, `regsvr32`, registry write, TSF profile operation, or input-method setting change was performed. `install.ps1 -Install` remains blocked.
- Next code step: add injectable operations around the real registration calls, then test every failure stage, rollback order, original HRESULT preservation, cleanup failure reporting, and repeated cleanup. Mutation tests must remain isolated from the developer workstation.

### Rollback HRESULT diagnostics follow-up — current session (2026-10-09)

- Added `OutputDebugStringW` diagnostics for profile rollback failures (cleanup manager creation and `ITfInputProcessorProfiles::Unregister`). The diagnostic captures the original registration HRESULT and the cleanup HRESULT while preserving the original HRESULT as the returned failure.
- Kept failed `RegisterCategory` cleanup conservative: the failure result does not prove this call created the category, so it is not blindly unregistered; this avoids deleting pre-existing TSF-only orphan metadata. A successful profile `Register` is still required before profile rollback is attempted.
- `gofmt`, `go test ./...`, `go vet ./...`, and `install.ps1 -BuildOnly` all passed. Direct-COM smoke passed negative HRESULT/output-pointer cases and 100 object lifetimes × 5 Activate/Deactivate cycles. DLL SHA-256: `1C8A28DE36C273821484D10CB83F924B0971455C4ABBA42A38D7A2FF94247120`.
- The Go module still reports `[no test files]`; transaction fault injection and partial-metadata retry tests remain outstanding. No registration export, `regsvr32`, registry/TSF mutation, or input-method setting change was performed; `install.ps1 -Install` remains blocked.

### COM subtree rollback diagnostics follow-up (2026-10-09)

- Closed one additional observability gap in `StrategyRegisterAll`: if deleting the exact Strategy A CLSID subtree fails during rollback, the code now emits `OutputDebugStringW` diagnostics containing both the original registration HRESULT and the cleanup HRESULT. The registration API continues returning the original failure.
- Ran `gofmt -w main.go`, `go test ./...`, and `go vet ./...`; formatting and static checks passed. `go test` explicitly reports `[no test files]`, so this is not transaction behavior coverage.
- Ran `install.ps1 -BuildOnly`: DLL build passed and native smoke passed negative HRESULT/output-pointer checks plus 100 object lifetimes × 5 Activate/Deactivate cycles. DLL SHA-256: `6145833E121913E77200C5545DE572A621DC9626D4D7E52E04407ABD12817AC2`.
- This is a narrow diagnostics improvement, not completion of P0. Production-used injectable seams, deterministic failure-injection tests, rollback-order/HRESULT assertions, and partial-TSF-metadata retry tests remain outstanding. No registration export or `regsvr32` was invoked; no registry, TSF profile, or input-method state was changed. `install.ps1 -Install` remains blocked.

### Shared registration transaction coordinator — continuation (2026-10-09)

- Added `registration_txn.h`, a shared coordinator for the production registration sequence and a deterministic fake-operation native test. Refactored `StrategyRegisterAll` to pass production callbacks for exact CLSID probing, COM values, TSF profile registration, exact CLSID rollback, and cleanup diagnostics.
- Added `registration_txn_test.cpp`. It injects failures at the five transaction stages and checks that failed mutations invoke exact-subtree rollback, successful registration runs all stages without rollback, and a cleanup failure is reported while preserving the original registration HRESULT.
- Validation: `gofmt -w main.go`, `go test ./...` (exit 0; still reports `[no test files]`), `go vet ./...` (exit 0), `install.ps1 -BuildOnly` (exit 0), direct-COM smoke passed negative HRESULT/output-pointer cases and 100 object lifetimes × 5 Activate/Deactivate cycles, and `RegistrationTxnTest.exe` passed its transaction assertions. DLL SHA-256: `B77F479BA8085B992A4B00A760106D2644FB5E6581EE20AE612E6DD98399175F`.
- The cgo build emits C23-extension warnings for unnamed callback parameters; these are warnings, not build failures.
- P0 remains incomplete. The test seam currently wraps the TSF profile registration as one high-level operation; individual `ITfInputProcessorProfiles`/`ITfCategoryMgr` calls are not yet independently injectable. Detailed assertions for pre-existing TSF-only/orphan metadata preservation, invalid/null/truncated module paths, and retry-safe partial unregistration remain outstanding. No registration export, `regsvr32`, registry/TSF mutation, or input-method setting change was performed. `-Install` remains blocked.
### P0 continuation — production path validation and transaction assertions (2026-10-09)

- Added `StrategyValidateModulePath` to `registration_txn.h` and wired the production `StrategyRegisterAll` path through it before any registry/TSF operation. It rejects null paths, zero capacity, empty paths, and truncated paths with deterministic HRESULTs.
- Expanded `registration_txn_test.cpp` to assert the exact forward operation sequence reached for each injected failure, that a failed preflight does not roll back, that mutation-stage failures reach exact COM subtree rollback, and that valid/null/empty/zero-capacity/truncated module paths behave as expected.
- Validation: focused native test rebuilt with `clang++ -std=c++17 .\registration_txn_test.cpp -o .\RegistrationTxnTest.exe -lole32 -luuid` and passed (`PASS: registration transaction failure injection, rollback, and HRESULT preservation`). `gofmt -w .\main.go`, `go test ./...` (exit 0; reports `[no test files]`), `go vet ./...` (exit 0), and `install.ps1 -BuildOnly` (exit 0) passed. Direct-COM smoke passed negative HRESULT/output-pointer checks and 100 object lifetimes × 5 Activate/Deactivate cycles.
- DLL SHA-256 after build: `B9500351D071A423333B623A63CDC8218AED02583C388CBB1068F1E66457AE07`. Existing C23-extension warnings about unnamed cgo-generated callback parameters remain warnings; no validation command failed.
- P0 remains incomplete: production TSF profile/category calls are still grouped inside a high-level callback, so individual Register/AddLanguageProfile/EnableLanguageProfile/RegisterCategory and cleanup steps are not independently injectable. TSF-only/orphan metadata preservation and retry-safe partial unregistration remain unproven. `install.ps1 -Install` remains fail-closed.
- No registration export, `regsvr32`, registry write, TSF profile operation, or input-method setting change was performed.

### P0 transaction seams — per-call coordinator (2026-10-09)

The production registration and unregistration paths now call the shared coordinator in `registration_txn.h`. Each actual step is its own callback: COM class probe, class-name write, module-path write, `ThreadingModel=Apartment` write, `ITfInputProcessorProfiles::Register`, `AddLanguageProfile` and `EnableLanguageProfile` for `0x0409`, `0x0804`, and `0x0411` under display name `strategy-go-com`, and `ITfCategoryMgr::RegisterCategory` for `GUID_TFCAT_TIP_KEYBOARD`. Cleanup callbacks are `UnregisterCategory`, `Unregister` of the Strategy A CLSID, and deletion of the exact CLSID subtree. Identity remains `{A4C7D921-6B35-4E8A-91F2-7D0B6C3E5A14}` / `{E8B1C4D7-2A63-49F0-B5D8-1C7E3A9F6240}`.

The coordinator records only artifacts this invocation created. The class-name write counts as creating the COM key because `RegCreateKeyExW` can create that key before the value write fails. Profile cleanup runs only after `Register` succeeds. Category cleanup runs only after `RegisterCategory` succeeds, so a failed category call does not unregister a pre-existing category. Cleanup order is category, then profiles, then the COM subtree. If a cleanup fails, its HRESULT is reported with the original registration HRESULT, and the original HRESULT is returned.

Unregistration always attempts category, profiles, and the COM class. Production category and profile callbacks enumerate first and skip the mutating call when this CLSID is absent; `RegDeleteTreeW` treats a missing key as success. A second pass therefore succeeds without deleting unrelated COM values or orphan TSF metadata. A hard failure is returned after the remaining steps run.

`registration_txn_test.cpp` drives that same coordinator with fakes. It does not load the DLL, call `DllRegisterServer`/`DllUnregisterServer`, or touch the registry or TSF. `go test ./...` still reports `[no test files]` and is not transaction coverage.

Commands, from `native\golang\strategy-go-com`:

- `clang++ -std=c++17 .\registration_txn_test.cpp -o .\RegistrationTxnTest.exe -lole32 -luuid`, then `.\RegistrationTxnTest.exe` twice. Both runs exited 0 and printed `PASS: registration transaction failure injection, rollback, HRESULT preservation, orphan preservation, path rejection, and retry-safe unregistration`.
- `gofmt -w .\main.go` exited 0. `gofmt -l .\main.go` printed nothing afterward.
- `go test ./...` exited 0 with `? strategy-go-com-experiment [no test files]`.
- `go vet ./...` exited 0.
- `.\install.ps1 -BuildOnly` twice. Both runs exited 0, printed `Build-only mode complete. No registration was attempted.`, and the smoke line `PASS: negative HRESULT/output-pointer cases; 100 object lifetimes x 5 Activate/Deactivate cycles`.

DLL SHA-256 from the two BuildOnly runs: `AC903593FA3CAB0044500D160D461230BABC13FB498621BFBE7F4176265AB304`, then `4AE67DB7BAD2EBCD6880AB0A309995E4F978AC15DED0145641B0B90943457494`. The file on disk is the second build. The hashes differ because `go build -buildmode=c-shared` does not produce byte-identical PE images across runs. No cgo warning was printed; the registration callbacks now use named parameters. The cgo build id of the on-disk DLL is `QwK1JxygC2V7ObGk5OZB/GQl4KTFLyZVgobUiMylq/FQ2nz36U5mRC6nn-eCNt/H0WHX4SEMfAJJi57OkUa`.

`install.ps1 -Install` was not executed. After the build it still throws before any `regsvr32`, registration export, or registry/TSF write. No `DllRegisterServer` or `DllUnregisterServer` call, `regsvr32`, or input-method change was performed.

These transaction results are coordinator decisions under fake operations. They are not proof that Windows TSF accepted a live registration, and they are separate from the direct-COM smoke checks. Live idempotence against the real registry remains untested. Real TSF activation, `ITfKeyEventSink`, and text commit were not started. P0's non-mutating coordinator criteria for this session are met; `-Install` stays fail-closed until a reviewed disposable-VM run exists.
