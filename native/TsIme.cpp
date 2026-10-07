#define INITGUID
#include "TsIme.h"

#include <new>

namespace {
volatile LONG g_serverLocks = 0;
volatile LONG g_objectCount = 0;

class TextService final : public ITfTextInputProcessor {
public:
    TextService() : refCount_(1), threadMgr_(nullptr), clientId_(TF_CLIENTID_NULL) { InterlockedIncrement(&g_objectCount); }
    ~TextService() { if (threadMgr_) threadMgr_->Release(); InterlockedDecrement(&g_objectCount); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER; *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor) { *object = static_cast<ITfTextInputProcessor*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refCount_)); }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_)); if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* threadMgr, TfClientId clientId) override {
        if (!threadMgr) return E_INVALIDARG;
        if (threadMgr_) threadMgr_->Release(); threadMgr_ = threadMgr; threadMgr_->AddRef(); clientId_ = clientId; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Deactivate() override {
        if (threadMgr_) { threadMgr_->Release(); threadMgr_ = nullptr; }
        clientId_ = TF_CLIENTID_NULL; return S_OK;
    }
private:
    LONG refCount_;
    ITfThreadMgr* threadMgr_;
    TfClientId clientId_;
};

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() : refCount_(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER; *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *object = static_cast<IClassFactory*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refCount_)); }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_)); if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* service = new (std::nothrow) TextService(); if (!service) return E_OUTOFMEMORY;
        const HRESULT hr = service->QueryInterface(riid, object); service->Release(); return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { if (lock) InterlockedIncrement(&g_serverLocks); else InterlockedDecrement(&g_serverLocks); return S_OK; }
private:
    LONG refCount_;
};

HRESULT CreateProfiles(ITfInputProcessorProfiles** profiles) {
    if (!profiles) return E_POINTER; *profiles = nullptr;
    return CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles, reinterpret_cast<void**>(profiles));
}

HRESULT RegisterTipProfile() {
    ITfInputProcessorProfiles* profiles = nullptr; HRESULT hr = CreateProfiles(&profiles); if (FAILED(hr)) return hr;
    hr = profiles->Register(CLSID_TypeScriptWindowsIme);
    if (SUCCEEDED(hr)) {
        hr = profiles->AddLanguageProfile(CLSID_TypeScriptWindowsIme, static_cast<LANGID>(0xFFFF), GUID_TypeScriptWindowsImeProfile,
            kTypeScriptWindowsImeName, static_cast<ULONG>(-1), nullptr, 0, 0);
    }
    if (SUCCEEDED(hr)) {
        ITfCategoryMgr* categoryMgr = nullptr;
        hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr, reinterpret_cast<void**>(&categoryMgr));
        if (SUCCEEDED(hr)) {
            hr = categoryMgr->RegisterCategory(CLSID_TypeScriptWindowsIme, GUID_TFCAT_TIP_KEYBOARD, CLSID_TypeScriptWindowsIme);
            categoryMgr->Release();
        }
    }
    profiles->Release(); return hr;
}

HRESULT UnregisterTipProfile() {
    ITfInputProcessorProfiles* profiles = nullptr; HRESULT hr = CreateProfiles(&profiles); if (FAILED(hr)) return hr;
    hr = profiles->Unregister(CLSID_TypeScriptWindowsIme); profiles->Release(); return hr;
}

HRESULT SetRegString(HKEY root, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* value) {
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(root, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);
    const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
    result = RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), bytes);
    RegCloseKey(key); return HRESULT_FROM_WIN32(result);
}

HRESULT RegisterComServer() {
    wchar_t modulePath[MAX_PATH]{}; const DWORD length = GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));
    if (length == 0 || length == ARRAYSIZE(modulePath)) return HRESULT_FROM_WIN32(GetLastError());
    const wchar_t* clsid = L"CLSID\\{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}";
    HRESULT hr = SetRegString(HKEY_CLASSES_ROOT, clsid, nullptr, kTypeScriptWindowsImeName); if (FAILED(hr)) return hr;
    hr = SetRegString(HKEY_CLASSES_ROOT, L"CLSID\\{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}\\InprocServer32", nullptr, modulePath); if (FAILED(hr)) return hr;
    return SetRegString(HKEY_CLASSES_ROOT, L"CLSID\\{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}\\InprocServer32", L"ThreadingModel", L"Apartment");
}

HRESULT UnregisterComServer() {
    const LSTATUS result = RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}");
    if (result == ERROR_FILE_NOT_FOUND) return S_OK; return HRESULT_FROM_WIN32(result);
}

HRESULT RunComRegistration(bool unregister) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); const bool shouldUninitialize = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return init;
    HRESULT hr = unregister ? UnregisterTipProfile() : RegisterComServer();
    if (SUCCEEDED(hr)) hr = unregister ? UnregisterComServer() : RegisterTipProfile();
    if (!unregister && FAILED(hr)) UnregisterComServer();
    if (shouldUninitialize) CoUninitialize(); return hr;
}
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* object) {
    if (clsid != CLSID_TypeScriptWindowsIme) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) ClassFactory(); if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, object); factory->Release(); return hr;
}
STDAPI DllCanUnloadNow() { return (g_objectCount == 0 && g_serverLocks == 0) ? S_OK : S_FALSE; }
STDAPI DllRegisterServer() { return RunComRegistration(false); }
STDAPI DllUnregisterServer() { return RunComRegistration(true); }
