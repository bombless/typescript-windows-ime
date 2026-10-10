# Strategy D: Go COM implementation with generated Windows bindings

## Target environment and scope

**Target is this development machine only: Windows/amd64 (x86-64).** Cross-architecture support is not a requirement for this strategy. Do not spend time building or running a Windows/386 harness unless the target requirement changes. Use this machine's native GOOS, GOARCH, compiler, and SDK as the acceptance environment; record those values with test evidence. Historical 386 compile-only notes below are background evidence, not an open acceptance gate.


## Goal
Use generated or maintained Go Windows API/interface declarations to reduce manual ABI risk while implementing the TSF COM server in Go.

## Core idea
- Evaluate Windows bindings such as `go-win32api` for the exact TSF interfaces, GUIDs, and declarations required.
- Implement the COM server in Go using generated declarations as the source of truth for signatures and layouts.
- Keep the key reducer and IME policy in Go; no C++ adapter in the target architecture.
- Generate only what is needed, or isolate generated dependencies so updates are reproducible.

## Work packages
1. Check whether the selected binding source contains `ITfTextInputProcessor`, `ITfKeyEventSink`, keystroke-manager interfaces, and edit-session declarations.
2. Verify declarations against Microsoft documentation and Windows SDK headers, including inherited vtable order and callback signatures.
3. Determine whether the bindings support implementing COM objects, not merely calling COM methods.
4. Build a minimal activation prototype, then add key-event registration and text commit.
5. Pin generator/tool versions and document regeneration and review procedures.
6. Run real TSF tests and compare complexity with Strategies A and B.

## Acceptance criteria
- Used declarations have traceable authoritative sources and repeatable generation or review steps.
- Go can implement and expose required COM interfaces; bindings are not merely client-side wrappers.
- Activation, key handling, edit sessions, and commit work in a real Windows text service.
- Generated-code updates cannot silently alter ABI-critical declarations without tests or review.

## Main risks
- A binding package may declare TSF interfaces for calling them but provide no mechanism to implement COM objects in Go.
- TSF coverage may be incomplete or stale, requiring manual declarations anyway.
- Generator output may increase build complexity and make debugging vtables harder.

## First experiment
Produce a coverage report listing each required TSF interface/method, its binding source, and whether Go-side implementation is supported. Do not assume interface declarations imply server-side implementation support.

## Current status (2026-10-09)

The first experiment produced `COVERAGE.md` and the read-only discovery script `Verify-Coverage.ps1` in this directory. The installed Windows SDK 10.0.22000.0 contains the four required interfaces in `um/msctf.h`; `COVERAGE.md` records the follow-up scan, IIDs, and validation evidence.

The existing Go module checks passed from `native/golang`: `go test ./...` (cached success) and `go vet ./...` (success).

**Follow-up on 2026-10-09:** the local module cache does contain `github.com/zzl/go-win32api/v2@v2.0.1`. A scan of that exact source tree found no text matches for the four TSF interfaces. Requests to `proxy.golang.org` still fail, so a current version could not be inspected. This does not establish that no upstream version or custom generation can provide the interfaces. SDK/IDL spot-checks established the four IIDs and direct `IUnknown` inheritance, but the full ABI audit and server-side implementation remain unproven. No dependency was added or pinned. No TSF activation or real text-input tests were run. No system registration/installation was performed.

## Next-session handoff (refreshed 2026-10-09)

**Purpose:** resume in a new ChatGPT context without repeating the completed investigation. Read `COVERAGE.md` for evidence and this section for the actionable queue.

### What is now known

- `github.com/zzl/go-win32api/v2` has tags `v2.0.0`, `v2.0.1`, `v2.0.2`, `v2.1.0`, and `v2.2.0`. Exact `v2.2.0` source was downloaded to the local Go module cache for inspection only (origin commit `56d319e1869e40dd106b4056332d599a49d33eda`; checksums in `COVERAGE.md`).
- `Verify-Coverage.ps1` was run against both the cached v2.0.1 source and v2.2.0. The SDK declarations were found, but neither inspected binding tree produced text matches for `ITfTextInputProcessor`, `ITfKeyEventSink`, `ITfKeystrokeMgr`, or `ITfEditSession`. This establishes a gap in those inspected trees, not a claim about every possible generator/source.
- The installed SDK `um/msctf.idl` was reviewed for method order and core signatures. The four interfaces directly inherit `IUnknown`. Full ABI verification is still open: architecture-sensitive types, calling convention, generated Go method layout, and object/callback lifetime have not been proven.
- `go-com@v1.5.0` provides generic COM implementation/class-factory patterns but does not fill the TSF declaration gap.
- `go test ./...` and `go vet ./...` passed from `native/golang` and `strategy-manual-com`; `go test ./...` passed from `strategy-go-com`. These are existing-code checks, not TSF runtime acceptance.
- **Current decision: no-go for using `go-win32api/v2@v2.2.0` as-is. Strategy D remains experimental/blocked.** No production TIP registration, installer, or system change was performed. No project module files were changed.

### Next-session instructions — start here, do not repeat completed scans

**Current branch of investigation:** the namespace-level generator experiment succeeded. The next task is no longer to search the v2.2.0 binding tree or merely prove that a generator exists. Continue with a narrowly scoped generated declaration and a compile-only Go COM-server proof.

#### Completed evidence to reuse

- `go-win32api/v2@v2.0.1` and `v2.2.0` were scanned; the four required TSF interface names were absent from those inspected binding trees. Do not repeat these scans unless the candidate changes.
- SDK `um/msctf.idl` IIDs, direct `IUnknown` inheritance and method order were spot-checked. This is not a complete ABI audit.
- `zzl/go-winapi-gen` commit `4fd990193d56c9ea8fb18ece1adac5016c7dcbc4` was downloaded to `%TEMP%\strategy-d-generator-review\go-winapi-gen-main`. Its bundled `assets/Windows.Win32.winmd` can generate `UI.TextServices.go` after adding `UI.TextServices` to the namespace allowlist in the temporary checkout.
- Two runs produced identical output (SHA-256 in `COVERAGE.md`). The generated file is about 354 KB and includes many unrelated declarations. The generator's filter is namespace-level, not interface-level.
- The generator emits client wrappers using `syscall.SyscallN`; it does not generate server-side COM callback thunks or implementation objects. `go-com@v1.5.0` has generic COM implementation patterns, but compatibility with generated TSF interfaces has not been proven.
- Full evidence, source revision, hashes, and caveats are in `COVERAGE.md`.

#### Ordered next actions

1. **Inspect how to extract a minimal dependency-closed declaration set.** Work in a fresh temporary copy or an isolated subdirectory. Prefer extending the generator/model pipeline or a deterministic post-generation extractor over manually rewriting signatures. Target only `IUnknown` plus `ITfTextInputProcessor` for the first proof; include only genuinely required referenced types/constants. Record exact scripts, inputs, and outputs. Do not copy the 354 KB file into the strategy yet.
2. **Create an isolated compile fixture in this strategy directory.** Only after the minimal output is reproducible, add a local `go.mod`/`go.sum` here if needed; pin the generator commit, metadata asset hash, Go version, and `go-com` version. Never modify `native/golang/go.mod`. Compile a minimal implementation that exposes `ITfTextInputProcessor` through `go-com@v1.5.0`. If the generated interface shape is incompatible, record the exact mismatch rather than papering over it.
3. **Prove callback/vtable generation and COM identity.** Determine whether a small reusable generator extension can emit server-side vtable callback thunks in the required Windows calling convention. Test `QueryInterface`, `AddRef`/`Release`, inherited `IUnknown` slots, method dispatch, and object lifetime. A compile-only success is not runtime ABI proof.
4. **Complete the ABI audit before exercising TSF.** Cross-check every used declaration against installed SDK `um/msctf.h` and `um/msctf.idl`: IID, signatures, parameter direction, pointer width, `HRESULT`, `BOOL`, `WPARAM`/`LPARAM`, vtable order, calling convention and ownership/lifetime. Record 32-/64-bit assumptions explicitly.
5. **Only if the minimal server proof passes, add behavior incrementally.** First activate/deactivate with repeated and failure-path checks and documented COM apartment/threading assumptions; next implement key sink registration/unregistration and callbacks; then implement `ITfEditSession.DoEditSession` and verify actual text commit inside a TSF host. Ensure callbacks cannot outlive deactivation.
6. **Run checks and compare strategies.** Run isolated fixture tests, then `go test ./...` and `go vet ./...` in applicable Go directories. Compare effort/risk with `strategy-go-com` and `strategy-manual-com`; keep Strategy D blocked if narrow generation or server-side callback generation becomes disproportionate.
7. **Only claim acceptance after real-host evidence.** Required: activation/deactivation, key handling, edit-session text commit, lifetime/release checks, reproducible regeneration and reviewable ABI diffs. Until then status remains experimental/blocked.

#### Guardrails

- Keep all generated files, `go.mod` and `go.sum` inside `strategy-generated-bindings`; do not change `native/golang/go.mod`.
- Do not register the production TIP, run `native/install-user-golang.ps1`, or modify system configuration.
- Do not add a C++ fallback unless a documented experiment proves it necessary.
- `Verify-Coverage.ps1` is text discovery only, not ABI proof.
- Do not add the large generated TextServices file or adopt the generator as a project dependency before the narrow-output and server-implementation gates pass.

## Current acceptance checklist

- [x] Coverage report and repeatable read-only discovery script created.
- [x] Required interface names located in the installed Windows SDK header.
- [x] Existing Go module unit tests pass from `native/golang`.
- [x] Cached `go-win32api/v2@v2.0.1` source scanned; no required TSF interface text matches found (upstream/current coverage remains unknown).
- [ ] All used declarations checked against SDK/IDL for GUIDs, signatures, and vtable order; only IID/inheritance spot-check is complete.
- [ ] Go-side COM implementation compiled and `IUnknown` behavior tested.
- [ ] Minimal activation/deactivation and lifetime tests pass.
- [ ] Key-event registration and callbacks pass.
- [ ] Edit-session text commit passes in a real TSF host.
- [ ] Generator/toolchain pinned and regeneration/review process repeatable.
- [x] Strategy A/B comparison documented and go/no-go decision recorded (2026-10-09 follow-up; Strategy D remains blocked/experimental on the inspected candidate).

## Latest execution checkpoint (2026-10-09)

- Network source inspection succeeded through the local proxy at `127.0.0.1:7897`. Available `go-win32api/v2` tags were `v2.0.0`, `v2.0.1`, `v2.0.2`, `v2.1.0`, and `v2.2.0`. Inspected `v2.2.0` without adding it to any project module; its source still had no text matches for the four required TSF interfaces. Module checksum and origin commit are recorded in `COVERAGE.md`.
- Inspected `msctf.idl` method order and signatures for all four interfaces at declaration level. The matrix is now more specific, but this is not a complete ABI audit: architecture-sensitive type mapping, exact generated Go declarations, calling convention, COM callback implementation and object lifetime are not proven.
- `go test ./...` and `go vet ./...` passed from `native/golang`; the same checks passed from `strategy-manual-com`; `go test ./...` passed from `strategy-go-com`. No TSF runtime tests were performed.
- Decision: **no-go for using `go-win32api/v2@v2.2.0` as-is**. Keep Strategy D experimental/blocked until a repeatable, narrowly scoped generation route for TSF interfaces is demonstrated. The generic `go-com@v1.5.0` implementation framework alone does not fill the TSF declaration gap. Compare Strategy A's current `E_NOINTERFACE` activation blocker and Strategy B's pre-server ABI/callback experiments before investing in custom generation.
- No `native/golang/go.mod` edit, no isolated strategy module/dependency added, no production TIP registration, and no installer/system modification. `COVERAGE.md` contains the detailed execution evidence and next experiment.

## Scope boundaries
- No C++ fallback in the target architecture unless a documented experiment proves it necessary.
- Do not generate or import unrelated Win32 APIs merely because the package offers them.

## Latest execution checkpoint (2026-10-09, generator source experiment)

- Inspected `zzl/go-winapi-gen` at commit `4fd990193d56c9ea8fb18ece1adac5016c7dcbc4`. Its bundled WinMD can produce the required four TSF declarations when `Windows.Win32.UI.TextServices` is added to its namespace allowlist.
- Two generation runs produced byte-identical `UI.TextServices.go` output (SHA-256 recorded in `COVERAGE.md`), proving repeatability for that checked-in metadata asset and generator source.
- Limitation: the generator only filters by namespace and emits client-side `SyscallN` wrappers; it does not generate COM callback thunks or server-side implementation objects. The generated namespace file is ~354 KB, so the required narrow-output route is not yet proven.
- **Next immediate task:** create a reproducible extraction/codegen experiment that emits only `IUnknown` plus `ITfTextInputProcessor`, then compile a minimal Go-side implementation using `go-com@v1.5.0` inside this strategy directory. Keep Strategy D blocked until callback ABI and lifetime are proven. No production registration, installer, or system configuration was touched.

## Latest execution checkpoint (2026-10-09, minimal extraction fixture)

- Added `Extract-Minimal.ps1`, a deterministic post-generation extractor that selects the `ITfTextInputProcessor` IID/interface/vtable/client-wrapper block from the previously generated `UI.TextServices.go`. It fails closed if the expected IID or following interface boundary is absent and normalizes the generated unkeyed `syscall.GUID` literal to keyed fields because `go vet` rejects the original literal; IID fields/bytes are unchanged.
- Generated `itf_text_input_processor_generated.go` from the temporary generator output. Added a local `go.mod`/`go.sum` under this strategy directory; `native/golang/go.mod` remains untouched. `go test ./...` and `go vet ./...` both pass for the extracted declaration shape, IID and vtable-size checks.
- Follow-up refinement: `Extract-Minimal.ps1` now emits `ITfThreadMgr` as a distinct opaque struct embedding `IUnknown`, rather than a direct alias. `fixture_test.go` guards against the alias regression. This remains a pointer-target placeholder, not a dependency-closed semantic declaration.
- `go test .` and `go vet .` passed after regeneration when run from an explicit PowerShell `Set-Location` to this strategy directory. A wrapper invocation that did not actually enter the directory failed with a Go module-root error; rerunning from the explicit directory passed.
- Inspected `go-com@v1.5.0` source and examples. Server-side implementation uses explicit COM object wrappers and `syscall.NewCallback` vtable entries; compatibility with this generated TSF interface is not automatic. The custom processor callback/vtable bridge and COM identity/lifetime remain unproven.
- Decision remains **blocked/experimental**. Next: implement a narrow custom processor COM-object/vtable fixture against `go-com@v1.5.0`, then test `QueryInterface`, `AddRef`/`Release`, inherited slots, dispatch and lifetime before moving to full dependency closure and TSF activation. No TSF activation, key sink, edit session, real-host test, production registration, installer or system change was performed.

## Latest execution checkpoint (2026-10-09, local COM vtable fixture)

- Added `github.com/zzl/go-com v1.5.0` to this strategy's isolated `go.mod` and refreshed `go.sum`; `native/golang/go.mod` remains untouched.
- Added `processor_com_fixture_test.go`: a custom `ITfTextInputProcessorVtbl` uses `go-com`'s `IUnknownComObj` callbacks for inherited `IUnknown` slots and local `syscall.NewCallback` thunks for `Activate`/`Deactivate`. The test invokes generated interface wrappers, checks `QueryInterface` for both the TSF IID and `IID_IUnknown`, verifies returned interface identity, releases queried references, and checks both method dispatch paths.
- Validation from this directory: `go test .` passed and `go vet .` passed. This is a local in-process fixture only; it does not establish cross-architecture ABI correctness, concurrent/lifetime safety under TSF, COM apartment correctness, or compatibility with a real TSF host.
- **Status remains blocked/experimental.** `ITfThreadMgr` is still an opaque signature placeholder; no dependency-closed declaration set or full per-parameter SDK ABI audit exists. The original fixture did not test final-release behavior after all references are gone, activation by COM class factory, key sink registration, edit sessions, or text commit. The next-session run strengthened `processor_com_fixture_test.go`: it now checks `AddRef`/`Release` counts, `QueryInterface` identity for the TSF IID and `IUnknown`, unsupported-IID rejection, method dispatch, and final `Release` invoking the COM wrapper's `Finalize` hook. `go test .` and `go vet .` pass from this directory. SDK `msctf.idl` spot-check confirms `ITfTextInputProcessor::Activate(ITfThreadMgr*, TfClientId)` and `Deactivate()` order; `TfClientId` is a `DWORD` alias, represented as `uint32` in the fixture. This is still a 64-bit local fixture, not a complete ABI audit: callback calling convention, 32-bit behavior, error HRESULT round-tripping, concurrent lifetime, class-factory activation, key sink registration, edit sessions, and real-host text commit remain unproven. Do not register/install the production TIP. Next: audit callback ABI and the complete declaration dependency closure against SDK headers, then add class-factory/activation proof only if the audit passes.

## Latest execution checkpoint (2026-10-09, ABI layout assertions)

- Added explicit tests for processor vtable method offsets (after the three inherited `IUnknown` slots), fixed-width 32-bit `HRESULT`, and the 32-bit `TfClientId`/`DWORD` mapping.
- `go test .` and `go vet .` pass on Windows/amd64. `GOARCH=386 go test -c` also passes, but the 32-bit test executable was not run.
- This improves compile/layout evidence only. The callback calling convention and error-HRESULT round-trip remain unproven, as do apartment/threading, reentrancy, and concurrent lifetime behavior. `ITfThreadMgr` remains an opaque pointer placeholder, not a dependency-closed declaration.
- Next: inspect the generator's type model/WinMD for the precise transitive declarations needed by `ITfTextInputProcessor`; audit callback ABI and lifetime contract before attempting class-factory activation. Keep Strategy D blocked until the callback proof and then real-host TSF acceptance gates pass. No TSF activation, production registration, installer or system change was performed.

## Latest execution checkpoint (2026-10-09, HRESULT callback round-trip)

- Rechecked installed SDK 10.0.22000.0 `um/msctf.idl`: `ITfTextInputProcessor` directly inherits `IUnknown`; `Activate(ITfThreadMgr*, TfClientId)` and `Deactivate()` return `HRESULT`; `TfClientId` is a `DWORD` alias. The fixture's method ordering and fixed-width client ID match these declarations; `ITfThreadMgr` remains an opaque pointer target, not a dependency-closed interface declaration.
- Inspected `go-com@v1.5.0` COM implementations: vtable callback entries are constructed with `syscall.NewCallback`, with callback methods returning `uintptr`. This validates consistency with the library's general pattern, not full TSF ABI correctness or 32-bit runtime behavior.
- Extended the local fixture to test `E_FAIL` round-tripping through the Activate/Deactivate callback thunk and generated wrapper, checking that failure does not mutate activation state and that subsequent success calls still work.
- Validation passed from this strategy directory: `go test .`, `go vet .`, and `GOARCH=386 go test -c`. The 386 artifact was not executed. This is local in-process amd64 runtime evidence plus 386 compile evidence only.
- Remaining gates: independently validate callback calling convention and external/native dispatch on 32-/64-bit Windows; close transitive declarations and per-parameter ABI audit; establish thread/apartment, reentrancy, and concurrent lifetime behavior; then consider class-factory activation. Real TSF host activation, key sink registration, edit sessions and text commit are still untested. Strategy D remains blocked/experimental. No production TIP registration, installer or system change was performed.

## Latest execution checkpoint (2026-10-09, callback ABI audit)

- Added `ABI-AUDIT.md` with traceable Windows SDK 10.0.22000.0 IDL/header locations for `ITfTextInputProcessor`, its IID, `IUnknown` inheritance, method order, and `HRESULT STDMETHODCALLTYPE` declarations.
- Checked the active Go toolchain (`go1.27.0 windows/amd64`) documentation for `syscall.NewCallback`: it creates Windows stdcall callbacks, requires a `uintptr`-sized return, limits argument sizes to pointer width, and callback memory is not released. The fixture's callback signatures fit those documented constraints on amd64. This is not independent proof of external/native COM dispatch or 386 runtime correctness.
- Current local tests establish only in-process amd64 wrapper dispatch, `QueryInterface` identity, reference-count transitions/finalization, and `S_OK`/`E_FAIL` round trips. The 386 build remains compile-only. `ITfThreadMgr` remains an opaque signature placeholder; no TSF host or class-factory activation was tested.
- Inspected pinned `go-com@v1.5.0` `com/impl.go`: the per-object refcount uses atomics, but `AddImpl` and final-release `free()` mutate the global `impls`/`freeImplSlots` slices without synchronization; `MuVtbl` only protects vtable initialization. Concurrent object creation/final release can therefore race on the global registry. This is a concrete additional blocker; do not stress-test the unsafe allocator path and assume a pass means safety.
- Recorded the risk in `ABI-AUDIT.md`. Before any multithreaded TSF use, either serialize all object lifecycle operations or maintain a reviewed patch/fork that synchronizes the registry and then test it. A native callback caller harness remains useful for ABI evidence but cannot address this framework race.
- Next gate: choose and validate a safe COM object-lifetime strategy, then test external/native callback dispatch if feasible. Do not advance to TSF activation until both callback ABI and lifetime concerns are resolved. No production registration, installer, or system change was performed.

## Latest execution checkpoint (2026-10-09, lifetime mitigation boundary)

- Re-read the pinned `go-com@v1.5.0` `com/impl.go` directly. `AddImpl`, `IUnknownComObj.Impl`, and final-release cleanup all access the package-global `impls` registry; `AddImpl` and `free()` mutate both `impls` and `freeImplSlots` without a registry lock. `MuVtbl` is unrelated to these accesses.
- Re-ran the isolated fixture validation from this directory: `go test .` passed (cached) and `go vet .` passed. This confirms no regression in the existing single-threaded fixture, not thread safety.
- Added a follow-up section to `ABI-AUDIT.md` explaining why a mutex local to the processor wrapper cannot contain a package-global race and defining the next safe experiment: a strategy-local `go-com` fork/replacement with one registry mutex, audited access to both slices, lifecycle hooks outside the lock, slot-reuse/final-release tests, and race testing where supported.
- No dependency or module files were changed, no concurrency stress test was run against the known-racy allocator, and no class-factory activation, TSF activation, registration, installer, or system configuration change was performed.
- **Decision remains blocked/experimental.** Next action is to implement and review the isolated synchronized fork before external callback or class-factory tests; callback ABI, COM reference-lifetime contract, apartment/threading, and real-host TSF behavior remain open.
## Latest execution checkpoint (2026-10-09, isolated COM registry synchronization)

- Copied the exact cached `go-com@v1.5.0` source to `third_party/go-com` under this strategy and added a local `replace` directive. The upstream module cache and `native/golang/go.mod` remain unchanged.
- Patched the local fork with a package-level RW mutex covering all `impls` and `freeImplSlots` accesses: `AddImpl`, `IUnknownComObj.Impl`, and final-release slot removal/reuse. Lifecycle hooks and `HeapFree` remain outside the registry lock.
- Added a concurrent regression test with 8 workers × 250 independent COM object lifecycle cycles.
- From this directory on Windows/amd64, `go test .`, `go vet .`, `go test -race .`, `go test ./...`, and `go vet ./...` all passed. `GOARCH=386 go test -c` compiled successfully, but the 386 executable was not run. A focused diff review against upstream confirmed the fork's `impl.go` changes are limited to the registry mutex and locks at `AddImpl`, `Impl`, and final-release slot removal. The registry access scan found no other slice references outside those protected code paths.
- This addresses the identified registry-slice data race in the isolated fixture only. It does not establish external COM reference-lifetime correctness, native callback ABI, apartment/reentrancy behavior, full declaration dependency closure, or real TSF-host behavior.
- **Status remains blocked/experimental.** Next: review the local fork diff and add focused slot reuse/finalization tests, then independently validate native callback dispatch and finish the ABI/dependency-closure audit. Do not proceed to class-factory/TSF activation until those gates pass. No production TIP registration, installer, or system configuration change was performed.

## New-context handoff — start here (2026-10-09, latest)

**Do not repeat** the binding scans, generator repeatability run, or initial `go-com` registry-race discovery. Reuse `COVERAGE.md` and `ABI-AUDIT.md` for prior evidence.

### Latest repository state

- Strategy-local module: `strategy-generated-bindings/go.mod` uses `replace github.com/zzl/go-com => ./third_party/go-com`; the isolated fork is copied from cached `go-com@v1.5.0`. `native/golang/go.mod` and the upstream module cache were not changed.
- `third_party/go-com/com/impl.go` now has `registryMu sync.RWMutex`. `AddImpl` takes the write lock; `IUnknownComObj.Impl` takes the read lock; final-release slot removal/reuse takes the write lock. `OnComObjFree`, `Finalize`, and `HeapFree` execute outside that lock.
- `processor_com_fixture_test.go` includes `TestConcurrentIndependentComObjectLifecycle`: 8 goroutines × 250 create/refcount/final-release cycles.
- Final checks from this strategy directory on Windows/amd64 passed: `gofmt`, `go test -race ./...`, and `go vet ./...`. Earlier in the same run, `go test ./...` and `go vet ./...` also passed. `GOARCH=386 go test -c` compiles, but the 386 executable has not been run.
- The fork diff against upstream `com/impl.go` was reviewed and is limited to the registry mutex and locks around `AddImpl`, `Impl`, and final-release registry slot cleanup. An access scan found no other references to either registry slice.
- `ABI-AUDIT.md` contains the callback ABI evidence and the registry-race mitigation record. The current fixture remains in-process amd64 evidence, not native COM caller or TSF-host evidence.

### Next actions — execute in this order

1. Add focused regression tests for registry slot reuse and finalization ordering (including ensuring lifecycle hooks run outside the registry lock); review edge cases such as duplicate/free-slot handling and document any assumptions about concurrent final `Release` versus outstanding calls.
2. Inspect the complete local fork diff, including any other files that may need synchronization. Keep the patch minimal and document its upstream base revision/checksum for reproducibility.
3. Check for an available Windows native compiler/toolchain and build a tiny native caller/harness that invokes the COM vtable callbacks, if feasible. Verify `QueryInterface`, `AddRef`/`Release`, `Activate`/`Deactivate`, calling convention and HRESULT round-trip. Do not treat Go-to-Go wrapper calls as external ABI proof. If no usable native compiler is available, record the limitation and choose a defensible alternative test plan rather than claiming ABI validation.
4. Complete the transitive declaration closure and per-parameter audit against SDK 10.0.22000.0 `um/msctf.h` and `um/msctf.idl`: IID, all method signatures, pointer width, `HRESULT`, `DWORD/TfClientId`, parameter direction/ownership, vtable order and architecture assumptions. `ITfThreadMgr` is still only an opaque pointer placeholder.
5. Only after the fork/lifetime and callback ABI gates pass, consider a minimal class-factory activation prototype. Then incrementally test activate/deactivate, key-sink registration, edit sessions and text commit in a real TSF host. Keep production registration and installer out of scope.
6. Run `go test -race ./...` and `go vet ./...` after relevant changes on the target Windows/amd64 machine. Do not require 386 compile/runtime checks for this project scope. Update `ABI-AUDIT.md` and this plan with exact commands/results. Compare the cost/risk against Strategy A and B before expanding the generator.

### Hard guardrails / status

- Strategy D is still **blocked/experimental**, not accepted for production.
- Do not register/install the production TIP, run `native/install-user-golang.ps1`, or modify system configuration.
- Do not change `native/golang/go.mod`; keep all fork/module changes isolated here.
- Do not add a C++ fallback without a documented experiment proving it necessary.
- Passing race tests only validates tested Go synchronization paths; it does not prove COM external reference-lifetime correctness, callback ABI, apartment/reentrancy behavior, or real TSF compatibility.

## Latest execution checkpoint (2026-10-09, lifecycle reentrancy and native callback probe)

- Extended `processor_com_fixture_test.go` with `TestLifecycleHooksRunOutsideRegistryLock`. Both `OnComObjFree` and `Finalize` re-enter the COM registry by creating and releasing an independent child object; the test has a three-second timeout to catch lock-order deadlocks.
- Validation from this strategy directory on Windows/amd64 passed: `gofmt -w processor_com_fixture_test.go`, `go test -race ./...`, `go vet ./...`, and a `GOARCH=386 go test -c` compile check. The 386 executable remains unrun.
- Reviewed `third_party/go-com/com/impl.go` against the cached `github.com/zzl/go-com@v1.5.0` source. The functional fork changes remain the package registry RW mutex and locks around `AddImpl`, `Impl`, and final-release slot removal. The diff also drops one standalone comment line; no other fork files were changed. The source was copied from cached module `github.com/zzl/go-com` (`go 1.19`, requires `go-win32api/v2 v2.0.1`). Cached upstream `com/impl.go` SHA-256: `420F140420890570FE5E24C708EF65A0950E4065C8E158D09F52EF43B014EB16`; upstream `go.mod` SHA-256: `6E95D03FA1BC689952F8887ACEE688BC8AB3AF30697A889C0BBD3B97C2122DB2`. The upstream VCS revision is not encoded in the module cache and remains to be independently sourced.
- A native compiler is available through LLVM-MinGW (`gcc`, Windows amd64; `CGO_ENABLED=1`). A disposable C-to-Go callback harness passed: native C invoked a `syscall.NewCallback` stdcall thunk with two pointer-sized arguments and observed the exact `E_FAIL` bit pattern (`0x80004005`) on return. This is stronger than Go-wrapper-only invocation, but it is not yet a native COM vtable caller and does not test `QueryInterface`, `AddRef`/`Release`, activation or 386 runtime behavior.
- Remaining gaps: explicit finalization-order assertions beyond the lifecycle-hook re-entry check; full parameter/dependency ABI audit; native vtable caller exercising all methods; and all TSF-host acceptance checks. `ITfThreadMgr` remains an opaque pointer placeholder. Status remains **blocked/experimental**; no TSF activation, registration, installer, or system configuration change was performed.

## Latest execution checkpoint (2026-10-09, registry slot reuse)

- Added `third_party/go-com/com/impl_registry_test.go` with `TestAddImplReusesFreedSlotsInLIFOOrder` and `TestAddImplRejectsOccupiedFreeSlot`. The tests isolate and restore the fork's registry globals, assert deterministic LIFO reuse and registry contents, and verify the occupied-slot invariant panics instead of silently overwriting an active implementation.
- `gofmt` completed. `go test -race -count=1 ./com` passed from `third_party/go-com`; the strategy root `go test -race ./...` and `go vet ./...` also passed. The fork's `go vet ./com` reports a pre-existing `possible misuse of unsafe.Pointer` warning in `com/context.go:64`. Running `go vet ./...` over the entire fork additionally flags pre-existing suspicious OR conditions in `ole/variant.go:1042` and `:1050`; these unrelated upstream files were not changed.
- The root `./...` wildcard does not traverse the nested fork module, so fork-package tests must be run separately from `third_party/go-com`.
- Next: build a native caller that invokes the actual processor COM vtable; then continue SDK declaration closure and parameter ownership audit. No production activation or system changes.

## Latest execution checkpoint (2026-10-09, native COM vtable caller)

- Added `native_vtable_probe.go`, a test-only cgo/native-C caller that invokes the actual `ITfTextInputProcessor` vtable slots for `QueryInterface`, `AddRef`, `Release`, `Activate`, and `Deactivate`.
- Added `TestNativeCCallerInvokesProcessorVtable`; native C successfully dispatched into the Go callback thunks on Windows/amd64, checked queried-interface identity and reference-count transitions, exercised both TSF methods, and confirmed finalization after the owning reference was released.
- Validation passed: `go test -count=1 .`, `go test -race -count=1 ./...`, `go vet ./...` from this strategy directory, and `go test -race -count=1 ./com` from `third_party/go-com`.
- This is native caller evidence for amd64 only, not proof for 386, class-factory activation, TSF host behavior, threading/apartment correctness, or real text commit. `ITfThreadMgr` remains an opaque pointer target and full dependency closure/parameter ownership audit remains open.
- Next: finish the SDK/IDL transitive declaration and ownership audit; do not proceed to class-factory activation until that audit and the architecture boundary are documented. Status remains **blocked/experimental**; no production registration, installer, or system change was performed.

## SDK signature follow-up (2026-10-09)

- Rechecked the installed SDK 10.0.22000.0: `TfClientId` is a `DWORD`; `ITfThreadMgr` is declared as an `IUnknown` interface; `ITfTextInputProcessor::Activate` receives `[in] ITfThreadMgr *` plus a by-value client ID, and `Deactivate` has no parameters. Both return `HRESULT` and the C++ header marks them `STDMETHODCALLTYPE`.
- The fixture's opaque `ITfThreadMgr` wrapper is adequate only for carrying an unused interface pointer; it is not a full binding. Any implementation retaining the thread manager must take/release a COM reference correctly.
- Native C vtable test remains amd64-only. Do not claim 386 calling-convention validation from the earlier compile-only artifact.
- Next: treat Windows/amd64 on this machine as the sole target; no 32-bit harness is required. Continue reviewing object ownership/threading and compare the cost of full `ITfThreadMgr` generation against Strategies A/B. Strategy D remains blocked/experimental.


## Target-scope decision (2026-10-09)

- Confirmed requirement: support only the current machine's Windows/amd64 environment. No Windows/386 or other architecture compatibility is required.
- Acceptance evidence must be collected on this machine using its native amd64 Go toolchain and native compiler. ABI review still must cover correct amd64 pointer widths, vtable order, callback behavior, HRESULT propagation, COM ownership, and lifecycle/threading assumptions.
- Historical references to GOARCH=386 go test -c remain in earlier checkpoints for traceability only. They are not required work and must not block progress or be described as target support.
- Remaining blockers are functional/COM correctness: dependency and parameter ownership audit, real TSF activation, key-sink behavior, edit-session text commit, and host/lifetime checks. Scope reduction to amd64 does not waive these gates.



## Latest execution checkpoint (2026-10-09, expanded SDK signature audit)

- Continued from the existing native vtable caller evidence; did not repeat binding scans or generator repeatability checks.
- Re-extracted compact, line-numbered declarations from Windows SDK 10.0.22000.0 `um/msctf.idl` and `um/msctf.h` for `TfClientId`, `ITfThreadMgr`, `ITfKeystrokeMgr`, `ITfKeyEventSink`, `ITfEditSession`, and `ITfTextInputProcessor`. Added the signature/dependency and pointer-ownership matrix to `ABI-AUDIT.md`.
- Confirmed the processor `Activate` parameter is an `[in] ITfThreadMgr*`, not a complete binding requirement for the current unused opaque-pointer fixture. Any future processor that retains the manager must `AddRef` it and release it during deactivation/finalization. The key-sink path requires `ITfKeystrokeMgr::AdviseKeyEventSink`/`UnadviseKeyEventSink`, the six-method `ITfKeyEventSink` vtable, pointer-sized `WPARAM`/`LPARAM` on amd64, and explicit sink unregister/lifetime handling. `ITfEditSession::DoEditSession` receives 32-bit `TfEditCookie` and is not yet implemented.
- Revalidation passed on the target Windows/amd64 machine: `go test -race -count=1 ./...` and `go vet ./...` from `strategy-generated-bindings`; `go test -race -count=1 ./com` from nested `third_party/go-com`.
- This closes a documentation-level signature/ownership pass for the listed interfaces, but not a generated dependency closure or runtime ABI proof for those not-yet-implemented interfaces. No new bindings, class factory, activation behavior, key sink, edit session, or text commit were added. `ITfThreadMgr` is still only an opaque pointer target in the fixture.
- **Status remains blocked/experimental.** Next action: use the pinned generator/model to produce a reproducible, dependency-closed minimal declaration set for the actual next server surface (processor plus the minimum required thread-manager/keystroke-manager/key-sink types), or document a no-go if this cannot be done narrowly. Only after that output is reviewed and callback/lifetime contracts are implemented should class-factory activation be considered. No production TIP registration, installer, or system configuration change was performed.

## Latest execution checkpoint (2026-10-09, generator model and extraction boundary)

- Resumed from the expanded SDK signature audit; did not repeat binding-source scans, generator repeatability runs, or native vtable tests.
- Inspected the pinned `zzl/go-winapi-gen` source at commit `4fd990193d56c9ea8fb18ece1adac5016c7dcbc4`: `gomodel.ApiFilter` filters namespaces only (`IncludeNs`), and `codegen.Generator.Gen` emits every model package/type. There is no existing interface-symbol allowlist or dependency-closure pass. Its `ModelParser` tracks type references for rendering/imports, but the current CLI does not expose a narrow symbol-rooted output mode.
- Located the already-generated `output/win32/UI.TextServices.go` in the temporary review checkout (353,956 bytes). It contains declaration blocks for `ITfThreadMgr`, `ITfKeystrokeMgr`, `ITfKeyEventSink`, `ITfContext`, `ITfEditSession`, and `ITfTextInputProcessor`; this confirms source metadata coverage, but not that a safe minimal subset can be cut out. The current `Extract-Minimal.ps1` is intentionally a fail-closed regex for the processor block and only supplies an opaque `ITfThreadMgr` placeholder. Extending that script by adding more independent regexes would not establish transitive type/constant closure and could silently omit ABI dependencies.
- Decision for this step: **do not expand the current regex extractor into the key-event surface yet**. The next implementation should add a symbol-rooted dependency-closure pass to an isolated generator checkout (or a deterministic AST/model-based extractor), produce a manifest of retained declarations and unresolved external references, and regenerate twice to verify byte identity. Keep generator edits and experiments outside this strategy until closure is demonstrated; do not copy the 354 KB namespace file into the strategy.
- No project source, module file, dependency, production registration, installer, or system configuration was changed in this checkpoint. No new tests were run because this inspection changed no code. Strategy D remains **blocked/experimental**; class-factory and TSF activation gates remain closed.

## Latest execution checkpoint (2026-10-09, AST closure extractor prototype)

- Added `cmd/closure-extract/main.go`, a Go `go/ast`-based tool that takes explicit top-level roots, follows identifier references to declarations within the same generated Go file, preserves imports, writes a deterministic Go subset, and emits a JSON manifest with source/output SHA-256, roots, retained symbols, and imports. Missing root symbols fail closed. This avoids brittle per-interface regex slicing, but is a prototype rather than a dependency-closed binding generator.
- Added focused tests covering transitive local type references, IID variable inclusion, exclusion of unrelated declarations, parseable output, byte-identical repeated output, and failure for missing roots.
- Applied it to the pinned generator checkout's `output/win32/UI.TextServices.go`, explicitly rooting the processor, thread manager, keystroke manager, key-event sink, edit session, context wrappers/interfaces/vtables and their IIDs. It retained 41 declarations in a 7,458-byte output. Two independent runs produced the same SHA-256 `E7660D95F22C75B73E3B1DCD30757472E9B5750D36E1E690302EBB3942BAD4C6`; the generated subset passes `gofmt` parsing.
- Important limitation found: generated wrapper structs, `*Interface` method interfaces, and `*Vtbl` structs are separate declarations, so the caller must explicitly root the actual server-facing surfaces; rooting only `ITfTextInputProcessor` silently omits its interface/vtable. Also, this first implementation only closes references against symbols declared in the single source file. It does **not yet enumerate unresolved cross-namespace references** (e.g. common `IUnknown`/HRESULT declarations) or prove the extracted set compiles as a standalone package. Do not replace `Extract-Minimal.ps1` or treat this output as production-ready bindings yet.
- Validation passed on Windows/amd64: `go test -count=1 ./cmd/closure-extract`, `go vet ./cmd/closure-extract`, strategy-wide `go test -race -count=1 ./...`, `go vet ./...`, and nested `third_party/go-com` `go test -race -count=1 ./com`.
- Next gate: extend the manifest to enumerate unresolved type references across the selected declarations, reconcile those names against all generated namespace files or the generator model, and fail if any required type remains unresolved. Then compile the resulting isolated fixture and add ABI/layout tests for every newly included interface. Keep class-factory and TSF activation blocked until that gate passes. No production registration, installer, or system configuration change was performed.

## Latest execution checkpoint (2026-10-09, unresolved-reference manifest)

- Extended `cmd/closure-extract` to emit `unresolved_type_refs` for names used in type positions that are not closed by the selected declarations, excluding Go predeclared types and import aliases. The collector deliberately avoids treating struct field/method names as dependencies.
- Regenerated a multi-interface subset from the already-generated `UI.TextServices.go`, rooting the processor, thread manager, keystroke manager, key-event sink, edit session, context wrappers/interfaces/vtables and IIDs. Output: 7,619 bytes; source SHA-256 `47fac627ae6aee653fdc17dcb0353ed412db8ddd39899f464fd4000cf25ebd60`; output SHA-256 `8f0d776b7894f6e84a586448466e40879c2ddb2bb785724e8c475040db4862f9`. An independent second run produced the same output hash.
- The manifest now explicitly reports unresolved references: `BOOL`, `BSTR`, `HRESULT`, `HWND`, `IUnknown`, `IUnknownInterface`, `IUnknownVtbl`, `LPARAM`, `PWSTR`, `WPARAM`, and `syscall.GUID`. A scan across the generated namespace files found `IUnknown`, `IUnknownInterface`, and `IUnknownVtbl` in `System.Com.go`, but did not find local declarations for the other named Win32 scalar/pointer aliases. `syscall.GUID` is supplied by the listed `syscall` import and is not a missing dependency. This confirms the selected single-file output is not yet standalone/dependency-closed; the COM base declarations and SDK-compatible Win32 aliases need a traceable resolution strategy.
- Validation on Windows/amd64: `go test -count=1 ./cmd/closure-extract`, strategy-wide `go test -race -count=1 ./...`, `go vet ./...`, and nested `third_party/go-com` `go test -race -count=1 ./com` passed.
- The extractor's unresolved-reference report is a type-position inventory, not yet a hard closure gate: references are not yet resolved against a full cross-file symbol index or generator model, and imports/qualified types need explicit classification. Do not treat the generated subset as compilable bindings or proceed to class-factory/TSF activation. Next: add a symbol index across all generated namespace files (and classify Win32 aliases that are intentionally provided by another package), fail closed for genuinely unresolved type names, then compile the isolated output and add layout tests for the added interfaces. Strategy D remains **blocked/experimental**. No production registration, installer, or system configuration change was performed.

## Latest execution checkpoint (2026-10-09, cross-file symbol index)

- Added `-index-dir` to `cmd/closure-extract`. It parses sibling generated `.go` files and builds a type-declaration index, excluding the primary input file. The manifest now distinguishes `resolved_external_type_refs` (declared in another generated file but not copied into this subset) from genuinely unresolved type names. The index intentionally indexes only `TypeSpec` declarations, avoiding false positives from constants/variables.
- Added `TestCrossFileIndexSeparatesExternalAndUnresolvedTypes`: a sibling `Shared` type is reported as an external dependency while an absent `Ghost` type remains unresolved. Focused extractor tests and `go vet ./cmd/closure-extract` passed.
- Applied the index to the 46-file generated `win32` namespace. The previously reported Win32 names are in `Foundation.go`: `BOOL`, `BSTR`, `HRESULT`, `HWND`, `LPARAM`, `PWSTR`, and `WPARAM`. `IUnknown`, `IUnknownInterface`, and `IUnknownVtbl` are present in other generated namespace files. With the index enabled, this sample reports no truly unresolved type names and reports these ten names as external references. Output remained byte-identical to the prior single-file subset (SHA-256 `8f0d776b7894f6e84a586448466e40879c2ddb2bb785724e8c475040db4862f9`), and a second indexed run was also byte-identical.
- Full validation passed on Windows/amd64: strategy `go test -race -count=1 ./...`, `go vet ./...`, and nested `third_party/go-com` `go test -race -count=1 ./com`.
- Important: this index is a resolver/report only; it does **not** yet copy external declarations into the extracted file, merge their imports, or recursively close dependencies across namespace files. Therefore, an empty `unresolved_type_refs` list means the names exist somewhere in the generated namespace, not that the extracted subset compiles standalone. Next: extend the extractor to carry source-file provenance for indexed symbols and recursively include required external declarations plus their imports, or build a generated fixture package from the original namespace files with a manifest-based allowlist. Compile that isolated fixture and add ABI/layout tests before considering activation. Strategy D remains **blocked/experimental**; no production registration, installer, or system configuration changes were performed.

## Handoff for next context (2026-10-09)

### Current working state

- Work in `D:\mcp-agent-workspace\typescript-windows-ime\native\golang\strategy-generated-bindings`.
- The AST extractor lives in `cmd/closure-extract/main.go`; regression tests are in `cmd/closure-extract/main_test.go`.
- Current CLI supports explicit roots, deterministic subset output, JSON manifest, unresolved type-reference reporting, and `-index-dir` for indexing sibling generated Go files.
- The cross-file index currently maps type names to existence only. It does **not** retain source-file provenance, import metadata, or declarations for copying. Do not mistake `resolved_external_type_refs` for a closed/compilable result.
- Reference sample: the pinned generator review checkout's `output/win32/UI.TextServices.go`; the indexed namespace has 46 generated Go files. The multi-interface subset hash is `8f0d776b7894f6e84a586448466e40879c2ddb2bb785724e8c475040db4862f9` and should remain reproducible unless the extraction algorithm intentionally changes.
- Existing validation last passed on Windows/amd64: `go test -race -count=1 ./...`, `go vet ./...` at this strategy root, plus `go test -race -count=1 ./com` from `third_party/go-com`. The nested fork's broader vet warnings are documented above; don't conflate them with root-package vet results.
- No commit was made. Keep all generator experiments isolated; do not change `native/golang/go.mod`.

### Next-session execution plan

1. **Inspect before editing.** Check `git status`, review the current extractor and tests, and confirm the temporary generator checkout/source still exists. Do not repeat earlier SDK scans, generator repeatability checks, or native-vtable probes unless code changes make them necessary.
2. **Choose the closure design explicitly.** Prefer a deterministic manifest/allowlist fixture assembled from the original generated namespace files if that avoids unsafe AST declaration rewriting. Otherwise upgrade the index to retain each type declaration's AST node, source path, imports, and dependency references, then recursively include dependencies. Keep same-file and cross-file resolution deterministic and fail closed on unresolved required types.
3. **Add focused tests first.** Cover cross-file transitive dependencies, cycles, duplicate/import-alias handling, stable output, missing types, and a fixture that genuinely compiles. Preserve source provenance in the manifest so every included declaration is auditable.
4. **Prove standalone compilation.** Build an isolated minimal fixture for the actual intended surface (processor plus only the required thread-manager/keystroke-manager/key-event-sink/edit-session declarations). Run `gofmt`, compile/test the fixture, and confirm two independent generations are byte-identical. A zero unresolved-name count alone is not acceptance.
5. **Audit ABI/layout and ownership.** Update `ABI-AUDIT.md` with exact declaration origins and check amd64 pointer widths, HRESULT/Win32 aliases, IIDs, vtable slot order, calling convention, parameter direction/ownership, COM AddRef/Release obligations, and lifecycle/reentrancy assumptions. Add layout/signature tests for every interface actually included.
6. **Revalidate the whole touched scope.** Run `go test -race -count=1 ./...` and `go vet ./...` at this directory; run `go test -race -count=1 ./com` separately in `third_party/go-com`. If the native vtable path or COM lifecycle code changes, rerun the native C vtable probe too.
7. **Reassess go/no-go before activation.** Only after dependency closure, standalone compilation, ABI/ownership review, and relevant native callback evidence pass may a minimal class-factory/activation prototype be considered. Real TSF-host tests for activation, key-sink registration, edit sessions, and text commit remain separate acceptance gates.

### Do not do in the next context

- Do not replace `Extract-Minimal.ps1` or widen regex extraction as a substitute for dependency closure.
- Do not copy the entire generated 354 KB namespace file into the strategy.
- Do not claim TSF compatibility based on parser success, an empty unresolved list, Go-to-Go calls, or amd64 vtable-probe results alone.
- Do not register/install the production TIP, run `native/install-user-golang.ps1`, or change system configuration.
- Strategy D remains **blocked/experimental** until the above gates are explicitly satisfied.

## Latest execution checkpoint (2026-10-09, cross-file provenance hardening)

- Resumed from the handoff and confirmed the temporary pinned generator checkout and 46-file generated `win32` namespace are still present. Did not repeat the earlier binding scans, SDK scan, or native vtable probe.
- Upgraded the cross-file type index from `name -> exists` to `name -> source path`. The manifest now emits `external_type_sources` entries for each resolved external type reference, preserving the exact generated file that declares it. Duplicate type names across indexed sibling files now fail closed instead of depending on directory traversal order.
- Added regression coverage for source provenance and duplicate-type rejection. Focused tests, strategy-wide `go test -race -count=1 ./...`, strategy `go vet ./...`, and nested `third_party/go-com` `go test -race -count=1 ./com` all passed on Windows/amd64.
- Re-ran extraction against the pinned TSF generated source with the cross-file index. The manifest now identifies `BOOL`, `HRESULT`, and `HWND` in `Foundation.go`, and `IUnknown`, `IUnknownInterface`, and `IUnknownVtbl` in `System.Com.go`; it still reports these as external references rather than copying them. The extracted output hash changed to `cb20ff5456fc8a154fc14e2eaafcfd5b4c53a6491e9ec4cdd99660b9153e28d6` for the explicitly supplied root set. This is provenance evidence only, not dependency closure or standalone compilation.
- Next: extend the indexed symbol records to retain declaration AST, source imports, and value declarations as needed; recursively include dependencies with cycle/duplicate handling and a deterministic provenance manifest. Add a genuine compile-fixture test before treating the result as closed. Status remains **blocked/experimental**; no production registration, installer, or system configuration change was performed.

## Latest execution checkpoint (2026-10-09, standalone closed subset)

- `cmd/closure-extract` now copies required cross-file type and value declarations, the imports those declarations use, and source-file provenance for every included symbol. Missing roots and unresolved required types fail. Duplicate indexed names fail only when that name is selected, so an unused collision such as `VK_F` does not block a rooted subset. Unkeyed `syscall.GUID` literals are rewritten to keyed fields; IID bytes are unchanged. Method bodies are not copied.
- Generated `closedtsf/declarations.go` (5,872 bytes) from the pinned checkout `output/win32`, rooting the processor, thread manager, keystroke manager, key-event sink, and edit session structs, interfaces, vtables, and IIDs. Two runs were byte-identical (SHA-256 `62195C7A1A17A7B3CBBACD3AEE616955BE9DEAB1919E19392FA85615D30D9EF3`). `gofmt -l` was clean. `go test ./closedtsf` compiles the package and checks amd64 widths, IID bytes, and vtable order. `ITfThreadMgr` includes its generated interface and vtable.
- `ABI-AUDIT.md` traces the included declarations to SDK 10.0.22000.0 `um/msctf.idl`, `um/msctf.h`, and the Win32 headers named there, including parameter direction and AddRef/Release ownership.
- **Status remains blocked/experimental.** No class-factory activation, TSF host test, production TIP registration, installer, or system configuration change was performed. `native/golang/go.mod` was not modified.
