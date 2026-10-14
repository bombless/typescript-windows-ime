#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msctf.h>
#include <cstdio>

static const CLSID CLSID_GoTip = {0xa4c7d921,0x6b35,0x4e8a,{0x91,0xf2,0x7d,0x0b,0x6c,0x3e,0x5a,0x14}};

using GetClassObjectFn = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);

static const CLSID CLSID_Wrong = {0x11111111,0x2222,0x3333,{0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb}};

static bool expectHr(const char* label, HRESULT actual, HRESULT expected) {
    if (actual != expected) {
        std::printf("FAIL: %s returned 0x%08lx, expected 0x%08lx\n", label, actual, expected);
        return false;
    }
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) { std::printf("LoadLibrary failed: %lu\n", GetLastError()); return 3; }
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    if (!getClassObject) { std::puts("DllGetClassObject export missing"); FreeLibrary(dll); return 4; }

    bool ok = true;
    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(CLSID_Wrong, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    ok &= expectHr("wrong CLSID", hr, CLASS_E_CLASSNOTAVAILABLE);
    ok &= (factory == nullptr);

    hr = getClassObject(CLSID_GoTip, IID_IClassFactory, nullptr);
    ok &= expectHr("DllGetClassObject null output", hr, E_POINTER);

    IUnknown* unsupported = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(1));
    const IID IID_Unsupported = {0xaaaaaaaa,0xbbbb,0xcccc,{0xdd,0xee,0xff,0x11,0x22,0x33,0x44,0x55}};
    hr = getClassObject(CLSID_GoTip, IID_Unsupported, reinterpret_cast<void**>(&unsupported));
    ok &= expectHr("unsupported factory IID", hr, E_NOINTERFACE);
    ok &= (unsupported == nullptr);

    hr = getClassObject(CLSID_GoTip, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) { std::printf("DllGetClassObject failed: 0x%08lx\n", hr); FreeLibrary(dll); return 5; }

    hr = factory->CreateInstance(nullptr, IID_ITfTextInputProcessor, nullptr);
    ok &= expectHr("CreateInstance null output", hr, E_POINTER);

    ITfTextInputProcessor* tip = reinterpret_cast<ITfTextInputProcessor*>(static_cast<uintptr_t>(1));
    hr = factory->CreateInstance(reinterpret_cast<IUnknown*>(factory), IID_ITfTextInputProcessor, reinterpret_cast<void**>(&tip));
    ok &= expectHr("aggregation rejection", hr, CLASS_E_NOAGGREGATION);
    ok &= (tip == nullptr);

    tip = reinterpret_cast<ITfTextInputProcessor*>(static_cast<uintptr_t>(1));
    hr = factory->CreateInstance(nullptr, IID_Unsupported, reinterpret_cast<void**>(&tip));
    ok &= expectHr("unsupported object IID", hr, E_NOINTERFACE);
    ok &= (tip == nullptr);

    IUnknown* factoryIdentity = nullptr;
    hr = factory->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&factoryIdentity));
    ok &= expectHr("factory IUnknown identity", hr, S_OK);
    ok &= (factoryIdentity != nullptr);
    if (factoryIdentity) factoryIdentity->Release();

    factory->Release();
    if (!ok) { std::puts("FAIL: one or more negative-path checks failed"); FreeLibrary(dll); return 9; }

    for (int i = 0; i < 100; ++i) {
        factory = nullptr;
        hr = getClassObject(CLSID_GoTip, IID_IClassFactory, reinterpret_cast<void**>(&factory));
        if (FAILED(hr) || !factory) { std::printf("iteration %d: factory creation failed: 0x%08lx\n", i, hr); FreeLibrary(dll); return 10; }
        tip = nullptr;
        hr = factory->CreateInstance(nullptr, IID_ITfTextInputProcessor, reinterpret_cast<void**>(&tip));
        factory->Release();
        if (FAILED(hr) || !tip) { std::printf("iteration %d: CreateInstance failed: 0x%08lx\n", i, hr); FreeLibrary(dll); return 11; }
        IUnknown* objectIdentity = nullptr;
        hr = tip->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&objectIdentity));
        if (FAILED(hr) || !objectIdentity) { std::printf("iteration %d: object IUnknown QI failed: 0x%08lx\n", i, hr); tip->Release(); FreeLibrary(dll); return 14; }
        ITfTextInputProcessor* tipAgain = nullptr;
        hr = objectIdentity->QueryInterface(IID_ITfTextInputProcessor, reinterpret_cast<void**>(&tipAgain));
        if (FAILED(hr) || !tipAgain) { std::printf("iteration %d: round-trip QI failed: 0x%08lx\n", i, hr); objectIdentity->Release(); tip->Release(); FreeLibrary(dll); return 15; }
        tipAgain->Release();
        objectIdentity->Release();
        for (int cycle = 0; cycle < 5; ++cycle) {
            hr = tip->Activate(nullptr, 1);
            if (FAILED(hr)) { std::printf("iteration %d cycle %d: Activate failed: 0x%08lx\n", i, cycle, hr); tip->Release(); FreeLibrary(dll); return 12; }
            hr = tip->Deactivate();
            if (FAILED(hr)) { std::printf("iteration %d cycle %d: Deactivate failed: 0x%08lx\n", i, cycle, hr); tip->Release(); FreeLibrary(dll); return 13; }
        }
        tip->Release();
    }

    std::puts("PASS: negative HRESULT/output-pointer cases; 100 object lifetimes x 5 Activate/Deactivate cycles");
    FreeLibrary(dll);
    return 0;
}
