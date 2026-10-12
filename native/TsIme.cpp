#define INITGUID
#include "TsIme.h"

#include "PipeBridge.h"
#include "NativeLog.h"
#include <ctfutb.h>
#include <ocidl.h>

#include <new>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace {
HRESULT RunComRegistration(bool unregister);

HRESULT GetRangeExtent(ITfRange* range, LONG* start, LONG* length) {
    if (!range || !start || !length) return E_POINTER;
    ITfRangeACP* rangeAcp = nullptr;
    const HRESULT hr = range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void**>(&rangeAcp));
    if (FAILED(hr)) return hr;
    const HRESULT extentHr = rangeAcp->GetExtent(start, length);
    rangeAcp->Release();
    return extentHr;
}
class TextService;

// TSF language-bar item: Node's mode response drives the visible CN/EN label.
DEFINE_GUID(GUID_TsImeLangBarItem, 0x4f77a7b1, 0x2d31, 0x4c62, 0x91, 0x42, 0x38, 0x1b, 0x9e, 0x50, 0x7c, 0x11);
class ModeLangBarItem final : public ITfLangBarItemButton, public ITfSource {
public:
    ModeLangBarItem() : refs_(1), chinese_(true), sink_(nullptr), cookie_(0) {}
    ~ModeLangBarItem() { if (sink_) sink_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfLangBarItem || iid == IID_ITfLangBarItemButton) *out = static_cast<ITfLangBarItemButton*>(this);
        else if (iid == IID_ITfSource) *out = static_cast<ITfSource*>(this); else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n=static_cast<ULONG>(InterlockedDecrement(&refs_)); if (!n) delete this; return n; }
    void SetMode(bool chinese) { if (chinese_ != chinese) { chinese_=chinese; if (sink_) sink_->OnUpdate(TF_LBI_TEXT); } }
    HRESULT STDMETHODCALLTYPE GetInfo(TF_LANGBARITEMINFO* i) override {
        if (!i) return E_POINTER; ZeroMemory(i,sizeof(*i)); i->clsidService=CLSID_TypeScriptWindowsIme;
        i->guidItem=GUID_TsImeLangBarItem; i->dwStyle=TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_BTN_TOGGLE | TF_LBI_STYLE_SHOWNINTRAY;
        i->ulSort=0; wcsncpy_s(i->szDescription,L"TypeScript Windows IME mode",_TRUNCATE); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStatus(DWORD* s) override { if(!s)return E_POINTER; *s=0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Show(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTooltipString(BSTR* s) override { if(!s)return E_POINTER; *s=SysAllocString(chinese_?L"TypeScript IME - Chinese":L"TypeScript IME - English"); return *s?S_OK:E_OUTOFMEMORY; }
    HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick,POINT,const RECT*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetIcon(HICON* i) override { if(!i)return E_POINTER; *i=nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE GetText(BSTR* s) override { if(!s)return E_POINTER; *s=SysAllocString(chinese_?L"\u4E2D":L"\u82F1"); return *s?S_OK:E_OUTOFMEMORY; }
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid,IUnknown* punk,DWORD* cookie) override {
        if(!punk||!cookie)return E_POINTER; if(iid!=IID_ITfLangBarItemSink)return E_NOINTERFACE;
        if(sink_)return E_UNEXPECTED; HRESULT hr=punk->QueryInterface(IID_ITfLangBarItemSink,reinterpret_cast<void**>(&sink_));
        if(SUCCEEDED(hr)){cookie_=1;*cookie=cookie_;} return hr;
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) override { if(!sink_||cookie!=cookie_)return E_INVALIDARG; sink_->Release();sink_=nullptr;cookie_=0;return S_OK; }
private:
    LONG refs_; bool chinese_; ITfLangBarItemSink* sink_; DWORD cookie_;
};
class TextService;


volatile LONG g_serverLocks = 0;
volatile LONG g_objectCount = 0;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\TypeScriptWindowsIME.Tsf";
// Where the text service looks for the core. KeyHandlingMockTest points this
// at a private pipe so its scripted core cannot race a running Host; nothing
// in production ever changes it.
const wchar_t* g_servicePipeName = kPipeName;

// The candidate window asks for the caret rectangle from a timer, so the
// service that owns the current composition has to be reachable from there.
TextService* g_activeService = nullptr;
HWND g_caretRetryWindow = nullptr;
constexpr UINT_PTR kCaretRetryTimer = 1;
constexpr int kCaretRetryDelayMs = 40;
constexpr int kCaretRetries = 12;
bool CreateCaretRetryWindow();
void DestroyCaretRetryWindow();
// TSF activates one text service per application process and the Host
// multiplexes every one of them over a single Node connection. Request ids
// restart at 1 in each process, so they cannot identify a request on their
// own. A session id that is unique per process lets the Host route each
// response back to the client that asked, and lets Node keep one composition
// per application instead of one shared composition.
std::atomic<unsigned long long> g_nextSessionId{1};
unsigned long long AllocateSessionId() {
    const unsigned long long processId = GetCurrentProcessId();
    // The low bits hold a per-process counter, the high bits the pid. Shifting
    // the pid up keeps the value inside the safe-integer range Node validates.
    const unsigned long long counter = g_nextSessionId.fetch_add(1) - 1;
    return (processId << 20) | (counter & 0xfffff);
}
void LogForegroundContext(const char* where, ITfContext* context) {
    const HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &pid);
    wchar_t image[MAX_PATH]{};
    HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    DWORD imageLength = ARRAYSIZE(image);
    if (process) {
        QueryFullProcessImageNameW(process, 0, image, &imageLength);
        CloseHandle(process);
    }
    wchar_t className[128]{};
    if (foreground) GetClassNameW(foreground, className, ARRAYSIZE(className));
    char imageUtf8[MAX_PATH * 3]{};
    char classUtf8[384]{};
    WideCharToMultiByte(CP_UTF8, 0, image, -1, imageUtf8, sizeof(imageUtf8), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, className, -1, classUtf8, sizeof(classUtf8), nullptr, nullptr);
    NativeLog::Write("AppContext where=%s hwnd=%p pid=%lu image=%s class=%s context=%p",
        where, foreground, static_cast<unsigned long>(pid),
        imageUtf8[0] ? imageUtf8 : "<unknown>", classUtf8[0] ? classUtf8 : "<unknown>", context);
}
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
                // Ending a TSF composition does not erase the text it owns.
                // Clear the range first, or the next composition can be
                // inserted after stale pinyin (for example, producing "abc").
                ITfRange* emptyRange = nullptr;
                hr = composition_->GetRange(&emptyRange);
                if (SUCCEEDED(hr) && emptyRange) {
                    hr = emptyRange->SetText(ec, 0, L"", 0);
                    NativeLog::Write("CompositionEditSession clear-empty SetText hr=0x%08lX",
                        static_cast<unsigned long>(hr));
                    emptyRange->Release();
                } else {
                    NativeLog::Write("CompositionEditSession clear-empty GetRange failed hr=0x%08lX",
                        static_cast<unsigned long>(hr));
                }
                if (SUCCEEDED(hr)) hr = composition_->EndComposition(ec);
                NativeLog::Write("CompositionEditSession clear-empty EndComposition hr=0x%08lX",
                    static_cast<unsigned long>(hr));
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
            LONG beforeStart = 0, beforeLength = 0;
            const HRESULT beforeExtentHr = GetRangeExtent(range, &beforeStart, &beforeLength);
            NativeLog::Write("TSFCommit begin commit=%d textChars=%zu rangeExtentHr=0x%08lX start=%ld length=%ld composition=%p",
                commit_ ? 1 : 0, wideText.size(), static_cast<unsigned long>(beforeExtentHr),
                beforeStart, beforeLength, composition_);
            hr = range->SetText(ec, 0, wideText.c_str(), static_cast<LONG>(wideText.size()));
            LONG afterStart = 0, afterLength = 0;
            const HRESULT afterExtentHr = SUCCEEDED(hr) ? GetRangeExtent(range, &afterStart, &afterLength) : hr;
            NativeLog::Write("TSFCommit SetText hr=0x%08lX rangeExtentHr=0x%08lX start=%ld length=%ld",
                static_cast<unsigned long>(hr), static_cast<unsigned long>(afterExtentHr), afterStart, afterLength);
            if (SUCCEEDED(hr)) {
                ReadCaretRect(ec, range);

                // SetText replaces the composition text, but relying on the
                // application's implicit selection update can leave the caret
                // at the beginning of the newly inserted candidate. Explicitly
                // place the TSF selection at the end of the updated range.
                ITfRange* caretRange = nullptr;
                HRESULT caretHr = range->Clone(&caretRange);
                if (SUCCEEDED(caretHr) && caretRange) {
                    caretHr = caretRange->Collapse(ec, TF_ANCHOR_END);
                    if (SUCCEEDED(caretHr)) {
                        TF_SELECTION selection{};
                        selection.range = caretRange;
                        selection.style.ase = TF_AE_END;
                        selection.style.fInterimChar = FALSE;
                        LONG caretStart = 0, caretLength = 0;
                        const HRESULT caretExtentHr = GetRangeExtent(caretRange, &caretStart, &caretLength);
                        NativeLog::Write("TSFCommit SetSelection request start=%ld length=%ld extentHr=0x%08lX",
                            caretStart, caretLength, static_cast<unsigned long>(caretExtentHr));
                        caretHr = context_->SetSelection(ec, 1, &selection);
                        NativeLog::Write("TSFCommit SetSelection hr=0x%08lX", static_cast<unsigned long>(caretHr));
                        TF_SELECTION actualSelection{};
                        ULONG actualFetched = 0;
                        const HRESULT getSelectionHr = context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &actualSelection, &actualFetched);
                        LONG actualStart = 0, actualLength = 0;
                        HRESULT actualExtentHr = E_FAIL;
                        if (SUCCEEDED(getSelectionHr) && actualFetched == 1 && actualSelection.range) {
                            actualExtentHr = GetRangeExtent(actualSelection.range, &actualStart, &actualLength);
                            actualSelection.range->Release();
                        }
                        NativeLog::Write("TSFCommit selectionAfterSet hr=0x%08lX fetched=%lu extentHr=0x%08lX start=%ld length=%ld",
                            static_cast<unsigned long>(getSelectionHr), actualFetched,
                            static_cast<unsigned long>(actualExtentHr), actualStart, actualLength);
                    }
                    caretRange->Release();
                }
                if (FAILED(caretHr)) {
                    NativeLog::Write("CompositionEditSession SetSelection(end) failed hr=0x%08lX",
                        static_cast<unsigned long>(caretHr));
                    hr = caretHr;
                }
            }
            if (SUCCEEDED(hr) && commit_) {
                NativeLog::Write("TSFCommit EndComposition begin");
                hr = composition_->EndComposition(ec);
                NativeLog::Write("TSFCommit EndComposition hr=0x%08lX", static_cast<unsigned long>(hr));
                TF_SELECTION finalSelection{};
                ULONG finalFetched = 0;
                const HRESULT finalSelectionHr = context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &finalSelection, &finalFetched);
                LONG finalStart = 0, finalLength = 0;
                HRESULT finalExtentHr = E_FAIL;
                if (SUCCEEDED(finalSelectionHr) && finalFetched == 1 && finalSelection.range) {
                    finalExtentHr = GetRangeExtent(finalSelection.range, &finalStart, &finalLength);
                    finalSelection.range->Release();
                }
                NativeLog::Write("TSFCommit selectionAfterEnd hr=0x%08lX fetched=%lu extentHr=0x%08lX start=%ld length=%ld",
                    static_cast<unsigned long>(finalSelectionHr), finalFetched,
                    static_cast<unsigned long>(finalExtentHr), finalStart, finalLength);
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

// XAML/WinUI hosts (Windows Terminal, modern Store apps) drive the Windows 8+
// text stack and only ever ask for IID_ITfTextInputProcessorEx. A TIP that
// exposes the plain ITfTextInputProcessor alone is skipped by the IME switcher
// in those windows, so the extended interface is not optional here.
class TextService final : public ITfTextInputProcessorEx {
public:
    TextService()
        : refCount_(1), threadMgr_(nullptr), keyMgr_(nullptr), keySink_(nullptr),
          clientId_(TF_CLIENTID_NULL), keySinkAdvised_(false), nextRequestId_(1),
          sessionId_(AllocateSessionId()), pipe_(250), compositionContext_(nullptr),
          activateFlags_(0), activatedViaEx_(false), langBarMgr_(nullptr), langBarItem_(nullptr) {
        InterlockedIncrement(&g_objectCount);
    }

    ~TextService() {
        Deactivate();
        InterlockedDecrement(&g_objectCount);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessorEx) {
            *object = static_cast<ITfTextInputProcessorEx*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_ITfTextInputProcessor) {
            *object = static_cast<ITfTextInputProcessor*>(this);
            AddRef();
            return S_OK;
        }
        NativeLog::Write("TextService::QueryInterface unsupported iid=%08lX-%04X-%04X",
            static_cast<unsigned long>(riid.Data1), riid.Data2, riid.Data3);
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
const bool pipeConnected = pipe_.ConnectToServer(g_servicePipeName, 250);
        NativeLog::Write("TextService::Activate pipe connect=%d", pipeConnected ? 1 : 0);
        HRESULT langHr = threadMgr_->QueryInterface(IID_ITfLangBarItemMgr, reinterpret_cast<void**>(&langBarMgr_));
        if (SUCCEEDED(langHr) && langBarMgr_) {
            langBarItem_ = new (std::nothrow) ModeLangBarItem();
            if (langBarItem_) {
                langHr = langBarMgr_->AddItem(static_cast<ITfLangBarItemButton*>(langBarItem_));
                if (FAILED(langHr)) { langBarItem_->Release(); langBarItem_ = nullptr; }
            }
        }
        NativeLog::Write("TextService::Activate language-bar registration hr=0x%08lX item=%p",
            static_cast<unsigned long>(langHr), langBarItem_);
        g_activeService = this;
        CreateCaretRetryWindow();
        NativeLog::Write("TextService::Activate success this=%p", this);

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* threadMgr, TfClientId clientId,
                                         DWORD flags) override {
        // The candidate list is drawn by the Host process and the edits go
        // through ITfContext, so the TF_TMAE_* hints (secure mode, 16-bit
        // hosts, console-only activation) do not change what has to happen
        // here. Run the same activation so the key sink is advised identically
        // on both paths.
        NativeLog::Write("TextService::ActivateEx enter this=%p clientId=%lu flags=0x%08lX secure=%d "
                         "uielementOnly=%d noActivateTip=%d console=%d wow16=%d",
            this, static_cast<unsigned long>(clientId), static_cast<unsigned long>(flags),
            (flags & TF_TMAE_SECUREMODE) ? 1 : 0, (flags & TF_TMAE_UIELEMENTENABLEDONLY) ? 1 : 0,
            (flags & TF_TMAE_NOACTIVATETIP) ? 1 : 0, (flags & TF_TMAE_CONSOLE) ? 1 : 0,
            (flags & TF_TMAE_WOW16) ? 1 : 0);
        const HRESULT hr = Activate(threadMgr, clientId);
        // Activate runs Deactivate first, so the state is only final here.
        activateFlags_ = flags;
        activatedViaEx_ = SUCCEEDED(hr);
        NativeLog::Write("TextService::ActivateEx result hr=0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Deactivate() override {
        NativeLog::Write("TextService::Deactivate enter this=%p clientId=%lu advised=%d viaEx=%d", this,
            static_cast<unsigned long>(clientId_), keySinkAdvised_ ? 1 : 0, activatedViaEx_ ? 1 : 0);
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
        if (langBarMgr_ && langBarItem_) langBarMgr_->RemoveItem(static_cast<ITfLangBarItemButton*>(langBarItem_));
        if (langBarItem_) { langBarItem_->Release(); langBarItem_ = nullptr; }
        if (langBarMgr_) { langBarMgr_->Release(); langBarMgr_ = nullptr; }
        if (threadMgr_) {
            threadMgr_->Release();
            threadMgr_ = nullptr;
        }
        clientId_ = TF_CLIENTID_NULL;
        // The list lives on the Host's own window, not on this document: the
        // service is going away and has to take it down before the pipe it is
        // sent on is disconnected.
        HideCandidates();
        pipe_.Disconnect();
        compositionContext_ = nullptr;
        if (composition_) {
            composition_->Release();
            composition_ = nullptr;
        }

        if (g_activeService == this) {
            g_activeService = nullptr;
        }
        DestroyCaretRetryWindow();
        activateFlags_ = 0;
        activatedViaEx_ = false;
        NativeLog::Write("TextService::Deactivate complete this=%p", this);
        return S_OK;
    }

bool TestKey(ITfContext*, WPARAM wParam, LPARAM, bool& consume);
    bool HandleKey(ITfContext* context, WPARAM wParam, LPARAM, bool& consume);
    void LogActiveProfile(const char* where) const;
    void OnCompositionTerminated();
    bool ProbeCaretRect(RECT& caret);
    void RetryCaret();
    // The list is drawn on the Host's own window, so it does not follow the
    // caret: the key sink calls these when TSF moves the focus to or from this
    // thread, and Deactivate calls Hide before the pipe goes away.
    void HideCandidates();
    void ShowCandidatesForComposition();
    // Lets the adapter tests assert that both activation paths really advise
    // the key sink instead of inferring it from log output.
    bool KeySinkAdvised() const { return keySinkAdvised_; }
    // Lets the adapter tests assert that ActivateEx ran its own body and kept
    // the TF_TMAE_* hints the modern stack passed in.
    DWORD ActivateFlags() const { return activateFlags_; }
    // KeyEventSink uses narrow wrappers for diagnostic logging.
    int CurrentModifiers() { return Modifiers(); }
    static std::string DiagnosticKeyName(WPARAM vk) { return KeyName(vk); }
    // Deterministic modifier injection for the native mock test; disabled by default.
    void SetModifierOverrideForTest(int modifiers) { modifierOverrideForTest_ = modifiers; }

private:
    static std::string KeyName(WPARAM vk);
    int Modifiers() const;

bool CallCore(const std::string& type, WPARAM vk, LPARAM lParam, bool& consume,
                  std::string& response);
    bool ApplyInputMode(const std::string& response);
    bool ApplyResponse(ITfContext* context, const std::string& response);
    bool UpdateCandidates(ITfContext* context, const std::string& response, RECT caret);

    LONG refCount_;
    ITfThreadMgr* threadMgr_;
    ITfKeystrokeMgr* keyMgr_;
    KeyEventSink* keySink_;
    TfClientId clientId_;
    bool keySinkAdvised_;
    unsigned int nextRequestId_;
    // Identifies this text service across the shared Host connection.
    unsigned long long sessionId_;
    // Serialize key callbacks so request IDs and pipe responses cannot race.
    std::mutex callCoreMutex_;
    PipeBridge pipe_;
    ITfContext* compositionContext_;
    // Set by ActivateEx so a regression in either activation path is visible in
    // the log instead of showing up as an unexplained missing key sink.
    DWORD activateFlags_;
    bool activatedViaEx_;
    ITfLangBarItemMgr* langBarMgr_;
    ModeLangBarItem* langBarItem_;
    std::string lastCandidateResponse_;
    int caretRetriesLeft_ = 0;
    int modifierOverrideForTest_ = -1;
    RECT lastCaretRect_{};
    ITfComposition* composition_ = nullptr;
};

void TextService::RetryCaret() {
    if (caretRetriesLeft_ <= 0) {
        if (g_caretRetryWindow) KillTimer(g_caretRetryWindow, kCaretRetryTimer);
        return;
    }
    RECT caret{};
    if (ProbeCaretRect(caret)) {
        caretRetriesLeft_ = 0;
        lastCaretRect_ = caret;
        if (g_caretRetryWindow) KillTimer(g_caretRetryWindow, kCaretRetryTimer);
        UpdateCandidates(compositionContext_, lastCandidateResponse_, caret);
        return;
    }
    if (--caretRetriesLeft_ <= 0 && g_caretRetryWindow) KillTimer(g_caretRetryWindow, kCaretRetryTimer);
}

LRESULT CALLBACK CaretRetryWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM) {
    if (message == WM_TIMER && wParam == kCaretRetryTimer) {
        TextService* service = g_activeService;
        if (service) service->RetryCaret();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, 0);
}

bool CreateCaretRetryWindow() {
    if (g_caretRetryWindow && IsWindow(g_caretRetryWindow)) return true;
    static const wchar_t kClassName[] = L"TypeScriptWindowsImeCaretRetry";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = CaretRetryWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        registered = true;
    }
    g_caretRetryWindow = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    return g_caretRetryWindow != nullptr;
}

void DestroyCaretRetryWindow() {
    if (g_caretRetryWindow) {
        KillTimer(g_caretRetryWindow, kCaretRetryTimer);
        DestroyWindow(g_caretRetryWindow);
        g_caretRetryWindow = nullptr;
    }
}

HRESULT KeyEventSink::OnSetFocus(BOOL foreground) {
    LogForegroundContext("OnSetFocus", nullptr);
    NativeLog::Write("OnSetFocus service=%p foreground=%d", service_, foreground ? 1 : 0);
    service_->LogActiveProfile(foreground ? "OnSetFocus-foreground" : "OnSetFocus-background");
    // The Host draws the list on a topmost window of its own, so it does not
    // follow the caret: TSF reports the focus move here and the list has to go
    // while another application owns the text. Coming back re-shows it while
    // the composition this service is still holding is alive.
    if (foreground) service_->ShowCandidatesForComposition();
    else service_->HideCandidates();
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
    case VK_DELETE: return "Delete";
    case VK_ESCAPE: return "Escape";
    case VK_RETURN: return "Enter";
    case VK_SPACE: return " ";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PLUS: return "=";
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT: return "Shift";
    case VK_LEFT: return "ArrowLeft";
    case VK_UP: return "ArrowUp";
    case VK_RIGHT: return "ArrowRight";
    case VK_DOWN: return "ArrowDown";
    default:
        if (vk >= '1' && vk <= '9') return std::string(1, static_cast<char>(vk));
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
int TextService::Modifiers() const {
    if (modifierOverrideForTest_ >= 0) return modifierOverrideForTest_;
    // TSF may invoke the key sink on a thread whose queued-key state does not
    // reliably reflect the modifier held by the user. Prefer either view of
    // the state so Shift+Space is not forwarded as a plain Space key.
    const auto isDown = [](int vk) {
        return (GetKeyState(vk) & 0x8000) != 0 || (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
    int modifiers = 0;
    if (isDown(VK_SHIFT)) modifiers |= 1;
    if (isDown(VK_CONTROL)) modifiers |= 2;
    if (isDown(VK_MENU)) modifiers |= 4;
    if (isDown(VK_LWIN) || isDown(VK_RWIN)) modifiers |= 8;
    return modifiers;
}

bool TextService::CallCore(const std::string& type, WPARAM vk, LPARAM lParam, bool& consume,
                           std::string& response) {
    std::lock_guard<std::mutex> callLock(callCoreMutex_);
    consume = false;
    const std::string key = KeyName(vk);
    // Forward unknown keys too: Node.js owns key behavior and can interpret
    // the virtual-key code even when this native adapter has no display name.

    if (!pipe_.IsConnected() && !pipe_.ConnectToServer(g_servicePipeName, 50)) {
        NativeLog::Write("CallCore type=%s key=%s pipe unavailable", type.c_str(), key.c_str());
        return false;
    }

    const unsigned int id = nextRequestId_++;
    const unsigned int scanCode = (static_cast<unsigned int>(lParam) >> 16) & 0xff;
    // Snapshot once so the JSON sent to Node and the diagnostic log cannot disagree.
    const int modifiers = Modifiers();
    const std::string request =
        std::string("{\"id\":") + std::to_string(id) +
        ",\"session\":" + std::to_string(sessionId_) +
        ",\"type\":\"" + type +
        "\",\"vk\":" + std::to_string(static_cast<unsigned int>(vk)) +
        ",\"scanCode\":" + std::to_string(scanCode) +
        ",\"key\":\"" + JsonEscape(key) +
        "\",\"modifiers\":" + std::to_string(modifiers) + "}";

    NativeLog::Write("CallCore request id=%u session=%llu type=%s vk=0x%02X key=%s modifiers=%d",
        id, sessionId_, type.c_str(), static_cast<unsigned int>(vk), key.c_str(), modifiers);
    if (vk == VK_SPACE) {
        const bool ctrlSpace = (modifiers & 2) != 0 && (modifiers & (4 | 8)) == 0;
        NativeLog::Write("SPACE_DIAGNOSTIC type=%s id=%u session=%llu modifiers=%d ctrl=%d shift=%d alt=%d win=%d classifiedCtrlSpace=%d",
            type.c_str(), id, sessionId_, modifiers, (modifiers & 2) != 0 ? 1 : 0,
            (modifiers & 1) != 0 ? 1 : 0, (modifiers & 4) != 0 ? 1 : 0,
            (modifiers & 8) != 0 ? 1 : 0, ctrlSpace ? 1 : 0);
        if (ctrlSpace) {
            NativeLog::Write("SHORTCUT Ctrl+Space forwarding to Node: type=%s id=%u session=%llu vk=0x%02X key='%s' modifiers=%d",
                type.c_str(), id, sessionId_, static_cast<unsigned int>(vk), key.c_str(), modifiers);
        }
    }
    if (!pipe_.Call(request, response, id)) {
        NativeLog::Write("CallCore response id=%u failed", id);
        pipe_.Disconnect();
        return false;
    }
    consume = JsonBool(response, "consume", false);
    // Log receipt of the Node reply and its mode field without logging the full
    // payload (which may contain user composition text).
    const std::string responseMode = JsonString(response, "mode");
    NativeLog::Write("CallCore NODE_REPLY_RECEIVED id=%u consume=%d bytes=%zu mode='%s'",
        id, consume ? 1 : 0, response.size(), responseMode.empty() ? "<missing>" : responseMode.c_str());
    return true;
}

bool TextService::TestKey(ITfContext* context, WPARAM wParam, LPARAM lParam, bool& consume) {
    (void)context;
    // Forward modifier state to TypeScript; the core owns shortcut policy.
    std::string response;
    return CallCore("testKeyDown", wParam, lParam, consume, response);
}

bool TextService::HandleKey(ITfContext* context, WPARAM wParam, LPARAM lParam, bool& consume) {
    // Shortcut consumption is decided by TypeScript using the forwarded modifiers.
    std::string response;
    if (!CallCore("keyDown", wParam, lParam, consume, response)) return false;
    if (!ApplyInputMode(response)) NativeLog::Write("HandleKey failed to mirror Node input mode");
    if (consume && context) return ApplyResponse(context, response);
    return true;
}

bool TextService::ApplyInputMode(const std::string& response) {
    const std::string mode = JsonString(response, "mode");
    NativeLog::Write("ApplyInputMode NODE_MODE_PARSED mode='%s' responseBytes=%zu threadMgr=%d",
        mode.empty() ? "<missing>" : mode.c_str(), response.size(), threadMgr_ ? 1 : 0);
    if (mode != "chinese" && mode != "english") {
        NativeLog::Write("ApplyInputMode NODE_MODE_IGNORED reason=missing-or-unsupported-mode");
        return true;
    }
    if (!threadMgr_) {
        NativeLog::Write("ApplyInputMode NODE_MODE_RECEIVED_BUT_NOT_APPLIED reason=no-thread-manager mode=%s", mode.c_str());
        return false;
    }
    if (langBarItem_) langBarItem_->SetMode(mode == "chinese");

    ITfCompartmentMgr* compartments = nullptr;
    HRESULT hr = threadMgr_->QueryInterface(IID_ITfCompartmentMgr,
        reinterpret_cast<void**>(&compartments));
    if (FAILED(hr) || !compartments) {
        NativeLog::Write("ApplyInputMode QueryInterface(ITfCompartmentMgr) hr=0x%08lX",
            static_cast<unsigned long>(hr));
        return false;
    }
    ITfCompartment* conversion = nullptr;
    hr = compartments->GetCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &conversion);
    compartments->Release();
    if (FAILED(hr) || !conversion) {
        NativeLog::Write("ApplyInputMode GetCompartment hr=0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    VARIANT value;
    VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = mode == "chinese" ? TF_CONVERSIONMODE_NATIVE : 0;
    hr = conversion->SetValue(clientId_, &value);
    conversion->Release();
    NativeLog::Write("ApplyInputMode mode=%s conversion=0x%08lX hr=0x%08lX",
        mode.c_str(), static_cast<unsigned long>(value.lVal), static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        const std::string confirmed = "{\"id\":0,\"session\":" + std::to_string(sessionId_) +
            ",\"type\":\"modeApplied\",\"mode\":\"" + mode + "\"}";
        if (!pipe_.Notify(confirmed)) NativeLog::Write("ApplyInputMode failed to notify Host of confirmed mode=%s", mode.c_str());
    }
    return SUCCEEDED(hr);
}

bool TextService::ApplyResponse(ITfContext* context, const std::string& response) {
    LogForegroundContext("ApplyResponse", context);
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
        
        HideCandidates();
        return false;
    }

if (hasCommit || composition.empty()) {
        HideCandidates();
        compositionContext_ = nullptr;
        
        if (composition_) {
            composition_->Release();
            composition_ = nullptr;
        }
    } else {
        if (!composition_) composition_ = session.DetachComposition();
        compositionContext_ = context;
        if (!UpdateCandidates(context, response, session.CaretRect())) return false;
    }
    return true;
}

bool TextService::UpdateCandidates(ITfContext* context, const std::string& response, RECT caret) {
    lastCandidateResponse_ = response;
    LogForegroundContext("UpdateCandidates", context);
    const std::vector<std::string> candidates = JsonCandidateTexts(response);
    const UINT selection = JsonUInt(response, "selectedCandidate", 0);
    // Keep the last known anchor: an application can report no layout right
    // after the composition text changed, and the window must stay where it was.
    if (caret.right > caret.left || caret.bottom > caret.top) {
        lastCaretRect_ = caret;
    } else {
        caret = lastCaretRect_;
        if (caret.right <= caret.left || caret.bottom <= caret.top) {
            caretRetriesLeft_ = kCaretRetries;
            if (g_caretRetryWindow) SetTimer(g_caretRetryWindow, kCaretRetryTimer, kCaretRetryDelayMs, nullptr);
        }
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
        std::lock_guard<std::mutex> callLock(callCoreMutex_);
    if (!pipe_.IsConnected() && !pipe_.ConnectToServer(g_servicePipeName, 50)) return false;
    const unsigned int id = nextRequestId_++;
    const HWND foreground = GetForegroundWindow();
    UINT dpi = foreground ? GetDpiForWindow(foreground) : 0;
    if (!dpi) dpi = GetDpiForSystem();
    if (!dpi) dpi = 96;
    std::string request = "{\"id\":" + std::to_string(id) + ",\"session\":" + std::to_string(sessionId_)
        + ",\"type\":\"showCandidates\",\"candidates\":[";
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i) request += ",";
        request += "\"" + JsonEscape(candidates[i]) + "\"";
    }
    request += "],\"selection\":" + std::to_string(selection)
        + ",\"caret\":{\"left\":" + std::to_string(caret.left)
        + ",\"top\":" + std::to_string(caret.top) + ",\"right\":" + std::to_string(caret.right)
        + ",\"bottom\":" + std::to_string(caret.bottom) + "},\"dpi\":" + std::to_string(dpi) + "}";
    if (!pipe_.Notify(request)) NativeLog::Write("UpdateCandidates Notify failed");
    return true;
}

void TextService::HideCandidates() {
    std::lock_guard<std::mutex> callLock(callCoreMutex_);
    if (!pipe_.IsConnected() && !pipe_.ConnectToServer(g_servicePipeName, 50)) return;
    const unsigned int id = nextRequestId_++;
    const std::string request = "{\"id\":" + std::to_string(id) + ",\"session\":" + std::to_string(sessionId_) + ",\"type\":\"hideCandidates\"}";
    pipe_.Notify(request);
}

void TextService::ShowCandidatesForComposition() {
    if (!composition_ || !compositionContext_) return;
    // Re-shows the list the composition is still composing. The anchor is the
    // caret the last keystroke measured; an empty one is re-probed by the caret
    // retry timer. UpdateCandidates takes callCoreMutex_ itself and hides again
    // when the reply it remembers carries no candidates.
    NativeLog::Write("ShowCandidatesForComposition replyBytes=%zu",
        lastCandidateResponse_.size());
    UpdateCandidates(compositionContext_, lastCandidateResponse_, lastCaretRect_);
}

void TextService::OnCompositionTerminated() {
    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    compositionContext_ = nullptr;
    
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
    LogForegroundContext("OnTestKeyDown", context);
    if (!eaten) return E_POINTER;
    // Capture modifier state at callback entry; TSF dispatch timing can differ from physical key state.
    const SHORT ctrl = GetKeyState(VK_CONTROL);
    const SHORT asyncCtrl = GetAsyncKeyState(VK_CONTROL);
    const SHORT leftCtrl = GetKeyState(VK_LCONTROL);
    const SHORT asyncLeftCtrl = GetAsyncKeyState(VK_LCONTROL);
    const SHORT rightCtrl = GetKeyState(VK_RCONTROL);
    const SHORT asyncRightCtrl = GetAsyncKeyState(VK_RCONTROL);
    const SHORT shift = GetKeyState(VK_SHIFT);
    const SHORT alt = GetKeyState(VK_MENU);
    const int modifiersBefore = service_->CurrentModifiers();
    bool consume = false;
    const bool ok = service_->TestKey(context, wParam, lParam, consume);
    *eaten = ok && consume ? TRUE : FALSE;
    const int modifiersAfter = service_->CurrentModifiers();
    NativeLog::Write("OnTestKeyDown context=%p vk=0x%02X key=%s modifiersBefore=%d modifiersAfter=%d ctrl=0x%04X asyncCtrl=0x%04X leftCtrl=0x%04X asyncLeftCtrl=0x%04X rightCtrl=0x%04X asyncRightCtrl=0x%04X shift=0x%04X alt=0x%04X ok=%d consume=%d eaten=%d",
        context, static_cast<unsigned int>(wParam), TextService::DiagnosticKeyName(wParam).c_str(), modifiersBefore, modifiersAfter,
        static_cast<unsigned short>(ctrl), static_cast<unsigned short>(asyncCtrl), static_cast<unsigned short>(leftCtrl),
        static_cast<unsigned short>(asyncLeftCtrl), static_cast<unsigned short>(rightCtrl), static_cast<unsigned short>(asyncRightCtrl),
        static_cast<unsigned short>(shift), static_cast<unsigned short>(alt), ok ? 1 : 0, consume ? 1 : 0, *eaten ? 1 : 0);
    return S_OK;
}

HRESULT KeyEventSink::OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
    if (!eaten) return E_POINTER;
    *eaten = FALSE;
    return S_OK;
}

HRESULT KeyEventSink::OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
    LogForegroundContext("OnKeyDown", context);
    if (!eaten) return E_POINTER;
    // Capture modifier state at callback entry for comparison with test-key dispatch.
    const SHORT ctrl = GetKeyState(VK_CONTROL);
    const SHORT asyncCtrl = GetAsyncKeyState(VK_CONTROL);
    const SHORT leftCtrl = GetKeyState(VK_LCONTROL);
    const SHORT asyncLeftCtrl = GetAsyncKeyState(VK_LCONTROL);
    const SHORT rightCtrl = GetKeyState(VK_RCONTROL);
    const SHORT asyncRightCtrl = GetAsyncKeyState(VK_RCONTROL);
    const SHORT shift = GetKeyState(VK_SHIFT);
    const SHORT alt = GetKeyState(VK_MENU);
    const int modifiersBefore = service_->CurrentModifiers();
    bool consume = false;
    const bool ok = service_->HandleKey(context, wParam, lParam, consume);
    *eaten = ok && consume ? TRUE : FALSE;
    const int modifiersAfter = service_->CurrentModifiers();
    NativeLog::Write("OnKeyDown context=%p vk=0x%02X key=%s modifiersBefore=%d modifiersAfter=%d ctrl=0x%04X asyncCtrl=0x%04X leftCtrl=0x%04X asyncLeftCtrl=0x%04X rightCtrl=0x%04X asyncRightCtrl=0x%04X shift=0x%04X alt=0x%04X ok=%d consume=%d eaten=%d",
        context, static_cast<unsigned int>(wParam), TextService::DiagnosticKeyName(wParam).c_str(), modifiersBefore, modifiersAfter,
        static_cast<unsigned short>(ctrl), static_cast<unsigned short>(asyncCtrl), static_cast<unsigned short>(leftCtrl),
        static_cast<unsigned short>(asyncLeftCtrl), static_cast<unsigned short>(rightCtrl), static_cast<unsigned short>(asyncRightCtrl),
        static_cast<unsigned short>(shift), static_cast<unsigned short>(alt), ok ? 1 : 0, consume ? 1 : 0, *eaten ? 1 : 0);
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

// Every category this TIP registers. GUID_TFCAT_TIP_KEYBOARD makes it a keyboard
// TIP at all; the TIPCAP categories are the capability declarations a host reads
// before offering the TIP to the user. Classic Win32 hosts like Notepad only
// need the keyboard category, but packaged/immersive hosts (Windows Terminal,
// Store apps) and the Win+Space switcher skip a keyboard TIP that does not
// declare these capabilities, which is why switching worked in Notepad only.
const GUID kTipCategories[] = {
    GUID_TFCAT_TIP_KEYBOARD,
    GUID_TFCAT_TIPCAP_COMLESS,
    GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    GUID_TFCAT_TIPCAP_SECUREMODE,
    GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
    GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
};

HRESULT RegisterTipProfile() {
    NativeLog::Write("RegisterTipProfile begin");
    ITfInputProcessorProfiles* profiles = nullptr;
    HRESULT hr = CreateProfiles(&profiles);
    if (FAILED(hr)) return hr;
    hr = profiles->Register(CLSID_TypeScriptWindowsIme);
    NativeLog::Hr("ITfInputProcessorProfiles::Register", hr);
    if (SUCCEEDED(hr)) {
        // Remove legacy profiles from earlier builds; this TIP is zh-CN only.
        profiles->RemoveLanguageProfile(CLSID_TypeScriptWindowsIme, 0x0409, GUID_TypeScriptWindowsImeProfile);
        profiles->RemoveLanguageProfile(CLSID_TypeScriptWindowsIme, 0x0411, GUID_TypeScriptWindowsImeProfile);
        constexpr LANGID langid = 0x0804; // Simplified Chinese (zh-CN)
        hr = profiles->AddLanguageProfile(CLSID_TypeScriptWindowsIme, langid,
            GUID_TypeScriptWindowsImeProfile, kTypeScriptWindowsImeName,
            static_cast<ULONG>(wcslen(kTypeScriptWindowsImeName)), nullptr, 0, 0);
        NativeLog::Write("AddLanguageProfile langid=0x%04X hr=0x%08lX", langid, static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        ITfCategoryMgr* categoryMgr = nullptr;
        hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfCategoryMgr, reinterpret_cast<void**>(&categoryMgr));
        NativeLog::Hr("Create ITfCategoryMgr", hr);
        if (SUCCEEDED(hr)) {
            for (const GUID& catid : kTipCategories) {
                const HRESULT categoryHr = categoryMgr->RegisterCategory(
                    CLSID_TypeScriptWindowsIme, catid, CLSID_TypeScriptWindowsIme);
                if (SUCCEEDED(hr) && FAILED(categoryHr)) hr = categoryHr;
                NativeLog::Write("RegisterCategory cat=%08lX hr=0x%08lX",
                    static_cast<unsigned long>(catid.Data1),
                    static_cast<unsigned long>(categoryHr));
            }
            categoryMgr->Release();
        }
    }
    profiles->Release();
    NativeLog::Hr("RegisterTipProfile result", hr);
    return hr;
}

HRESULT UnregisterTipProfile() {
    NativeLog::Write("UnregisterTipProfile begin");
    // Category metadata is separate from the profile registration, so each
    // category this TIP registered is removed explicitly. An already absent
    // category is not an uninstall failure; only the profile Unregister result
    // decides the outcome.
    ITfCategoryMgr* categoryMgr = nullptr;
    HRESULT categoryResult = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr,
        CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
        reinterpret_cast<void**>(&categoryMgr));
    NativeLog::Hr("Create ITfCategoryMgr", categoryResult);
    if (SUCCEEDED(categoryResult)) {
        for (const GUID& catid : kTipCategories) {
            const HRESULT result = categoryMgr->UnregisterCategory(
                CLSID_TypeScriptWindowsIme, catid, CLSID_TypeScriptWindowsIme);
            NativeLog::Write("UnregisterCategory cat=%08lX hr=0x%08lX",
                static_cast<unsigned long>(catid.Data1),
                static_cast<unsigned long>(result));
        }
        categoryMgr->Release();
    }
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
