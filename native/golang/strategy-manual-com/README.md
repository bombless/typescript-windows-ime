# Strategy B — manual COM ABI experiment

This is the first ABI-conformance slice of `PLAN.md`, not a registered TSF DLL.
It records the minimal GUID/HRESULT/vtable surface and tests the GUID byte order,
interface slot counts, and reference-count transitions before any callback is
allowed into a host process.

## Sources and scope

- Windows SDK `10.0.22000.0`, `um/unknwn.h`: `IUnknown` and `IClassFactory` declarations.
- Windows SDK `10.0.22000.0`, `um/msctf.h`: `ITfTextInputProcessor` is declared at
  the `ITfTextInputProcessor : public IUnknown` declaration with IID
  `AA80E7F7-2021-11D2-93E0-0060B067B86E`; its methods are `Activate` then `Deactivate`.
- Existing sibling `adapter/TsfGoAdapter.cpp`: project CLSID `5D8C1D62-9F73-4B4C-A3D2-1E7B8A9C6401`.

`IClassFactory` vtable: `IUnknown`'s 3 slots, then `CreateInstance`, `LockServer`.
`ITfTextInputProcessor` vtable: `IUnknown`'s 3 slots, then `Activate`, `Deactivate`.

## Known blockers before a COM server can be claimed

- This pure-Go package does not yet expose `DllGetClassObject` / `DllCanUnloadNow`.
- Callback ABI, pointer ownership, COM apartment behavior, activation/deactivation,
  and lifetime rooting have not been proven in a real TSF host.
- A narrow callback spike now passes on Windows amd64: native C called a `syscall.NewCallback` Go thunk 10,001 times, including after forced GC. This proves only that callback mechanism for scalar arguments/results; it does not establish COM vtable correctness, long-lived object ownership, reentrancy/thread safety, unload behavior, or TSF activation. See `callback-experiment/README.md` and the latest checkpoint in `PLAN.md`.
- Do not infer that `//export` alone or a successful callback spike makes a safe COM server. The next step is a COM-shaped `IUnknown` vtable harness using native-allocated object/vtable memory, followed by pointer-ownership and lifetime checks before attempting class-factory activation.
- Only Windows amd64 is currently tested; x86/arm64 are not declared supported.
- Do not register this experiment as an input method until the exported class
  factory and in-process activation smoke test exist.

Run the current checks with `go test ./...` from this directory on Windows.