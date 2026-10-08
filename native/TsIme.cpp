#define INITGUID
#include "TsIme.h"
#include "CandidateWindow.h"
#include "PipeBridge.h"
#include "NativeLog.h"

#include <new>
#include <string>
#include <vector>

namespace {
HRESULT RunComRegistration(bool unregister);

class TextService;


volatile LONG g_serverLocks = 0;
volatile LONG g_objectCount = 0;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\TypeScriptWindowsIME.Tsf";

// The candidate window asks for the caret rectangle from a timer, so the
// service that owns the current composition has to be reachable from there.
TextService* g_activeService = nullptr;
bool ReadCaretRectFromService(RECT& caret);

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

std::vector<std::wstring> WideCandidates(const std::vector<std::string>& candidates) {
    std::vector<std::wstring> wide;
    wide.reserve(candidates.size());
    for (const std::string& candidate : candidates) wide.push_back(Utf8ToWide(candidate));
    return wide;
}

class TextService;

// Re-reads the caret rectangle of the live composition. Applications lay out
// composition text after the edit session that inserted it, so the first
// ITfContextView::GetTextExt call often reports nothing useful.
class CaretProbeSession final : public ITfEditSession {
public:
    CaretProbeSession(ITfContext* context, ITfComposition* composition, RECT* caret)
        : refCount_(1), context_(context), composition_(composition), caret_(caret), hr_(E_FAIL) {
        if (context_) context_->AddRef();
        if (composition_) composition_->AddRef();
    }
    ~CaretProbeSession() {
        if (composition_) composition_->Release();
        if (context_) context_->Release();
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
        if (!context_ || !composition_ || !caret_) return E_UNEXPECTED;
        ITfRange* range = nullptr;
        HRESULT hr = composition_->GetRange(&range);
        if (FAILED(hr)) {
            NativeLog::Hr("CaretProbe GetRange", hr);
            hr_ = hr;
            return hr;
        }
        ITfContextView* view = nullptr;
        hr = context_->GetActiveView(&view);
        if (FAILED(hr) || !view) {
            NativeLog::Hr("CaretProbe GetActiveView", FAILED(hr) ? hr : E_UNEXPECTED);
            range->Release();
            hr_ = FAILED(hr) ? hr : E_UNEXPECTED;
            return hr_;
        }

        RECT rect{};
        BOOL clipped = FALSE;
        ITfRange* end = nullptr;
        if (SUCCEEDED(range->Clone(&end)) && end) {
            end->Collapse(ec, TF_ANCHOR_END);
            hr = view->GetTextExt(ec, end, &rect, &clipped);
            end->Release();
        } else {
            hr = E_FAIL;
        }
        if (FAILED(hr) || rect.right <= rect.left) {
            NativeLog::Write("CaretProbe caret end hr=0x%08lX rect=%ld,%ld,%ld,%ld",
                static_cast<unsigned long>(hr), rect.left, rect.top, rect.right, rect.bottom);
            RECT whole{};
            hr = view->GetTextExt(ec, range, &whole, &clipped);
            if (SUCCEEDED(hr) && whole.right > whole.left) rect = whole;
        }
        view->Release();
        range->Release();

        if (rect.right > rect.left && rect.bottom > rect.top) *caret_ = rect;
        NativeLog::Write("CaretProbe result hr=0x%08lX rect=%ld,%ld,%ld,%ld",
            static_cast<unsigned long>(hr), rect.left, rect.top, rect.right, rect.bottom);
        hr_ = FAILED(hr) ? hr : (rect.right > rect.left ? S_OK : E_FAIL);
        return hr_;
    }
private:
    LONG refCount_;
    ITfContext* context_;
    ITfComposition* composition_;
    RECT* caret_;
    HRESULT hr_;
};

class CompositionSink final : public ITfCompositionSink {
public:
    explicit CompositionSink(TextService* service) : refCount_(1), service_(service) {}
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
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie, ITfComposition*) override;
private:
    LONG refCount_;
    TextService* service_;
};

class CompositionEditSession final : public ITfEditSession {
public:
    CompositionEditSession(TextService* service, ITfContext* context, ITfComposition* composition,
                           const std::string& text, bool commit)
        : refCount_(1), service_(service), context_(context), composition_(composition), text_(text),
          commit_(commit), hr_(E_FAIL) {
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
        if (FAILED(hr)) {
            NativeLog::Hr("CompositionEditSession QueryInterface(ITfContextComposition)", hr);
            return hr;
        }

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

        if (composition_) {
            // An application may end the composition behind our back. A dead
            // ITfComposition must not poison every later edit session.
            ITfRange* probe = nullptr;
            if (FAILED(composition_->GetRange(&probe)) || !probe) {
                NativeLog::Write("CompositionEditSession stale composition discarded");
                composition_->Release();
                composition_ = nullptr;
            } else {
                probe->Release();
            }
        }

        if (!composition_) {
            TF_SELECTION selection{};
            ULONG fetched = 0;
            hr = context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
            if (FAILED(hr) || fetched != 1) {
                contextComposition->Release();
                hr_ = FAILED(hr) ? hr : E_FAIL;
                NativeLog::Write("CompositionEditSession GetSelection failed hr=0x%08lX fetched=%lu",
                    static_cast<unsigned long>(hr_), fetched);
                return hr_;
            }

            auto* sink = new (std::nothrow) CompositionSink(service_);
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
                NativeLog::Hr("CompositionEditSession StartComposition", hr);
                return hr_;
            }
            NativeLog::Write("CompositionEditSession StartComposition ok composition=%p", composition_);
        }

        ITfRange* range = nullptr;
        hr = composition_->GetRange(&range);
        if (SUCCEEDED(hr)) {
            const std::wstring wideText = Utf8ToWide(text_);
            hr = range->SetText(ec, 0, wideText.c_str(), static_cast<LONG>(wideText.size()));
            if (SUCCEEDED(hr)) ReadCaretRect(ec, range);
            if (SUCCEEDED(hr) && commit_) {
                hr = composition_->EndComposition(ec);
            }
            range->Release();
        }
        if (FAILED(hr)) NativeLog::Write("CompositionEditSession apply text failed hr=0x%08lX", static_cast<unsigned long>(hr));

        if (commit_ && composition_) {
            composition_->Release();
            composition_ = nullptr;
        }
        contextComposition->Release();
        hr_ = hr;
        return hr;
    }

    // Screen coordinates of the insertion point at the end of the composition.
    // The candidate window is drawn there. Applications that do not implement
    // ITfContextView leave this empty and the window falls back to the caret.
    const RECT& CaretRect() const { return caretRect_; }

    HRESULT Result() const { return hr_; }
private:
    void ReadCaretRect(TfEditCookie ec, ITfRange* range) {
        ITfContextView* view = nullptr;
        if (FAILED(context_->GetActiveView(&view)) || !view) {
            return;
        }
        // The insertion point anchors the window. Applications that have not
        // laid out the fresh composition yet fail on the collapsed range, so
        // the whole composition rectangle is used as the fallback anchor.
        ITfRange* end = nullptr;
        if (SUCCEEDED(range->Clone(&end)) && end) {
            end->Collapse(ec, TF_ANCHOR_END);
            RECT rect{};
            BOOL clipped = FALSE;
            if (SUCCEEDED(view->GetTextExt(ec, end, &rect, &clipped))) caretRect_ = rect;
            end->Release();
        }
        if (caretRect_.right <= caretRect_.left) {
            RECT rect{};
            BOOL clipped = FALSE;
            if (SUCCEEDED(view->GetTextExt(ec, range, &rect, &clipped))) caretRect_ = rect;
        }
        view->Release();
    }

    LONG refCount_;
    TextService* service_;
    ITfContext* context_;
    std::string text_;
    bool commit_;
    HRESULT hr_;
    RECT caretRect_{};
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
        g_activeService = this;
        CandidateWindow::SetCaretProvider(&ReadCaretRectFromService);
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
        if (g_activeService == this) {
            g_activeService = nullptr;
            CandidateWindow::SetCaretProvider(nullptr);
        }
        CandidateWindow::Destroy();
        NativeLog::Write("TextService::Deactivate complete this=%p", this);
        return S_OK;
    }

bool TestKey(ITfContext*, WPARAM wParam, LPARAM, bool& consume);
    bool HandleKey(ITfContext* context, WPARAM wParam, LPARAM, bool& consume);
    void LogActiveProfile(const char* where) const;
    void OnCompositionTerminated();
    bool ProbeCaretRect(RECT& caret);

private:
    static std::string KeyName(WPARAM vk);
    static int Modifiers();

bool CallCore(const std::string& type, WPARAM vk, LPARAM lParam, bool& consume,
                  std::string& response);
    bool ApplyResponse(ITfContext* context, const std::string& response);
    bool UpdateCandidates(ITfContext* context, const std::string& response, RECT caret);
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
    RECT lastCaretRect_{};
    ITfComposition* composition_ = nullptr;
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
    CompositionEditSession session(this, context, composition_, text, hasCommit);
    HRESULT sessionResult = E_FAIL;
    const HRESULT requestResult = context->RequestEditSession(
        clientId_, &session, TF_ES_SYNC | TF_ES_READWRITE, &sessionResult);
    NativeLog::Write("ApplyResponse RequestEditSession request=0x%08lX session=0x%08lX",
        static_cast<unsigned long>(requestResult), static_cast<unsigned long>(sessionResult));
    if (FAILED(requestResult) || FAILED(sessionResult)) {
        // A rejected edit session must not stay broken: drop the composition so
        // the next keystroke starts a fresh one instead of failing forever.
        NativeLog::Write("ApplyResponse edit session rejected; dropping composition state");
        if (composition_) {
            composition_->Release();
            composition_ = nullptr;
        }
        compositionContext_ = nullptr;
        compositionText_.clear();
        HideCandidates();
        return false;
    }

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
        if (!UpdateCandidates(context, response, session.CaretRect())) return false;
    }
    return true;
}

bool TextService::UpdateCandidates(ITfContext* context, const std::string& response, RECT caret) {
    const std::vector<std::string> candidates = JsonCandidateTexts(response);
    const UINT selection = JsonUInt(response, "selectedCandidate", 0);
    // Keep the last known anchor: an application can report no layout right
    // after the composition text changed, and the window must stay where it was.
    if (caret.right > caret.left || caret.bottom > caret.top) {
        lastCaretRect_ = caret;
    } else {
        caret = lastCaretRect_;
    }
    NativeLog::Write("UpdateCandidates context=%p count=%zu selected=%u caret=%ld,%ld,%ld,%ld",
        context, candidates.size(), selection, caret.left, caret.top, caret.right, caret.bottom);
    for (size_t i = 0; i < candidates.size(); ++i) {
        NativeLog::Write("UpdateCandidates candidate[%zu]=%s", i, candidates[i].c_str());
    }
    if (candidates.empty() || !context) {
        NativeLog::Write("UpdateCandidates -> hide (empty=%d context=%d)", candidates.empty() ? 1 : 0, context ? 1 : 0);
        HideCandidates();
        return true;
    }
    if (!CandidateWindow::Show(WideCandidates(candidates), selection, caret)) {
        NativeLog::Write("UpdateCandidates -> CandidateWindow::Show failed");
        return false;
    }
    return true;
}

void TextService::HideCandidates() {
    CandidateWindow::Hide();
}

void TextService::OnCompositionTerminated() {
    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    compositionContext_ = nullptr;
    compositionText_.clear();
    HideCandidates();
}

bool TextService::ProbeCaretRect(RECT& caret) {
    if (!compositionContext_ || !composition_) return false;
    CaretProbeSession session(compositionContext_, composition_, &caret);
    HRESULT sessionResult = E_FAIL;
    const HRESULT request = compositionContext_->RequestEditSession(
        clientId_, &session, TF_ES_SYNC | TF_ES_READWRITE, &sessionResult);
    if (FAILED(request) || FAILED(sessionResult)) {
        NativeLog::Write("ProbeCaretRect edit session failed request=0x%08lX session=0x%08lX",
            static_cast<unsigned long>(request), static_cast<unsigned long>(sessionResult));
        return false;
    }
    return caret.right > caret.left && caret.bottom > caret.top;
}

bool ReadCaretRectFromService(RECT& caret) {
    TextService* service = g_activeService;
    return service ? service->ProbeCaretRect(caret) : false;
}

HRESULT CompositionSink::OnCompositionTerminated(TfEditCookie, ITfComposition*) {
    // The application ended the composition on its own. Keeping the stale
    // ITfComposition makes every later edit session fail, so the service drops
    // it here instead of waiting for the next commit.
    NativeLog::Write("CompositionSink::OnCompositionTerminated service=%p", service_);
    if (service_) service_->OnCompositionTerminated();
    return S_OK;
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
