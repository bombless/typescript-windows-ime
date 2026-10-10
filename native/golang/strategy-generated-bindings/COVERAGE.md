# Strategy D — TSF binding coverage and feasibility report

## Result (2026-10-09)

**Decision: keep this strategy experimental; do not replace the existing adapter yet.** The installed Windows SDK provides authoritative TSF declarations, and `github.com/zzl/go-com` documents a Go-side COM implementation pattern. A local module cache contains `github.com/zzl/go-win32api/v2@v2.0.1`, but a scan of that source tree found no text matches for the four required TSF interfaces. The newer candidate version could not be queried because `proxy.golang.org` is unreachable. The local scan is evidence only about cached v2.0.1 and does not prove the latest package or custom generation lacks these declarations.

## Interface coverage matrix

| Required surface | Authoritative declaration | Binding source status on this machine | Go-side implementation status | Next proof required |
|---|---|---|---|---|
| `ITfTextInputProcessor` (`Activate`, `Deactivate`) | Windows SDK `um/msctf.h`; Microsoft Learn interface page | **Unknown** — candidate module unavailable locally and registry access failed | **Plausible, unproven** — `go-com` describes COM interface implementation, but this interface has not been compiled against the candidate bindings | Resolve/pin `go-win32api/v2`; locate generated interface, IID, and exact signatures; implement a minimal object and verify `QueryInterface`/vtable calls |
| `ITfKeyEventSink` (`OnSetFocus`, `OnTestKeyDown`, `OnTestKeyUp`, `OnKeyDown`, `OnKeyUp`, `OnPreservedKey`) | Windows SDK `um/msctf.h`; Microsoft Learn interface page | **Unknown** | **Plausible, higher risk** — callbacks must be exposed with exact ABI and inherited `IUnknown` slots | Compare every method, parameter type, HRESULT and vtable position with SDK; call through TSF or a native harness |
| `ITfKeystrokeMgr` (at minimum `AdviseKeyEventSink`, `UnadviseKeyEventSink`) | Windows SDK `um/msctf.h` | **Unknown** | Client-side calls appear to be the intended generated-binding use case; not a Go-implemented interface for this experiment | Confirm methods/IID and prove registration/unregistration from the processor's `Activate`/`Deactivate` lifecycle |
| `ITfEditSession` (`DoEditSession`) | Windows SDK `um/msctf.h`; Microsoft Learn TSF API docs | **Unknown** | **Plausible, unproven** — Go must implement a callback object passed to TSF, not merely invoke a method | Verify callback interface ABI and run a real edit session that inserts text |
| COM object identity, `IUnknown`, class factory and lifetime | Windows SDK COM ABI; `go-com` README/API | Not a TSF binding question | **Framework claims a pattern exists; target integration unverified** | Prove `QueryInterface`, `AddRef`/`Release`, class factory activation and callback lifetime under real TSF |

## Evidence and traceability

- Local SDK used for the inspection: `C:\Program Files (x86)\Windows Kits\10\Include\10.0.22000.0\um\msctf.h` (Windows SDK 10.0.22000.0). Relevant declarations found at approximately lines 5631 (`ITfEditSession`), 9619 (`ITfKeystrokeMgr`), 9894 (`ITfKeyEventSink`), and 10500 (`ITfTextInputProcessor`). These locations can move in other SDK versions; rerun `Verify-Coverage.ps1` against the installed SDK.
- Microsoft Learn: [`ITfTextInputProcessor`](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nn-msctf-itftextinputprocessor), [`ITfKeyEventSink`](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nn-msctf-itfkeyeventsink), [`Activate`](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itftextinputprocessor-activate).
- Candidate generated bindings: [`github.com/zzl/go-win32api`](https://github.com/zzl/go-win32api), module [`github.com/zzl/go-win32api/v2`](https://pkg.go.dev/github.com/zzl/go-win32api/v2). Its published README says most bindings are generated from Microsoft's Win32 metadata, that only frequently used APIs are included, and omitted APIs may require generating code. Published module version observed during research: `v2.2.0` (2024-05-09); this is a candidate version, not yet adopted or verified.
- Candidate COM implementation framework: [`github.com/zzl/go-com`](https://github.com/zzl/go-com), [package API](https://pkg.go.dev/github.com/zzl/go-com/com). Its README says it offers a pattern for implementing COM interfaces; this does not prove that the generated TSF declarations are implementable without adapter code.
- Registry check attempted with `go list -m -versions github.com/zzl/go-win32api` and `github.com/zzl/go-com`; both failed to connect to `proxy.golang.org`. No dependency was added and no version was pinned in `go.mod`.

## Reproduction

Run from this directory in PowerShell:

```powershell
./Verify-Coverage.ps1
```

To inspect a checked-out or downloaded binding tree explicitly:

```powershell
./Verify-Coverage.ps1 -BindingRoot 'C:\path\to\go-win32api'
```

The script only reads files. It reports declarations and likely method matches; it is a discovery aid, **not** a proof of ABI compatibility or server-side implementability. Review signatures and vtable order against `msctf.h`/`Msctf.idl` before using any declaration.

## Generator and update policy

1. Do not add the dependency until network/package source is available and interface coverage has been inspected.
2. If upstream generated files cover the required interfaces, pin an exact module version in the isolated strategy module and commit `go.mod` plus `go.sum`.
3. If custom generation is needed, pin the generator commit/version, metadata revision, command line, and output directory; check generated diffs into source control.
4. For every update, diff ABI-critical declarations (GUIDs, method order, pointer width, HRESULT, BOOL, WPARAM/LPARAM, and callback signatures) against the SDK and rerun compile-time and runtime tests.
5. Do not count a compile or a `QueryInterface` smoke test as acceptance. Final acceptance still requires activation, key sink registration, edit-session text commit, and testing inside a real TSF host.

## Follow-up execution log (2026-10-09)

- Re-ran `Verify-Coverage.ps1` against the default module-cache root and explicitly against `C:\Users\bombl\go\pkg\mod\github.com\zzl\go-win32api\v2@v2.0.1`. The SDK declarations were found; no source-text matches were found for `ITfTextInputProcessor`, `ITfKeyEventSink`, `ITfKeystrokeMgr`, or `ITfEditSession` in cached v2.0.1.
- `go list -m -versions github.com/zzl/go-win32api/v2` and `go list -m -versions github.com/zzl/go-com` could not reach `proxy.golang.org`. No dependency or strategy module was changed.
- Cached `github.com/zzl/go-com@v1.5.0` README documents a general callback/COM-interface implementation pattern. This is not proof that the missing TSF interfaces can be implemented without generated declarations or additional ABI work.
- SDK/IDL spot-checks confirmed the following IIDs and base interfaces in SDK 10.0.22000.0: `ITfTextInputProcessor` `{aa80e7f7-2021-11d2-93e0-0060b067b86e}`, `ITfKeyEventSink` `{aa80e7f5-2021-11d2-93e0-0060b067b86e}`, `ITfKeystrokeMgr` `{aa80e7f0-2021-11d2-93e0-0060b067b86e}`, and `ITfEditSession` `{aa80e803-2021-11d2-93e0-0060b067b86e}`. All four directly inherit `IUnknown`; method declarations in `msctf.idl` match the interface method names listed above. This is a spot-check, not a complete per-parameter ABI audit.
- `go test ./...` and `go vet ./...` both completed successfully from `native/golang` (tests reported cached success). No activation, callback ABI, keyboard registration, edit-session, or real-host test was possible from this evidence alone.

## Acceptance status

- [x] First experiment produced a coverage matrix and explicit unknowns.
- [x] Local SDK declarations located for all four TSF interfaces.
- [x] Cached `go-win32api/v2@v2.0.1` source scanned; no required TSF interface text matches found.
- [x] Required interface IIDs and `IUnknown` inheritance spot-checked against the installed SDK/IDL.
- [ ] Candidate generated declarations compiled with a Go-side implementation.
- [ ] Activation, key handling, edit sessions, and text commit tested in a real TSF host.
- [ ] Generator/tool versions pinned and regeneration made reproducible.

## Follow-up execution log (2026-10-09, upstream check)

- The first default network attempt could not reach `sum.golang.org`. A local proxy at `http://127.0.0.1:7897` was reachable and allowed Go module queries/downloads. No repository `go.mod` or `go.sum` was changed.
- `go list -m -versions github.com/zzl/go-win32api/v2` returned `v2.0.0`, `v2.0.1`, `v2.0.2`, `v2.1.0`, `v2.2.0`. Downloaded exact upstream tag `v2.2.0` for inspection only; origin commit `56d319e1869e40dd106b4056332d599a49d33eda`; module checksum `h1:vLVc9ATxK1wY4qcT4XhahieFfgI1AngkCzsQXuDVlew=`; go.mod checksum `h1:doi6ewHPdh9tDmqe837Ro7IwqtB9yE+1fC8suK/Ssj0=`. Latest `github.com/zzl/go-com` resolves to `v1.5.0` (2023-01-29), already cached.
- Re-ran `Verify-Coverage.ps1 -BindingRoot "$env:USERPROFILE\go\pkg\mod\github.com\zzl\go-win32api\v2@v2.2.0"`: SDK declarations found; no text matches for all four target TSF interfaces. This is direct evidence for inspected v2.2.0 source only, not for every possible generator or future version.
- Inspected installed SDK IDL (`um/msctf.idl`): `ITfEditSession::DoEditSession(TfEditCookie)`; `ITfKeystrokeMgr` starts with `AdviseKeyEventSink(TfClientId, ITfKeyEventSink*, BOOL)` and `UnadviseKeyEventSink(TfClientId)`; `ITfKeyEventSink` callback order is `OnSetFocus`, `OnTestKeyDown`, `OnTestKeyUp`, `OnKeyDown`, `OnKeyUp`, `OnPreservedKey`; `ITfTextInputProcessor` methods are `Activate(ITfThreadMgr*, TfClientId)` then `Deactivate()`. Each directly inherits `IUnknown`, so the inherited three slots precede these methods. This is a signature/order review of the IDL declarations, not a complete binary ABI/runtime audit; pointer-width, HRESULT, BOOL, WPARAM/LPARAM, calling convention and lifetime tests remain outstanding.
- `go-com@v1.5.0` has a generic COM object constructor and class-factory implementation sources, so a framework pattern exists. It does not supply the missing TSF interfaces in the inspected binding package.
- Validation rerun: `go test ./...` and `go vet ./...` passed from `native/golang`; `go test ./...` and `go vet ./...` passed from `strategy-manual-com`; `go test ./...` passed from `strategy-go-com`. These checks do not demonstrate TSF activation.
- Strategy comparison from local plans: Strategy A has a DLL prototype but its native smoke test currently fails `CreateInstance` with `E_NOINTERFACE`; Strategy B has ABI declaration/unit-test slices and a scalar callback spike but no COM server; Strategy D has no TSF binding declarations in the inspected v2.2.0 source and no proven reproducible TSF generation pipeline. Recommendation: **no-go for implementing the TSF server on v2.2.0 as-is; keep Strategy D blocked/experimental**. Next useful experiment, before hand-writing interfaces, is to establish a reproducible minimal generation path from Microsoft SDK/metadata or another maintained source and compile one interface against `go-com`. No activation, key-event, edit-session or real-host test was attempted. No registration/installation was performed.

## Follow-up execution log (2026-10-09, generator source experiment)

- Downloaded the public `zzl/go-winapi-gen` source archive to a temporary directory only; inspected upstream `main` commit `4fd990193d56c9ea8fb18ece1adac5016c7dcbc4` (commit date reported by GitHub API: 2023-09-04; repository license: MPL-2.0). The archive contains `assets/Windows.Win32.winmd` (SHA-256 `81BFFCC120D9743680047912914FCCF62C0F189E3611612780BECAE07F317AD9`), `cmd/win32api-gen/main.go`, and the Go model/codegen implementation. Its `go.mod` pins `github.com/zzl/go-winmd v1.0.0` and the legacy `github.com/zzl/go-win32api v1.1.3`.
- The generator's checked-in default namespace allowlist omits `Windows.Win32.UI.TextServices`. In the temporary checkout only, added `UI.TextServices` to that allowlist and ran `go run ./cmd/win32api-gen`. It successfully emitted `output/win32/UI.TextServices.go` and included all four required interface declarations with their IIDs and methods. Generated output SHA-256 was identical on two runs: `47FAC627AE6AEE653FDC17DCB0353ED412DB8DDD39899F464FD4000CF25EBD60`.
- This establishes a repeatable **namespace-level declaration-generation path** from the generator's bundled WinMD. It does **not** yet establish a narrowly scoped four-interface output: the generated TextServices file is about 354 KB and includes many additional TSF declarations. The generator's filter is namespace-level only; it has no interface-name allowlist.
- Crucially, `codegen.genInterface` emits client wrappers that invoke methods with `syscall.SyscallN`. It emits interface/IID/vtable declarations but does not generate server-side callback thunks or COM implementation classes. Therefore this generator alone does not satisfy Strategy D's server-side implementation requirement; the callback/vtable glue still needs a separate, reviewed implementation or a generator extension.
- The source archive was reviewed in `%TEMP%\strategy-d-generator-review\go-winapi-gen-main`; no repository source files were changed during the generator experiment. The temporary generator edit and generated output are outside the project workspace.
- Decision update: the declaration-generation feasibility question is partially answered **yes**, but the full narrow-output/server-implementation gate is still open. Do not yet add the generated 354 KB namespace file or pin this generator as a project dependency. Next, inspect whether a small reproducible post-filter/codegen extension can emit only `IUnknown` plus `ITfTextInputProcessor`, and prove the resulting interface can be exposed by `go-com@v1.5.0` in an isolated strategy module. No TSF activation or real-host tests were performed.

## Follow-up execution log (2026-10-09, minimal extraction fixture)

- Added `Extract-Minimal.ps1` and generated `itf_text_input_processor_generated.go` from the previously inspected generator output. The script selects the IID/interface/vtable/client-wrapper block and normalizes the generated unkeyed GUID literal to keyed fields so `go vet` passes without changing IID bytes.
- Added an isolated `go.mod`/`go.sum` in this strategy directory. `go test ./...` and `go vet ./...` passed. The fixture checks the IID, vtable size and Go interface method shape. The parent `native/golang/go.mod` was not modified.
- Follow-up refinement: the extractor now emits a distinct opaque `ITfThreadMgr` struct embedding `IUnknown`, rather than aliasing the name directly to `win32.IUnknown`. A regression test asserts that the names remain distinct. This is still only a pointer-target placeholder; it is not the real dependency-closed `ITfThreadMgr` declaration.
- Re-ran `go test .` and `go vet .` from this strategy directory after regeneration; both passed. An initial `go test ./...` invocation through the agent wrapper ran without the requested directory as its process cwd and failed with `directory prefix . does not contain a main module`; repeating with an explicit PowerShell `Set-Location` succeeded. This was a command-context issue, not a Go test failure.
- Inspected cached `go-com@v1.5.0` implementation sources and examples. Its pattern uses `IUnknownImpl`, a `ComObjInterface`, and per-interface COM-object/vtable wrappers with `syscall.NewCallback`; it does not automatically implement this generated TSF interface. The custom `ITfTextInputProcessor` callback/vtable bridge, identity behavior, and lifetime remain unproven.
- Status remains **blocked/experimental**. The next-session refinement strengthened the custom processor COM-object/vtable fixture against `go-com@v1.5.0`: it checks initial reference ownership, `AddRef`/`Release` counts, successful `QueryInterface` identity for the TSF IID and `IUnknown`, unsupported-IID rejection, Activate/Deactivate dispatch, and final `Release` calling the COM wrapper's `Finalize` hook. The first run exposed that go-com invokes `Finalizable` on the COM wrapper, not the implementation object; the fixture was corrected to match that contract. `go test .` and `go vet .` then passed from this directory.
- SDK `um/msctf.idl` spot-check at SDK 10.0.22000.0 confirms `ITfTextInputProcessor : IUnknown`, `Activate(ITfThreadMgr*, TfClientId)` followed by `Deactivate()`, and `TfClientId` is a `DWORD` typedef (mapped to `uint32` in the fixture). This does not close the full ABI audit: callback calling convention, 32-bit behavior, HRESULT error round-tripping, concurrency/lifetime under TSF, class-factory activation, dependency-closed `ITfThreadMgr`, key sink registration, edit sessions, and real-host text commit remain unproven. No TSF activation, key handling, edit session, real-host test, production TIP registration, installation or system configuration change was performed.
- Next gate: finish the per-parameter and callback ABI audit, including HRESULT/pointer-width assumptions and vtable layout; then prove class-factory activation and object lifetime only if the audit passes. Keep Strategy D blocked until that and the real-host acceptance criteria are met.

## Follow-up execution log (2026-10-09, ABI layout assertions)

- Strengthened `fixture_test.go` to assert `ITfTextInputProcessorVtbl.Activate` and `.Deactivate` offsets are exactly slots 3 and 4 after the inherited three-slot `IUnknown` vtable, and that `HRESULT` and the `TfClientId`/`DWORD` representation are 4 bytes. These are layout/type-width assertions, not proof of the callback calling convention or lifetime safety.
- Validation from `strategy-generated-bindings`: `go test .` passed; `go vet .` passed; `GOARCH=386 go test -c` passed, in addition to the native `windows/amd64` test run. The 386 artifact was compile-only and was not executed.
- SDK `msctf.idl` confirms the processor methods are `HRESULT Activate([in] ITfThreadMgr *ptim, [in] TfClientId tid)` followed by `HRESULT Deactivate()`, with `TfClientId` declared as `DWORD`. The generated fixture represents the interface parameter as an opaque pointer target and the ID as `uint32`; it still lacks a dependency-closed `ITfThreadMgr` declaration.
- **Still open at this checkpoint:** exact callback ABI on both architectures, HRESULT error round-tripping, COM apartment/threading assumptions, reentrancy and concurrent final-release behavior. The `syscall.NewCallback` fixture invokes callbacks in-process through generated client wrappers only. Because these gates are not yet closed, class-factory/activation and TSF behavior work has not been promoted, and Strategy D remains blocked/experimental. No TSF activation, registration, installer or system change was performed.

## Follow-up execution log (2026-10-09, HRESULT callback round-trip)

- Cross-checked SDK 10.0.22000.0 `um/msctf.idl`: `ITfTextInputProcessor : IUnknown`; `Activate([in] ITfThreadMgr *ptim, [in] TfClientId tid)` and `Deactivate()` both return `HRESULT`; `TfClientId` maps to `DWORD`/32-bit unsigned. The local fixture's method order and parameter widths match these declarations, but `ITfThreadMgr` is still only an opaque pointer target.
- Inspected cached `go-com@v1.5.0` implementation sources: the framework consistently builds COM vtable entries with `syscall.NewCallback` and uses `uintptr` return values on callback wrappers. This supports the chosen implementation pattern but is not independent proof of the TSF callback ABI, especially on 32-bit Windows.
- Extended `processor_com_fixture_test.go` to exercise `E_FAIL` from both Activate and Deactivate through the callback thunk and generated client wrapper, and to verify failure does not mutate the fixture's activation state. Follow-up success calls verify recovery.
- Validation passed in this Windows/amd64 session: `go test .`, `go vet .`, and `GOARCH=386 go test -c`. The 386 test binary was compile-only and was not executed. Thus HRESULT round-tripping is now tested on amd64, while 32-bit runtime behavior, external/native caller behavior, apartment/threading, reentrancy, and concurrent final-release remain unproven.
- Strategy D remains **blocked/experimental**. No class-factory activation, TSF host activation, key sink, edit session, text commit, registration, installer, or system configuration change was performed.