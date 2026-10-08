#define INITGUID
#include "TsIme.h"
#include "PipeBridge.h"

#include <new>
#include <string>
#include <vector>

namespace {
HRESULT RunComRegistration(bool unregister);


volatile LONG g_serverLocks = 0;
volatile LONG g_objectCount = 0;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\TypeScriptWindowsIME";

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) out += '?';
            else out += static_cast<char>(c);
            break;
        }
    }
    return out;
}

bool JsonBool(const std::string& json, const char* key, bool fallback = false) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = json.find(needle);
    if (pos == std::string::npos) return fallback;
    const size_t value = pos + needle.size();
    if (json.compare(value, 4, "true") == 0) return true;
    if (json.compare(value, 5, "false") == 0) return false;
    return fallback;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string JsonString(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const size_t start = json.find(needle);
    if (start == std::string::npos) return {};
    size_t pos = start + needle.size();
    std::string out;
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') return out;
        if (c == '\\' && pos < json.size()) {
            const char escaped = json[pos++];
            switch (escaped) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '\\': out += '\\'; break;
            case '"': out += '"'; break;
            default: out += escaped; break;
            }
        } else {
            out += c;
        }
    }
    return {};
}

class CompositionSink final : public ITfCompositionSink {
public:
    CompositionSink() : refCount_(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_ITfCompositionSink) {
            *object = static_cast<ITfCompositionSink*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refCount_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie, ITfComposition*) override { return S_OK; }
private:
    LONG refCount_;
};

class CompositionEditSession final : public ITfEditSession {
public:
    CompositionEditSession(ITfContext* context, ITfComposition* composition, const std::string& text, bool commit)
        : refCount_(1), context_(context), composition_(composition), text_(text), commit_(commit), hr_(E_FAIL) {
        if (context_) context_->AddRef();
        if (composition_) composition_->AddRef();
    }
    ~CompositionEditSession() {
        if (composition_) composition_->Release();
        if (context_) context_->Release();
    }
    ITfComposition* DetachComposition() {
        ITfComposition* result = composition_;
        composition_ = nullptr;
        return result;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
            *object = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refCount_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie ec) override {
        if (!context_) return E_UNEXPECTED;

        ITfContextComposition* contextComposition = nullptr;
        HRESULT hr = context_->QueryInterface(IID_ITfContextComposition, reinterpret_cast<void**>(&contextComposition));
        if (FAILED(hr)) return hr;

        if (text_.empty()) {
            if (composition_) {
                hr = composition_->EndComposition(ec);
                composition_->Release();
                composition_ = nullptr;
            }
            contextComposition->Release();
            hr_ = hr;
            return hr;
        }

        if (!composition_) {
            TF_SELECTION selection{};
            ULONG fetched = 0;
            hr = context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
            if (FAILED(hr) || fetched != 1) {
                contextComposition->Release();
                hr_ = FAILED(hr) ? hr : E_FAIL;
                return hr_;
            }

            auto* sink = new (std::nothrow) CompositionSink();
            if (!sink) {
                contextComposition->Release();
                hr_ = E_OUTOFMEMORY;
                return hr_;
            }

            hr = contextComposition->StartComposition(ec, selection.range, sink, &composition_);
            sink->Release();
            selection.range->Release();
            if (FAILED(hr)) {
                contextComposition->Release();
                hr_ = hr;
                return hr_;
            }
        }

        ITfRange* range = nullptr;
        hr = composition_->GetRange(&range);
        if (SUCCEEDED(hr)) {
            const std::wstring wideText = Utf8ToWide(text_);
            hr = range->SetText(ec, 0, wideText.c_str(), static_cast<LONG>(wideText.size()));
            if (SUCCEEDED(hr) && commit_) {
                hr = composition_->EndComposition(ec);
            }
            range->Release();
        }

        if (commit_ && composition_) {
            composition_->Release();
            composition_ = nullptr;
        }
        contextComposition->Release();
        hr_ = hr;
        return hr;
    }

    HRESULT Result() const { return hr_; }
private:
    LONG refCount_;
    ITfContext* context_;
    std::string text_;
    bool commit_;
    HRESULT hr_;
    ITfComposition* composition_ = nullptr;
};

class TextService;

class KeyEventSink final : public ITfKeyEventSink {
public:
    explicit KeyEventSink(TextService* service) : refCount_(1), service_(service) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refCount_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext*, WPARAM, LPARAM, BOOL*) override;
    HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL*) override;
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext*, WPARAM, LPARAM, BOOL*) override;
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL*) override;
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext*, REFGUID, BOOL*) override;
private:
    LONG refCount_;
    TextService* service_;
};

class TextService final : public ITfTextInputProcessor {
public:
    TextService()
        : refCount_(1), threadMgr_(nullptr), keyMgr_(nullptr), keySink_(nullptr),
          clientId_(TF_CLIENTID_NULL), nextRequestId_(1), pipe_(250), compositionContext_(nullptr),
          compositionText_() {
        InterlockedIncrement(&g_objectCount);
    }

    ~TextService() {
        Deactivate();
        InterlockedDecrement(&g_objectCount);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor) {
            *object = static_cast<ITfTextInputProcessor*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refCount_)); }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (!count) delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* threadMgr, TfClientId clientId) override {
        if (!threadMgr) return E_INVALIDARG;
        Deactivate();

        threadMgr_ = threadMgr;
        threadMgr_->AddRef();
        clientId_ = clientId;

        HRESULT hr = threadMgr_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&keyMgr_));
        if (FAILED(hr)) {
            Deactivate();
            return hr;
        }

        keySink_ = new (std::nothrow) KeyEventSink(this);
        if (!keySink_) {
            Deactivate();
            return E_OUTOFMEMORY;
        }

        hr = keyMgr_->AdviseKeyEventSink(clientId_, keySink_, TRUE);
        if (FAILED(hr)) {
            hr = keyMgr_->AdviseKeyEventSink(clientId_, keySink_, FALSE);
        }
        if (FAILED(hr)) {
            keySink_->Release();
            keySink_ = nullptr;
            Deactivate();
            return hr;
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Deactivate() override {
        if (keyMgr_ && keySink_ && clientId_ != TF_CLIENTID_NULL) {
            keyMgr_->UnadviseKeyEventSink(clientId_);
        }
        if (keySink_) {
            keySink_->Release();
            keySink_ = nullptr;
        }
        if (keyMgr_) {
            keyMgr_->Release();
            keyMgr_ = nullptr;
        }
        if (threadMgr_) {
            threadMgr_->Release();
            threadMgr_ = nullptr;
        }
        clientId_ = TF_CLIENTID_NULL;
        pipe_.Disconnect();
        compositionContext_ = nullptr;
        if (composition_) {
            composition_->Release();
            composition_ = nullptr;
        }
        compositionText_.clear();
        return S_OK;
    }

    bool TestKey(ITfContext*, WPARAM wParam, LPARAM, bool& consume);
    bool HandleKey(ITfContext* context, WPARAM wParam, LPARAM, bool& consume);

private:
    static std::string KeyName(WPARAM vk);
    static int Modifiers();

    bool CallCore(const std::string& type, WPARAM vk, LPARAM lParam, bool& consume,
                  std::string& response);
    bool ApplyResponse(ITfContext* context, const std::string& response);

    LONG refCount_;
    ITfThreadMgr* threadMgr_;
    ITfKeystrokeMgr* keyMgr_;
    KeyEventSink* keySink_;
    TfClientId clientId_;
    unsigned int nextRequestId_;
    PipeBridge pipe_;
    ITfContext* compositionContext_;
    std::string compositionText_;
    ITfComposition* composition_ = nullptr;
};

HRESULT KeyEventSink::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfKeyEventSink) {
        *object = static_cast<ITfKeyEventSink*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

std::string TextService::KeyName(WPARAM vk) {
    switch (vk) {
    case VK_BACK: return "Backspace";
    case VK_ESCAPE: return "Escape";
    case VK_RETURN: return "Enter";
    case VK_SPACE: return " ";
    default:
        if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
        if (vk >= 'a' && vk <= 'z') return std::string(1, static_cast<char>(vk));
        return {};
    }
}

int TextService::Modifiers() {
    int modifiers = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= 1;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= 2;
    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= 4;
    return modifiers;
}

bool TextService::CallCore(const std::string& type, WPARAM vk, LPARAM lParam, bool& consume,
                           std::string& response) {
    consume = false;
    const std::string key = KeyName(vk);
    if (key.empty()) return true;

    if (!pipe_.IsConnected() && !pipe_.Connect(kPipeName)) {
        return false;
    }

    const unsigned int id = nextRequestId_++;
    const unsigned int scanCode = (static_cast<unsigned int>(lParam) >> 16) & 0xff;
    const std::string request =
        std::string("{\"id\":") + std::to_string(id) +
        ",\"type\":\"" + type +
        "\",\"vk\":" + std::to_string(static_cast<unsigned int>(vk)) +
        ",\"scanCode\":" + std::to_string(scanCode) +
        ",\"key\":\"" + JsonEscape(key) +
        "\",\"modifiers\":" + std::to_string(Modifiers()) + "}";

    if (!pipe_.Call(request, response, id)) {
        pipe_.Disconnect();
        return false;
    }
    consume = JsonBool(response, "consume", false);
    return true;
}

bool TextService::TestKey(ITfContext* context, WPARAM wParam, LPARAM lParam, bool& consume) {
    (void)context;
    std::string response;
    return CallCore("testKeyDown", wParam, lParam, consume, response);
}

bool TextService::HandleKey(ITfContext* context, WPARAM wParam, LPARAM lParam, bool& consume) {
    std::string response;
    if (!CallCore("keyDown", wParam, lParam, consume, response)) return false;
    if (consume && context) return ApplyResponse(context, response);
    return true;
}

bool TextService::ApplyResponse(ITfContext* context, const std::string& response) {
    const std::string composition = JsonString(response, "composition");
    const std::string commit = JsonString(response, "commit");
    const bool hasCommit = response.find("\"commit\":") != std::string::npos;
    const std::string text = hasCommit ? commit : composition;

    if (!context) return false;
    CompositionEditSession session(context, composition_, text, hasCommit);
    HRESULT sessionResult = E_FAIL;
    const HRESULT requestResult = context->RequestEditSession(
        clientId_, &session, TF_ES_SYNC | TF_ES_READWRITE, &sessionResult);
    if (FAILED(requestResult) || FAILED(sessionResult)) return false;

    if (hasCommit || composition.empty()) {
        compositionContext_ = nullptr;
        compositionText_.clear();
        if (composition_) {
            composition_->Release();
            composition_ = nullptr;
        }
    } else {
        if (!composition_) composition_ = session.DetachComposition();
        compositionContext_ = context;
        compositionText_ = composition;
    }
    return true;
}

HRESULT KeyEventSink::OnTestKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
    if (!eaten) return E_POINTER;
    bool consume = false;
    const bool ok = service_->TestKey(context, wParam, lParam, consume);
    *eaten = ok && consume ? TRUE : FALSE;
    return S_OK;
}

HRESULT KeyEventSink::OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
    if (!eaten) return E_POINTER;
    *eaten = FALSE;
    return S_OK;
}

HRESULT KeyEventSink::OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
    if (!eaten) return E_POINTER;
    bool consume = false;
    const bool ok = service_->HandleKey(context, wParam, lParam, consume);
    *eaten = ok && consume ? TRUE : FALSE;
    return S_OK;
}

HRESULT KeyEventSink::OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
    if (!eaten) return E_POINTER;
    *eaten = FALSE;
    return S_OK;
}

HRESULT KeyEventSink::OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) {
    if (!eaten) return E_POINTER;
    *eaten = FALSE;
    return S_OK;
}

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() : refCount_(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *object = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refCount_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* service = new (std::nothrow) TextService();
        if (!service) return E_OUTOFMEMORY;
        const HRESULT hr = service->QueryInterface(riid, object);
        service->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_serverLocks);
        else InterlockedDecrement(&g_serverLocks);
        return S_OK;
    }
private:
    LONG refCount_;
};

HRESULT CreateProfiles(ITfInputProcessorProfiles** profiles) {
    if (!profiles) return E_POINTER;
    *profiles = nullptr;
    return CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfiles, reinterpret_cast<void**>(profiles));
}

HRESULT RegisterTipProfile() {
    ITfInputProcessorProfiles* profiles = nullptr;
    HRESULT hr = CreateProfiles(&profiles);
    if (FAILED(hr)) return hr;
    hr = profiles->Register(CLSID_TypeScriptWindowsIme);
    if (SUCCEEDED(hr)) {
        const LANGID languages[] = { 0x0409, 0x0804, 0x0411 };
        for (const LANGID langid : languages) {
            hr = profiles->AddLanguageProfile(CLSID_TypeScriptWindowsIme, langid,
                GUID_TypeScriptWindowsImeProfile, kTypeScriptWindowsImeName,
                static_cast<ULONG>(-1), nullptr, 0, 0);
            if (FAILED(hr)) break;
        }
    }
    if (SUCCEEDED(hr)) {
        ITfCategoryMgr* categoryMgr = nullptr;
        hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfCategoryMgr, reinterpret_cast<void**>(&categoryMgr));
        if (SUCCEEDED(hr)) {
            hr = categoryMgr->RegisterCategory(CLSID_TypeScriptWindowsIme,
                GUID_TFCAT_TIP_KEYBOARD, CLSID_TypeScriptWindowsIme);
            categoryMgr->Release();
        }
    }
    profiles->Release();
    return hr;
}

HRESULT UnregisterTipProfile() {
    ITfInputProcessorProfiles* profiles = nullptr;
    HRESULT hr = CreateProfiles(&profiles);
    if (FAILED(hr)) return hr;
    hr = profiles->Unregister(CLSID_TypeScriptWindowsIme);
    profiles->Release();
    return hr;
}

HRESULT SetRegString(HKEY root, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* value) {
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(root, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);
    const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
    result = RegSetValueExW(key, valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value), bytes);
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(result);
}

HRESULT RegisterComServer() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&RunComRegistration), &module)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, modulePath, ARRAYSIZE(modulePath));
    if (length == 0 || length == ARRAYSIZE(modulePath))
        return HRESULT_FROM_WIN32(GetLastError());

    const wchar_t* clsid = L"CLSID\\{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}";
    HRESULT hr = SetRegString(HKEY_CLASSES_ROOT, clsid, nullptr, kTypeScriptWindowsImeName);
    if (FAILED(hr)) return hr;
    hr = SetRegString(HKEY_CLASSES_ROOT,
        L"CLSID\\{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}\\InprocServer32",
        nullptr, modulePath);
    if (FAILED(hr)) return hr;
    return SetRegString(HKEY_CLASSES_ROOT,
        L"CLSID\\{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}\\InprocServer32",
        L"ThreadingModel", L"Apartment");
}

HRESULT UnregisterComServer() {
    const LSTATUS result = RegDeleteTreeW(HKEY_CLASSES_ROOT,
        L"CLSID\\{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}");
    if (result == ERROR_FILE_NOT_FOUND) return S_OK;
    return HRESULT_FROM_WIN32(result);
}

HRESULT RunComRegistration(bool unregister) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return init;

    HRESULT hr = unregister ? UnregisterTipProfile() : RegisterComServer();
    if (SUCCEEDED(hr)) hr = unregister ? UnregisterComServer() : RegisterTipProfile();
    if (!unregister && FAILED(hr)) UnregisterComServer();
    if (shouldUninitialize) CoUninitialize();
    return hr;
}
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* object) {
    if (clsid != CLSID_TypeScriptWindowsIme) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) ClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, object);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_objectCount == 0 && g_serverLocks == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() { return RunComRegistration(false); }
STDAPI DllUnregisterServer() { return RunComRegistration(true); }
