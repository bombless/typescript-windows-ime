# Strategy D — Processor callback ABI audit (partial)

Date: 2026-10-09  
Scope: the current `ITfTextInputProcessor` compile fixture only. This is an evidence record, not production ABI approval.

## Authoritative SDK evidence

SDK: Windows 10 SDK 10.0.22000.0.

- `um/msctf.idl:2355-2361` declares `ITfTextInputProcessor : IUnknown`, then `HRESULT Activate([in] ITfThreadMgr *ptim, [in] TfClientId tid)` and `HRESULT Deactivate()`, in that order.
- `um/msctf.h:10500-10509` declares IID `aa80e7f7-2021-11d2-93e0-0060b067b86e` and the C++ methods with `HRESULT STDMETHODCALLTYPE`. The interface has two methods after the inherited three `IUnknown` slots.
- `TfClientId` is the SDK's `DWORD`-sized identifier; the fixture uses `uint32`. `HRESULT` is represented by the dependency's 32-bit `win32.HRESULT` alias. Existing tests assert both widths and method slot offsets.
- `go doc syscall.NewCallback` on the current toolchain (`go1.27.0 windows/amd64`) states that it creates a callback conforming to Windows `stdcall`, requires a single `uintptr`-sized result, limits argument sizes to `uintptr`, and allocates callback memory that is never released. The current callbacks use `uintptr` results and pointer/`uint32` arguments, all no wider than a pointer on amd64. This documents Go's callback contract, but does not by itself prove the COM caller ABI on all supported architectures.
- `processor_com_fixture_test.go` exercises local generated-wrapper dispatch, `QueryInterface` identity, reference-count changes, unsupported-IID rejection, finalization, and `S_OK`/`E_FAIL` return paths on amd64. `GOARCH=386 go test -c` is compile-only evidence; that executable has not been run.

## Current mapping and explicit limits

| ABI item | Fixture mapping/evidence | Status |
|---|---|---|
| IID | `IID_ITfTextInputProcessor` matches the SDK UUID | Spot-checked |
| Inheritance and vtable order | 3 inherited slots + Activate + Deactivate; offset assertions | Compile/layout checked |
| `ptim` parameter | One pointer-sized `*ITfThreadMgr`; opaque signature-only placeholder | Pointer shape only; semantic declaration not closed |
| `tid` parameter | `uint32` for SDK `DWORD/TfClientId` | Width checked |
| Return value | 32-bit `HRESULT` crossing `syscall.NewCallback` as `uintptr`; `E_FAIL` round-trip test on amd64 | Local amd64 only |
| Callback calling convention | Go documents `NewCallback` as Windows stdcall | External/native dispatch and 386 runtime still untested |
| COM lifetime | In-process identity/refcount/finalize checks | No concurrency, reentrancy, or TSF-held-reference proof |
| Apartment/threading | Not covered | Open |

## Decision

Do not progress to TSF activation yet. The next meaningful gate is an independent native/external caller test for callback dispatch on supported Windows architectures, alongside an audit of `go-com`'s refcount/finalization behavior under concurrent release. Do not infer thread safety from the single-threaded fixture. The processor interface's ABI-shaped signature is plausible, but this evidence is not sufficient to register or install a text service.

## Newly identified framework lifetime risk

Inspection of the pinned `github.com/zzl/go-com@v1.5.0` `com/impl.go` shows that `IUnknownComObj.AddRef`/`Release` use `atomic.AddInt32` for the per-object counter, but final release calls `free()`, which reads/writes the package-global `impls` and `freeImplSlots` slices without taking a lock. `AddImpl` also mutates those slices without locking. `MuVtbl` protects vtable initialization only; it does not protect the implementation-slot registry. Concurrent COM object creation/final release across objects may therefore race on these global slices. The current single-threaded fixture does not exercise this and must not be treated as concurrency/lifetime proof. Do not try to prove safety by stress-testing a potentially racy allocator path; either isolate the framework behind serialized object lifecycle or patch/fork/vendor a reviewed synchronization fix before multithreaded use.

## Decision update

This concrete concurrency risk strengthens the no-go for TSF activation. A native caller harness would validate callback entry ABI but would not fix the framework's global registry race. Keep both the callback boundary and COM framework lifetime as separate blockers. No production TIP registration, installer, or system configuration change was performed.

## Follow-up investigation: viable mitigation boundary

The registry race is not safely contained by a mutex in the TSF processor implementation: `go-com` owns the package-global `impls` and `freeImplSlots` arrays, and every COM object created through `NewComObj` participates in that registry. Locking only this strategy's object factory would not protect other objects created by the same package, and locking only `Release` would leave `AddImpl` and `Impl()` concurrent with registry mutation. A process-wide serialization scheme would require every COM object operation using this package to cooperate, which a TSF host and unrelated package users cannot guarantee.

The next safe experiment should therefore be a strategy-local fork/replacement of the pinned `go-com` module, not a wrapper around the processor fixture. The patch should introduce one registry mutex and consistently protect `AddImpl`, `Impl` slot lookup, and slot removal/reuse. User lifecycle hooks (`OnComObjFree`/`Finalize`) and heap freeing must execute outside the registry lock to avoid re-entrant deadlocks; final-release ordering and slot reuse need explicit tests. Audit every access to both registry slices before accepting the patch. Add a race-oriented test for concurrent creation/final release of independent objects only after the fix is in place, and run `go test -race` where supported. This would address the identified Go data race, not establish COM's external reference-lifetime contract or callback ABI.

**Decision:** do not attempt class-factory activation or native callback dispatch until the synchronized fork is reviewed and tested. Keep the production dependency pinned and unmodified until that isolated patch demonstrates correctness; do not use stress tests against the currently racy implementation as evidence of safety.

## Follow-up execution: lifecycle reentrancy and native callback probe (2026-10-09)

- Added `TestLifecycleHooksRunOutsideRegistryLock` to `processor_com_fixture_test.go`. The `OnComObjFree` and `Finalize` hooks each create and release a child COM object, forcing registry re-entry; a three-second timeout makes a lock-order regression fail instead of hanging indefinitely.
- `gofmt`, `go test -race ./...`, `go vet ./...`, and a `GOARCH=386 go test -c` compile check passed from this strategy directory on Windows/amd64. The 386 executable was not run.
- LLVM-MinGW `gcc` is available and `CGO_ENABLED=1`. A disposable native C-to-Go callback harness successfully called a `syscall.NewCallback` stdcall thunk with two pointer-sized arguments and received `E_FAIL` (`0x80004005`) unchanged. This establishes direct native C entry into the callback thunk for this amd64 harness, but is not a native COM vtable caller and does not validate the complete processor vtable, `QueryInterface`, refcount behavior across a native caller, or 386 runtime behavior.
- The local `go-com` fork was compared with cached v1.5.0 `com/impl.go`; functional differences are the registry RW mutex and the three guarded registry paths. The diff also removes one standalone comment line. Cached upstream `com/impl.go` SHA-256: `420F140420890570FE5E24C708EF65A0950E4065C8E158D09F52EF43B014EB16`; upstream `go.mod` SHA-256: `6E95D03FA1BC689952F8887ACEE688BC8AB3AF30697A889C0BBD3B97C2122DB2`. The upstream VCS revision is not encoded in the module cache and remains to be independently sourced.
- Remaining blockers: finalization-order assertions beyond hook re-entry; full dependency closure and per-parameter ABI audit; native vtable caller test; 386 runtime evidence; COM apartment/reentrancy and external reference lifetime; and real TSF-host activation/key handling/edit-session text commit. `ITfThreadMgr` remains an opaque pointer placeholder. Do not proceed to class-factory or TSF activation yet.

## Registry slot-reuse regression tests (2026-10-09)

- Added `third_party/go-com/com/impl_registry_test.go`: `TestAddImplReusesFreedSlotsInLIFOOrder` verifies the free-slot stack order and implementation identities; `TestAddImplRejectsOccupiedFreeSlot` verifies a stale/corrupt free-list entry is not silently used to overwrite an occupied registry slot. The tests isolate and restore package registry state.
- `go test -race -count=1 ./com` passed from the nested `third_party/go-com` module. Root strategy `go test -race ./...` and `go vet ./...` passed. `go vet ./com` reports an existing unsafe-pointer warning at `com/context.go:64`; full-fork `go vet ./...` also reports existing suspicious OR conditions at `ole/variant.go:1042` and `ole/variant.go:1050`. No changes were made to those unrelated files.
- Note: Go does not traverse a nested module from the strategy root `./...`; validate the fork separately from `third_party/go-com`.

## Follow-up experiment: isolated registry synchronization (2026-10-09)

- Copied the exact cached `github.com/zzl/go-com@v1.5.0` source into `third_party/go-com` and added a strategy-local `replace` directive. The upstream module cache and `native/golang/go.mod` were not modified.
- Added a package-level `registryMu sync.RWMutex` in the local fork. `AddImpl` protects free-slot lookup/reuse and append under the write lock; `IUnknownComObj.Impl` protects registry lookup under the read lock; final release runs `OnComObjFree` and `Finalize` before taking the write lock to clear the slot and return it to the free list, then calls `HeapFree` after unlocking. This keeps user lifecycle hooks outside the registry lock to avoid re-entrant deadlock.
- Audited all `impls` and `freeImplSlots` references in the fork: they are limited to declarations and the three protected paths above. The lock protects slice access; it does not make an invalid COM use-after-final-release legal, and callers must still obey COM reference ownership.
- Added `TestConcurrentIndependentComObjectLifecycle` (8 goroutines × 250 independent create/AddRef/Release/final-release cycles) to exercise registry reuse and finalization after the patch.
- Validation from this strategy directory on Windows/amd64: `go test .`, `go vet .`, `go test -race .`, `go test ./...`, and `go vet ./...` all passed. `GOARCH=386 go test -c` also compiled successfully; the 386 test executable was not run. A focused diff review against upstream confirmed the fork's only `impl.go` changes are the registry mutex and locks at `AddImpl`, `Impl`, and final-release slot removal. This is evidence against the tested Go data race in this fixture, not proof of all framework concurrency semantics, callback ABI, reentrancy, COM apartment rules, or external TSF-host lifetime behavior.
- **Status remains blocked/experimental.** Next: review the fork diff against upstream and add focused slot-reuse/finalization tests; then independently validate native callback dispatch and complete the transitive ABI audit before considering class-factory activation. No TSF activation, registration, installer, or system change was performed.

## Follow-up experiment: native C caller of the processor vtable (2026-10-09)

- Added `native_vtable_probe.go`, a cgo helper whose C code reads the actual five-slot `ITfTextInputProcessor` vtable and invokes `QueryInterface`, `AddRef`, `Release`, `Activate`, and `Deactivate` as native function-pointer calls. It is a test-only probe; it does not register a COM class or start TSF.
- Added `TestNativeCCallerInvokesProcessorVtable`. On Windows/amd64, native C successfully calls into the Go `syscall.NewCallback` thunks. The test checks successful `QueryInterface` identity, expected reference-count observations (3 after native AddRef and 2 after native Release, with the queried reference then released), successful activation/deactivation HRESULTs, and finalization after the owning reference is released.
- Validation from `strategy-generated-bindings`: `go test -count=1 .`, `go test -race -count=1 ./...`, and `go vet ./...` passed. Separately, `go test -race -count=1 ./com` passed from nested `third_party/go-com`.
- This is independent native-to-Go vtable dispatch evidence for the tested Windows/amd64 ABI, including all five slots. It does not validate Windows/386 runtime behavior, full TSF parameter/dependency closure, class-factory activation, TSF apartment/reentrancy behavior, concurrent native calls against one object, or real-host text commit. The C helper's calling-convention claim is explicitly limited to amd64; do not extrapolate to 386.
- The strategy remains **blocked/experimental**. Next: complete the transitive declaration and parameter-ownership audit against `um/msctf.h`/`um/msctf.idl`, including the `ITfThreadMgr` parameter's real interface declaration and architecture assumptions. Only after those gates should class-factory/TSF-host work be considered.

## SDK parameter closure check (2026-10-09)

- Re-inspected installed Windows SDK 10.0.22000.0 at `um/msctf.idl:496` and `um/msctf.h:1026`: `TfClientId` is a `DWORD` alias, therefore a 32-bit by-value integer in the processor `Activate` signature.
- `um/msctf.idl:534` declares `ITfThreadMgr : IUnknown`; `um/msctf.h:52` provides the forward declaration and `um/msctf.h:1064-1084` begins its full interface. The processor declaration at `um/msctf.idl:2355-2361` and `um/msctf.h:10500-10509` takes `ITfThreadMgr *ptim` as `[in]` and `TfClientId tid` by value, followed by `Deactivate()`; both methods return `HRESULT` and use `STDMETHODCALLTYPE` in the header.
- For the current processor-only fixture, `ITfThreadMgr` is passed only as an opaque interface pointer and never dereferenced, so a forward/opaque wrapper is sufficient to model this parameter's pointer shape. It is **not** a complete `ITfThreadMgr` binding and must be expanded before any implementation calls thread-manager methods. If production `Activate` retains `ptim` beyond the call, it must follow COM ownership rules (take a reference and release it during deactivation/finalization); the fixture currently does not retain it.
- Architecture boundary: native C vtable calls were run only on Windows/amd64 with `x86_64-w64-windows-gnu`. The amd64 Windows ABI uses the platform's unified x64 calling convention, so this experiment cannot independently validate 32-bit `stdcall` dispatch. The earlier `GOARCH=386 go test -c` evidence is compile-only and predates the cgo probe; no 386 executable was run.
- Audit status remains partial: the used `ITfTextInputProcessor` IID, inheritance, slot order, method parameter widths/direction, and return width are now traced to SDK declarations, and native dispatch is exercised on amd64. Ownership/lifetime behavior for a real TSF-provided thread manager, full `ITfThreadMgr` method declarations, 386 runtime ABI, class-factory activation, and TSF-host behavior remain open.

## Expanded SDK signature/dependency audit (2026-10-09)

Authoritative source is the installed Windows 10 SDK 10.0.22000.0 (`um/msctf.idl` and `um/msctf.h`). Recheck these line references if the SDK version changes.

| Interface/type | SDK declaration | ABI-relevant mapping | Ownership / remaining boundary |
|---|---|---|---|
| `TfClientId` | `msctf.idl:496`; `msctf.h:1026` | `DWORD`, 32-bit unsigned by value; fixture uses `uint32` | Scalar; no pointer ownership |
| `ITfThreadMgr` | `msctf.idl:534-560`; full header interface begins near `msctf.h:1064` | Direct `IUnknown` inheritance; 3 inherited slots followed by 10 methods in IDL order | `Activate` receives `[in] ITfThreadMgr*`. Current fixture only transports the pointer. Any implementation retaining it after `Activate` must `AddRef` and release during `Deactivate`/finalization. Full binding remains absent. |
| `ITfKeystrokeMgr` | `msctf.idl:2154-2216`; IID `aa80e7f0-2021-11d2-93e0-0060b067b86e` | Direct `IUnknown`; `AdviseKeyEventSink(TfClientId, ITfKeyEventSink*, BOOL)` and `UnadviseKeyEventSink(TfClientId)` lead the method order; `WPARAM`/`LPARAM` are pointer-sized on amd64 and `BOOL` is 32-bit | The `[in]` sink is a COM interface; registration must be paired with unadvise before deactivation. No generated binding or runtime test yet. |
| `ITfKeyEventSink` | `msctf.idl:2220-2253`; IID `aa80e7f5-2021-11d2-93e0-0060b067b86e`; matching C++ signatures at `msctf.h:9893-9929` | Direct `IUnknown`; six methods in order: `OnSetFocus(BOOL)`, `OnTestKeyDown/Up(ITfContext*, WPARAM, LPARAM, BOOL*)`, `OnKeyDown/Up(...)`, `OnPreservedKey(ITfContext*, REFGUID, BOOL*)`; each returns `HRESULT` | Context/GUID pointers are borrowed `[in]` parameters for callback duration; `BOOL*` is `[out]` and must be written on success. Key arguments are pointer-sized on amd64. No callback vtable implemented yet. |
| `ITfEditSession` | `msctf.idl:1393-1404`; IID `aa80e803-2021-11d2-93e0-0060b067b86e`; header `msctf.h:5630-5637` | Direct `IUnknown`; one method `DoEditSession(TfEditCookie)` returning `HRESULT`; `TfEditCookie` is a `DWORD` (`msctf.h:1020`), so `uint32` | Cookie is scalar and valid only under TSF edit-session rules. Actual text mutation is untested. |
| `ITfTextInputProcessor` | `msctf.idl:2349-2361`; header `msctf.h:10500-10509`; IID `aa80e7f7-2021-11d2-93e0-0060b067b86e` | Direct `IUnknown`; `Activate(ITfThreadMgr*, TfClientId)` then `Deactivate()`, both `HRESULT STDMETHODCALLTYPE` | Existing fixture covers only this interface shape. Native C dispatch validates its five-slot vtable on Windows/amd64, not TSF activation. |

### Cross-interface conclusions

- These interfaces directly inherit `IUnknown`; each vtable starts with `QueryInterface`, `AddRef`, and `Release`. Interface-specific method order must match the SDK exactly.
- On the required Windows/amd64 target, pointers, `WPARAM`, and `LPARAM` are 64-bit; `DWORD`, `BOOL`, `TfClientId`, `TfEditCookie`, and `HRESULT` are 32-bit. Preserve the 32-bit `HRESULT` bit pattern across callback boundaries; the existing `E_FAIL` round-trip is native amd64 evidence.
- The extracted file is not dependency-closed for key events/edit sessions. It does not declare `ITfKeystrokeMgr`, `ITfKeyEventSink`, `ITfContext`, or `ITfEditSession` vtables. Do not add these by hand without a reproducible extraction/source trail and per-method tests.
- IDL `[in]` interface pointers are borrowed for the call unless the implementation explicitly takes a COM reference for later use. `[out]` pointers/BOOLs must be initialized on every success path. Runtime ownership and host callback timing still need tests.
- This is a signature and scalar/pointer ownership audit, not proof of TSF threading/apartment behavior, reentrancy safety, real thread-manager reference management, sink unregistration order, edit-session scheduling, or text commit. The class-factory/TSF activation gate is **not passed**.

## Closed declaration subset (2026-10-09)

`cmd/closure-extract` now copies a symbol-rooted closure across the generated namespace. The committed result is `closedtsf/declarations.go` (5,872 bytes), produced from the pinned generator checkout `output/win32` with roots for `ITfTextInputProcessor`, `ITfThreadMgr`, `ITfKeystrokeMgr`, `ITfKeyEventSink`, and `ITfEditSession` (each struct, interface, vtable, and IID). Two runs were byte-identical. The file is a real `package closedtsf` and `go test ./closedtsf` compiles it on this machine's Windows/amd64 toolchain. It does not include the ~354 KB `UI.TextServices.go`.

The subset copies original generated type and value declarations. It does not copy method bodies. `ITfThreadMgr` is the generated struct embedding `IUnknown`, plus `ITfThreadMgrInterface` and `ITfThreadMgrVtbl`; it is no longer an opaque placeholder. Parameter types whose own method sets are not named by a selected signature (`ITfDocumentMgr`, `IEnumTfDocumentMgrs`, `ITfFunctionProvider`, `IEnumTfFunctionProviders`, `ITfCompartmentMgr`, `ITfContext`) are included only as the generated `IUnknown` embeddings the signatures point at. Their method interfaces were not rooted and are not in this file.

Unkeyed `syscall.GUID` literals are rewritten to keyed `Data1`..`Data4` fields. The IID bytes are the generator's values and match the SDK UUIDs below. `go vet` rejects the original unkeyed form.

### SDK trace for declarations in the closed file

SDK root: `C:\Program Files (x86)\Windows Kits\10\Include\10.0.22000.0`. Target: Windows/amd64, pointer width 8. `HRESULT`, `BOOL`, `DWORD`, `UINT`, `ULONG`, `TfClientId`, and `TfEditCookie` are 32-bit. `HWND`, `WPARAM`, `LPARAM`, `PWSTR`, and `BSTR` are pointer-sized. `closedtsf/layout_test.go` checks these widths, the five IID byte patterns, and vtable field order after the three `IUnknown` slots.

| Declaration | Generated origin | SDK origin | amd64 shape and ownership |
|---|---|---|---|
| `BOOL` | `Foundation.go` | `shared/minwindef.h:157` `typedef int BOOL` | 32-bit. `[in]` by value; `[out] BOOL*` must be written on success. |
| `HRESULT` | `Foundation.go` | `shared/winerror.h:29711` `typedef long HRESULT` | 32-bit return on every method below. |
| `HWND` | `Foundation.go` | `shared/windef.h:39` `DECLARE_HANDLE(HWND)` | 64-bit handle. `AssociateFocus` takes it `[in]`; not retained. |
| `WPARAM`, `LPARAM` | `Foundation.go` | `shared/wtypes.h:151`, `:160` (`UINT_PTR`, `LONG_PTR`) | 64-bit by-value key arguments. |
| `PWSTR` | `Foundation.go` | `um/winnt.h:489` `WCHAR *PWSTR` | Pointer. `PreserveKey` / `SetPreservedKeyDescription` `pchDesc` is `[in, size_is(cchDesc)]`. |
| `BSTR` | `Foundation.go` | `shared/wtypes.h:737` `OLECHAR *BSTR` | Pointer. `GetPreservedKeyDescription` `[out] BSTR*` is caller-freed on success. |
| `TF_PRESERVEDKEY` | `UI.TextServices.go` | `um/msctf.h:9596-9600` `UINT uVKey; UINT uModifiers` | Two 32-bit fields, 8 bytes. Pointers are borrowed `[in]`. |
| `IUnknown` | `System.Com.go` | COM `IUnknown`: `QueryInterface`, `AddRef`, `Release` | Three `uintptr` slots. `AddRef`/`Release` results are 32-bit counts in the generated signature. |
| `ITfThreadMgr` | `UI.TextServices.go` | `um/msctf.idl:534-560`; IID `aa80e801-2021-11d2-93e0-0060b067b86e` | Direct `IUnknown`, then 11 methods in IDL order. `Activate` `[out] TfClientId*`. Document-manager, function-provider, and compartment pointers are `[in]` or `[out]` as in the IDL; an `[out]` interface must be released by the caller. Processor `Activate` receives `[in] ITfThreadMgr*` and must `AddRef` only if it keeps the pointer. |
| `ITfKeystrokeMgr` | `UI.TextServices.go` | `um/msctf.idl:2162-2215`; `um/msctf.h:9618+`; IID `aa80e7f0-2021-11d2-93e0-0060b067b86e` | Direct `IUnknown`, then 14 methods in IDL order. `AdviseKeyEventSink` `[in]` sink must be paired with `UnadviseKeyEventSink` before the sink is released. |
| `ITfKeyEventSink` | `UI.TextServices.go` | `um/msctf.idl:2226-2253`; IID `aa80e7f5-2021-11d2-93e0-0060b067b86e` | Direct `IUnknown`, then six methods in IDL order. Context and GUID pointers are borrowed for the call. `BOOL*` is `[out]`. |
| `ITfEditSession` | `UI.TextServices.go` | `um/msctf.idl:1401-1404`; IID `aa80e803-2021-11d2-93e0-0060b067b86e` | Direct `IUnknown`, then `DoEditSession([in] TfEditCookie)`. Cookie is a 32-bit `DWORD` (`um/msctf.h:1020`, `um/msctf.idl:490`). |
| `ITfTextInputProcessor` | `UI.TextServices.go` | `um/msctf.idl:2355-2361`; `um/msctf.h:10500-10509`; IID `aa80e7f7-2021-11d2-93e0-0060b067b86e` | Direct `IUnknown`, then `Activate([in] ITfThreadMgr*, [in] TfClientId)` and `Deactivate()`. |
| `TfClientId` | represented as `uint32` in generated signatures | `um/msctf.idl:496`; `um/msctf.h:1026` `DWORD` | 32-bit by value. |

`ITfDocumentMgr`, `IEnumTfDocumentMgrs`, `ITfFunctionProvider`, `IEnumTfFunctionProviders`, `ITfCompartmentMgr`, and `ITfContext` are present only as generated structs embedding `IUnknown`. Calling their methods requires a later rooted extraction of those interface and vtable symbols. This audit does not treat those embeddings as full bindings.

This closes declaration provenance and amd64 layout for the included symbols. It does not pass class-factory activation, TSF-host activate/deactivate, key-sink registration, edit-session text commit, or production registration. Strategy D remains blocked/experimental.
