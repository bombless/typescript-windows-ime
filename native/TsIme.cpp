#define INITGUID
#include "TsIme.h"
#include "PipeBridge.h"
#include "NativeLog.h"

#include <new>
#include <string>
#include <vector>

namespace {
HRESULT RunComRegistration(bool unregister);


volatile LONG g_serverLocks = 0;
volatile LONG g_objectCount = 0;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\TypeScriptWindowsIME.Tsf";

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

unsigned int JsonUInt(const std::string& json, const char* key, unsigned int fallback = 0) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = json.find(needle);
    if (pos == std::string::npos) return fallback;
    const size_t value = pos + needle.size();
    size_t end = value;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
    if (end == value) return fallback;
    try { return static_cast<unsigned int>(std::stoul(json.substr(value, end - value))); }
    catch (...) { return fallback; }
}

std::vector<std::string> JsonCandidateTexts(const std::string& json) {
    std::vector<std::string> candidates;
    const std::string marker = "\"candidates\":[";
    const size_t start = json.find(marker);
    if (start == std::string::npos) return candidates;
    size_t pos = start + marker.size();
    while (pos < json.size()) {
        const size_t object = json.find("{", pos);
        const size_t arrayEnd = json.find("]", pos);
        if (arrayEnd != std::string::npos && (object == std::string::npos || object > arrayEnd)) break;
        if (object == std::string::npos) break;
        const size_t text = json.find("\"text\":\"", object);
        if (text == std::string::npos || (arrayEnd != std::string::npos && text > arrayEnd)) break;
        size_t cursor = text + 8;
        std::string value;
        while (cursor < json.size()) {
            const char c = json[cursor++];
            if (c == '"') break;
            if (c == '\\' && cursor < json.size()) {
                const char escaped = json[cursor++];
                switch (escaped) {
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case '\\': value += '\\'; break;
                case '"': value += '"'; break;
                default: value += escaped; break;
                }
            } else value += c;
        }
        candidates.push_back(value);
        pos = object + 1;
    }
    return candidates;
}

constexpr GUID kCandidateListUiElementGuid =
    { 0x9f5b4d21, 0x4b54, 0x4f4f, { 0x9a, 0x2a, 0x7f, 0x32, 0x9e, 0x91, 0x4b, 0x16 } };

class CandidateListUIElement final : public ITfCandidateListUIElementBehavior {
public:
    CandidateListUIElement(ITfDocumentMgr* documentMgr, std::vector<std::string> candidates, UINT selection)
        : refCount_(1), documentMgr_(documentMgr), candidates_(std::move(candidates)), selection_(selection), shown_(TRUE) {
        if (documentMgr_) documentMgr_->AddRef();
    }
    ~CandidateListUIElement() { if (documentMgr_) documentMgr_->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_ITfUIElement || riid == IID_ITfCandidateListUIElement ||
            riid == IID_ITfCandidateListUIElementBehavior) {
            *object = static_cast<ITfCandidateListUIElementBehavior*>(this);
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
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* description) override {
        if (!description) return E_POINTER;
        *description = SysAllocString(L"TypeScript Windows IME candidates");
        return *description ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* guid) override {
        if (!guid) return E_POINTER;
        *guid = kCandidateListUiElementGuid;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override { shown_ = show; return S_OK; }
    HRESULT STDMETHODCALLTYPE IsShown(BOOL* show) override {
        if (!show) return E_POINTER;
        *show = shown_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD* flags) override {
        if (!flags) return E_POINTER;
        *flags = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION |
            TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr** documentMgr) override {
        if (!documentMgr) return E_POINTER;
        *documentMgr = documentMgr_;
        if (*documentMgr) (*documentMgr)->AddRef();
        return *documentMgr ? S_OK : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE GetCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = static_cast<UINT>(candidates_.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSelection(UINT* index) override {
        if (!index) return E_POINTER;
        *index = selection_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetString(UINT index, BSTR* string) override {
        if (!string) return E_POINTER;
        *string = nullptr;
        if (index >= candidates_.size()) return E_INVALIDARG;
        const std::wstring wide = Utf8ToWide(candidates_[index]);
        *string = SysAllocStringLen(wide.data(), static_cast<UINT>(wide.size()));
        return *string ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetPageIndex(UINT* index, UINT size, UINT* pageCount) override {
        if (!pageCount) return E_POINTER;
        *pageCount = 1;
        if (size == 0) return S_OK;
        if (!index) return E_POINTER;
        const UINT count = static_cast<UINT>(candidates_.size());
        const UINT limit = (size < count) ? size : count;
        for (UINT i = 0; i < limit; ++i) index[i] = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPageIndex(UINT*, UINT pageCount) override {
        return pageCount == 1 ? S_OK : E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT* page) override {
        if (!page) return E_POINTER;
        *page = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSelection(UINT index) override {
        if (index >= candidates_.size()) return E_INVALIDARG;
        selection_ = index;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Finalize() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Abort() override { return S_OK; }

    void Update(std::vector<std::string> candidates, UINT selection) {
        candidates_ = std::move(candidates);
        selection_ = candidates_.empty() ? 0 : (selection < candidates_.size() ? selection : 0);
        shown_ = !candidates_.empty();
    }

private:
    LONG refCount_;
    ITfDocumentMgr* documentMgr_;
    std::vector<std::string> candidates_;
    UINT selection_;
    BOOL shown_;
};

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
    HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override;
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
          clientId_(TF_CLIENTID_NULL), keySinkAdvised_(false), nextRequestId_(1), pipe_(250), compositionContext_(nullptr),
          compositionText_(), uiElementMgr_(nullptr), candidateElement_(nullptr), candidateUiElementId_(0) {
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
        NativeLog::Write("TextService::Activate enter this=%p threadMgr=%p clientId=%lu", this, threadMgr,
            static_cast<unsigned long>(clientId));
        if (!threadMgr) {
            NativeLog::Hr("TextService::Activate invalid threadMgr", E_INVALIDARG);
            return E_INVALIDARG;
        }
        Deactivate();

        threadMgr_ = threadMgr;
        threadMgr_->AddRef();
        clientId_ = clientId;

        HRESULT hr = threadMgr_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&keyMgr_));
        NativeLog::Hr("TextService::Activate QueryInterface(ITfKeystrokeMgr)", hr);
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
        if (SUCCEEDED(hr)) keySinkAdvised_ = true;
        NativeLog::Write("TextService::Activate AdviseKeyEventSink clientId=%lu hr=0x%08lX advised=%d",
            static_cast<unsigned long>(clientId_), static_cast<unsigned long>(hr), keySinkAdvised_ ? 1 : 0);

        // The pipe server belongs to the activated TSF instance, not to an
        // individual key event. Start accepting Node clients immediately so
        // Node can connect as soon as the IME is active.
        // IPC is optional during activation. TSF activation must remain fast
        // even when the Node/host process is not running yet; key handling
        // will use the short reconnect path below.
        const bool pipeConnected = pipe_.ConnectToServer(kPipeName, 250);
        NativeLog::Write("TextService::Activate pipe connect=%d", pipeConnected ? 1 : 0);
        NativeLog::Write("TextService::Activate success this=%p", this);

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Deactivate() override {
        NativeLog::Write("TextService::Deactivate enter this=%p clientId=%lu advised=%d", this,
            static_cast<unsigned long>(clientId_), keySinkAdvised_ ? 1 : 0);
        if (keyMgr_ && keySink_ && keySinkAdvised_ && clientId_ != TF_CLIENTID_NULL) {
            const HRESULT hr = keyMgr_->UnadviseKeyEventSink(clientId_);
            NativeLog::Hr("TextService::Deactivate UnadviseKeyEventSink", hr);
        }
        keySinkAdvised_ = false;
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
        HideCandidates();
        if (uiElementMgr_) {
            uiElementMgr_->Release();
            uiElementMgr_ = nullptr;
        }
        NativeLog::Write("TextService::Deactivate complete this=%p", this);
        return S_OK;
    }

    bool TestKey(ITfContext*, WPARAM wParam, LPARAM, bool& consume);
    bool HandleKey(ITfContext* context, WPARAM wParam, LPARAM, bool& consume);
    void LogActiveProfile(const char* where) const;

private:
    static std::string KeyName(WPARAM vk);
    static int Modifiers();

    bool CallCore(const std::string& type, WPARAM vk, LPARAM lParam, bool& consume,
                  std::string& response);
    bool ApplyResponse(ITfContext* context, const std::string& response);
    bool UpdateCandidates(ITfContext* context, const std::string& response);
    void HideCandidates();

    LONG refCount_;
    ITfThreadMgr* threadMgr_;
    ITfKeystrokeMgr* keyMgr_;
    KeyEventSink* keySink_;
    TfClientId clientId_;
    bool keySinkAdvised_;
    unsigned int nextRequestId_;
    PipeBridge pipe_;
    ITfContext* compositionContext_;
    std::string compositionText_;
    ITfComposition* composition_ = nullptr;
    ITfUIElementMgr* uiElementMgr_;
    CandidateListUIElement* candidateElement_;
    DWORD candidateUiElementId_;
};

HRESULT KeyEventSink::OnSetFocus(BOOL foreground) {
    NativeLog::Write("OnSetFocus service=%p foreground=%d", service_, foreground ? 1 : 0);
    service_->LogActiveProfile(foreground ? "OnSetFocus-foreground" : "OnSetFocus-background");
    return S_OK;
}

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

void TextService::LogActiveProfile(const char* where) const {
    if (!threadMgr_) {
        NativeLog::Write("ActiveProfile where=%s threadMgr=null", where);
        return;
    }
    ITfInputProcessorProfileMgr* profileMgr = nullptr;
    HRESULT hr = threadMgr_->QueryInterface(IID_ITfInputProcessorProfileMgr,
        reinterpret_cast<void**>(&profileMgr));
    if (FAILED(hr)) {
        NativeLog::Write("ActiveProfile where=%s QueryInterface hr=0x%08lX", where, static_cast<unsigned long>(hr));
        return;
    }
    TF_INPUTPROCESSORPROFILE profile{};
    hr = profileMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile);
    if (SUCCEEDED(hr)) {
        NativeLog::Write("ActiveProfile where=%s lang=0x%04X clsid=%08lX-%04X-%04X profile=%08lX-%04X-%04X active=%d",
            where, profile.langid, profile.clsid.Data1, profile.clsid.Data2, profile.clsid.Data3,
            profile.guidProfile.Data1, profile.guidProfile.Data2, profile.guidProfile.Data3);
    } else {
        NativeLog::Write("ActiveProfile where=%s GetActiveProfile hr=0x%08lX", where, static_cast<unsigned long>(hr));
    }
    profileMgr->Release();
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
    if (key.empty()) {
        NativeLog::Write("CallCore type=%s ignored vk=0x%02X", type.c_str(), static_cast<unsigned int>(vk));
        return true;
    }

    if (!pipe_.IsConnected() && !pipe_.ConnectToServer(kPipeName, 50)) {
        NativeLog::Write("CallCore type=%s key=%s pipe unavailable", type.c_str(), key.c_str());
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

    NativeLog::Write("CallCore request id=%u type=%s vk=0x%02X key=%s modifiers=%d",
        id, type.c_str(), static_cast<unsigned int>(vk), key.c_str(), Modifiers());
    if (!pipe_.Call(request, response, id)) {
        NativeLog::Write("CallCore response id=%u failed", id);
        pipe_.Disconnect();
        return false;
    }
    consume = JsonBool(response, "consume", false);
    NativeLog::Write("CallCore response id=%u consume=%d bytes=%zu", id, consume ? 1 : 0, response.size());
    return true;
}

bool TextService::TestKey(ITfContext* context, WPARAM wParam, LPARAM lParam, bool& consume) {
    (void)context;
    // Win+Space belongs to Windows' input-method switcher, not this TIP.
    // Do not forward it to Node as a plain Space key or allow the TSF sink
    // to consume it before the shell can advance to the next input method.
    if (wParam == VK_SPACE &&
        ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))) {
        consume = false;
        NativeLog::Write("TestKey bypass Win+Space");
        return true;
    }
    std::string response;
    return CallCore("testKeyDown", wParam, lParam, consume, response);
}

bool TextService::HandleKey(ITfContext* context, WPARAM wParam, LPARAM lParam, bool& consume) {
    // Win+Space belongs to Windows' input-method switcher. Keep it completely
    // transparent to the IME so Windows can perform the profile transition.
    if (wParam == VK_SPACE &&
        ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))) {
        consume = false;
        NativeLog::Write("HandleKey bypass Win+Space");
        return true;
    }
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
    const std::vector<std::string> candidates = JsonCandidateTexts(response);
    const UINT selected = JsonUInt(response, "selectedCandidate", 0);

    NativeLog::Write("ApplyResponse context=%p compositionBytes=%zu commit=%d candidates=%zu selected=%u responseBytes=%zu",
        context, composition.size(), hasCommit ? 1 : 0, candidates.size(), selected, response.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        NativeLog::Write("ApplyResponse candidate[%zu]=%s", i, candidates[i].c_str());
    }
    if (!context) return false;
    CompositionEditSession session(context, composition_, text, hasCommit);
    HRESULT sessionResult = E_FAIL;
    const HRESULT requestResult = context->RequestEditSession(
        clientId_, &session, TF_ES_SYNC | TF_ES_READWRITE, &sessionResult);
    NativeLog::Write("ApplyResponse RequestEditSession request=0x%08lX session=0x%08lX",
        static_cast<unsigned long>(requestResult), static_cast<unsigned long>(sessionResult));
    if (FAILED(requestResult) || FAILED(sessionResult)) return false;

    if (hasCommit || composition.empty()) {
        HideCandidates();
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
        if (!UpdateCandidates(context, response)) return false;
    }
    return true;
}

bool TextService::UpdateCandidates(ITfContext* context, const std::string& response) {
    const std::vector<std::string> candidates = JsonCandidateTexts(response);
    const UINT selection = JsonUInt(response, "selectedCandidate", 0);
    NativeLog::Write("UpdateCandidates context=%p count=%zu selected=%u existing=%d",
        context, candidates.size(), selection, candidateElement_ ? 1 : 0);
    for (size_t i = 0; i < candidates.size(); ++i) {
        NativeLog::Write("UpdateCandidates candidate[%zu]=%s", i, candidates[i].c_str());
    }
    if (candidates.empty() || !context) {
        NativeLog::Write("UpdateCandidates -> hide (empty=%d context=%d)", candidates.empty() ? 1 : 0, context ? 1 : 0);
        HideCandidates();
        return true;
    }
    if (!uiElementMgr_) {
        if (!threadMgr_) return false;
        const HRESULT hr = threadMgr_->QueryInterface(IID_ITfUIElementMgr, reinterpret_cast<void**>(&uiElementMgr_));
        NativeLog::Hr("UpdateCandidates QueryInterface(ITfUIElementMgr)", hr);
        if (FAILED(hr)) return false;
    }

    if (candidateElement_) {
        candidateElement_->Update(candidates, selection);
        candidateElement_->Show(TRUE);
        NativeLog::Write("UpdateCandidates -> updating UI element id=%lu",
            static_cast<unsigned long>(candidateUiElementId_));
        const HRESULT hr = uiElementMgr_->UpdateUIElement(candidateUiElementId_);
        NativeLog::Hr("UpdateCandidates UpdateUIElement", hr);
        return SUCCEEDED(hr);
    }

    ITfDocumentMgr* documentMgr = nullptr;
    HRESULT hr = context->GetDocumentMgr(&documentMgr);
    if (FAILED(hr) || !documentMgr) {
        NativeLog::Hr("UpdateCandidates GetDocumentMgr", FAILED(hr) ? hr : E_UNEXPECTED);
        return false;
    }
    auto* element = new (std::nothrow) CandidateListUIElement(documentMgr, candidates, selection);
    documentMgr->Release();
    if (!element) return false;

    BOOL show = TRUE;
    DWORD elementId = 0;
    hr = uiElementMgr_->BeginUIElement(static_cast<ITfUIElement*>(element), &show, &elementId);
    NativeLog::Hr("UpdateCandidates BeginUIElement", hr);
    if (FAILED(hr)) {
        element->Release();
        return false;
    }
    element->Show(show ? TRUE : FALSE);
    candidateElement_ = element;
    candidateUiElementId_ = elementId;
    NativeLog::Write("UpdateCandidates -> created UI element id=%lu show=%d",
        static_cast<unsigned long>(elementId), show ? 1 : 0);
    hr = uiElementMgr_->UpdateUIElement(candidateUiElementId_);
    NativeLog::Hr("UpdateCandidates initial UpdateUIElement", hr);
    return SUCCEEDED(hr);
}

void TextService::HideCandidates() {
    if (candidateElement_) {
        NativeLog::Write("HideCandidates id=%lu", static_cast<unsigned long>(candidateUiElementId_));
        candidateElement_->Show(FALSE);
        if (uiElementMgr_ && candidateUiElementId_ != 0) {
            const HRESULT hr = uiElementMgr_->EndUIElement(candidateUiElementId_);
            NativeLog::Hr("HideCandidates EndUIElement", hr);
        }
        candidateElement_->Release();
        candidateElement_ = nullptr;
        candidateUiElementId_ = 0;
    }
}

HRESULT KeyEventSink::OnTestKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
    if (!eaten) return E_POINTER;
    bool consume = false;
    const bool ok = service_->TestKey(context, wParam, lParam, consume);
    *eaten = ok && consume ? TRUE : FALSE;
    NativeLog::Write("OnTestKeyDown context=%p vk=0x%02X ok=%d consume=%d eaten=%d",
        context, static_cast<unsigned int>(wParam), ok ? 1 : 0, consume ? 1 : 0, *eaten ? 1 : 0);
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
    NativeLog::Write("OnKeyDown context=%p vk=0x%02X ok=%d consume=%d eaten=%d",
        context, static_cast<unsigned int>(wParam), ok ? 1 : 0, consume ? 1 : 0, *eaten ? 1 : 0);
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
    const HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfiles, reinterpret_cast<void**>(profiles));
    NativeLog::Hr("CreateProfiles", hr);
    return hr;
}

HRESULT RegisterTipProfile() {
    NativeLog::Write("RegisterTipProfile begin");
    ITfInputProcessorProfiles* profiles = nullptr;
    HRESULT hr = CreateProfiles(&profiles);
    if (FAILED(hr)) return hr;
    hr = profiles->Register(CLSID_TypeScriptWindowsIme);
    NativeLog::Hr("ITfInputProcessorProfiles::Register", hr);
    if (SUCCEEDED(hr)) {
        const LANGID languages[] = { 0x0409, 0x0804, 0x0411 };
        for (const LANGID langid : languages) {
            hr = profiles->AddLanguageProfile(CLSID_TypeScriptWindowsIme, langid,
                GUID_TypeScriptWindowsImeProfile, kTypeScriptWindowsImeName,
                static_cast<ULONG>(wcslen(kTypeScriptWindowsImeName)), nullptr, 0, 0);
            NativeLog::Write("AddLanguageProfile langid=0x%04X hr=0x%08lX", langid, static_cast<unsigned long>(hr));
            if (FAILED(hr)) break;
        }
    }
    if (SUCCEEDED(hr)) {
        ITfCategoryMgr* categoryMgr = nullptr;
        hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfCategoryMgr, reinterpret_cast<void**>(&categoryMgr));
        NativeLog::Hr("Create ITfCategoryMgr", hr);
        if (SUCCEEDED(hr)) {
            hr = categoryMgr->RegisterCategory(CLSID_TypeScriptWindowsIme,
                GUID_TFCAT_TIP_KEYBOARD, CLSID_TypeScriptWindowsIme);
            NativeLog::Hr("ITfCategoryMgr::RegisterCategory", hr);
            categoryMgr->Release();
        }
    }
    profiles->Release();
    NativeLog::Hr("RegisterTipProfile result", hr);
    return hr;
}

HRESULT UnregisterTipProfile() {
    NativeLog::Write("UnregisterTipProfile begin");
    ITfInputProcessorProfiles* profiles = nullptr;
    HRESULT hr = CreateProfiles(&profiles);
    if (FAILED(hr)) return hr;
    hr = profiles->Unregister(CLSID_TypeScriptWindowsIme);
    NativeLog::Hr("ITfInputProcessorProfiles::Unregister", hr);
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
    NativeLog::Write("RunComRegistration begin unregister=%d", unregister ? 1 : 0);
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    NativeLog::Hr("CoInitializeEx", init);
    const bool shouldUninitialize = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return init;

    HRESULT hr = unregister ? UnregisterTipProfile() : RegisterComServer();
    if (SUCCEEDED(hr)) hr = unregister ? UnregisterComServer() : RegisterTipProfile();
    if (!unregister && FAILED(hr)) UnregisterComServer();
    if (shouldUninitialize) CoUninitialize();
    NativeLog::Hr("RunComRegistration result", hr);
    return hr;
}
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* object) {
    NativeLog::Write("DllGetClassObject clsidMatch=%d object=%p", clsid == CLSID_TypeScriptWindowsIme ? 1 : 0, object);
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
