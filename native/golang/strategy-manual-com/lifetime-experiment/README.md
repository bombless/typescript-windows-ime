# Go DLL lifetime experiment (Windows only)

This isolated harness builds a small Go `c-shared` DLL and loads it from a native C host using `LoadLibrary`. It exports `DllCanUnloadNow` returning `S_FALSE` and one fixed `syscall.NewCallback` thunk. The host calls the thunk 10,000 times, requests Go GC through an exported function, then calls the thunk again.

Build and run from this directory in PowerShell:

```powershell
go build -buildmode=c-shared -o LifetimeExperiment.dll .
clang .\host.c -o .\LifetimeHost.exe
.\LifetimeHost.exe (Join-Path (Get-Location) 'LifetimeExperiment.dll')
```

The host intentionally never calls `FreeLibrary`. The experiment only demonstrates that the exported Go functions and callback thunk work while the module remains loaded and that the chosen unload policy reports `S_FALSE`; it does not prove that COM itself loaded the module, that a COM host obeys the unload result, that object reference counting is correct, or that DLL unload/reload is safe. It is not a COM server (`DllGetClassObject` is not implemented), is not registered, and must not be used as an input method. Runtime testing is Windows amd64 only.