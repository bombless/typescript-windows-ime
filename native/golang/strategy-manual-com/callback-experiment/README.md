# Windows Go callback and COM-shaped vtable experiment

This isolated Windows executable checks a scalar `syscall.NewCallback` thunk, a C-allocated `IUnknown`-shaped vtable, simultaneous callback entry from native Windows threads, and bounded same-thread callback reentrancy.

Run from this directory:

```powershell
go run .
```

Expected Windows amd64 output currently includes:

```text
PASS: scalar callback=90001 calls including 8x10000 concurrent calls; COM-shaped IUnknown vtable checks passed; vtable-callbacks=7; refs=2; pointer-size=8
```

The harness verifies supported `QueryInterface(IID_IUnknown)`, unsupported IID returning `E_NOINTERFACE` with output cleared, null output returning `E_POINTER`, `AddRef`/`Release` return values, callback dispatch after forced Go GC, 80,000 concurrent scalar callback calls across eight native Windows threads, and 64 levels of same-thread callback reentrancy. Go-side counters use atomics; the native helper waits for all worker threads before returning. The object and vtable memory are allocated by C; no Go pointer is retained in native memory.

Interpretation and limits:

- These are observed results on Windows amd64 with Go 1.27.0 and cgo enabled, not a formal guarantee for all callback usage patterns.
- `go test -race` does not instrument arbitrary native memory accesses or prove that foreign COM hosts obey safe lifetime rules.
- The harness is not a COM server. C owns and manually frees object memory; zero COM reference count is not connected to destruction.
- It does not test a foreign COM host, DLL unloading while callbacks are in flight, callback thunk reclamation (the Go docs say callback memory is never released), apartment/marshaling rules, or TSF activation and typing.
- Runtime validation was only on Windows amd64; x86/arm64 remain compile checks only. Do not register this prototype as an input method.
