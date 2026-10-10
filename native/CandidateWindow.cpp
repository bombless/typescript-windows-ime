#include "CandidateWindow.h"

#include "NativeLog.h"

// windows.h defines min/max macros unless NOMINMAX is set, and this file is
// compiled in a translation unit that already pulled windows.h in.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <mutex>
#include <vector>

namespace CandidateWindow {
namespace {

constexpr wchar_t kWindowClass[] = L"TypeScriptWindowsImeCandidateWindow";
constexpr size_t kMaxVisible = 9;
constexpr UINT_PTR kArrowCursorResource = 32512;

HWND g_window = nullptr;
bool g_classRegistered = false;
std::vector<std::wstring> g_candidates;
UINT g_selection = 0;
UINT g_dpi = 96;
HFONT g_font = nullptr;
int g_rowHeight = 0;
int g_numberWidth = 0;
int g_width = 0;
int g_height = 0;

// UI thread state. Written by the UI thread, read only after g_uiReady is
// signaled; g_uiMutex guards the lazy start so concurrent first calls agree.
HANDLE g_uiThread = nullptr;
DWORD g_uiThreadId = 0;
HWND g_uiSink = nullptr;
HANDLE g_uiReady = nullptr;
bool g_uiUnavailable = false;
std::mutex g_uiMutex;

int Scale(int value) { return MulDiv(value, static_cast<int>(g_dpi), 96); }
size_t Visible(size_t count) { return count < kMaxVisible ? count : kMaxVisible; }
int MinInt(int left, int right) { return left < right ? left : right; }
int MaxInt(int left, int right) { return left > right ? left : right; }
int ClampToWorkArea(int value, int low, int high) { return MinInt(MaxInt(value, low), MaxInt(high, low)); }
bool IsCaretRectEmpty(const RECT& rect);
RECT ApplicationRect();
void Place(HWND window, RECT caret);

HMODULE ModuleHandle() {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleHandle), &module)) {
        return module;
    }
    return GetModuleHandleW(nullptr);
}

bool EnsureFont(UINT dpi) {
    if (g_font && g_dpi == dpi) return true;
    if (g_font) {
        DeleteObject(g_font);
        g_font = nullptr;
    }
    g_dpi = dpi;
    LOGFONTW metrics{};
    metrics.lfHeight = -Scale(16);
    metrics.lfWeight = FW_NORMAL;
    metrics.lfCharSet = DEFAULT_CHARSET;
    metrics.lfQuality = CLEARTYPE_QUALITY;
    metrics.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy_s(metrics.lfFaceName, L"Microsoft YaHei UI");
    g_font = CreateFontIndirectW(&metrics);
    if (!g_font) {
        NativeLog::Win32("CandidateWindow CreateFontIndirect");
        return false;
    }
    return true;
}

void Paint(HDC target) {
    RECT client{};
    if (!GetClientRect(g_window, &client)) return;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    // TEMPORARY DIAGNOSTIC: TSWIME_PAINT_STAGE selects how much of the list is
    // drawn so the harness can find which step makes the window disappear.
    //   1 = solid colour with its own DC, 2 = the bitmap path up to the background,
    //   3 = background plus the selected row, 4 = plus the border,
    //   5 = the real list.
    const int stage = [] {
        wchar_t buffer[8]{};
        const DWORD length = GetEnvironmentVariableW(L"TSWIME_PAINT_STAGE", buffer,
            ARRAYSIZE(buffer));
        return length ? _wtoi(buffer) : 5;
    }();
    NativeLog::Write("CandidateWindow Paint client=%dx%d rows=%zu rowHeight=%d numberWidth=%d font=%p dpi=%u stage=%d",
        width, height, g_candidates.size(), g_rowHeight, g_numberWidth, g_font, g_dpi, stage);
    if (width <= 0 || height <= 0) return;

    // TEMPORARY VISIBILITY TEST: enabled by default until desktop presentation
    // is confirmed. Set TSWIME_RED_SCREEN_TEST=0 to restore normal list painting.
    wchar_t redTestValue[8]{};
    const DWORD redTestLength = GetEnvironmentVariableW(L"TSWIME_RED_SCREEN_TEST",
        redTestValue, ARRAYSIZE(redTestValue));
    const bool redTest = redTestLength == 0 || _wtoi(redTestValue) != 0;
    if (redTest) {
        const HBRUSH diagnosticRed = CreateSolidBrush(RGB(255, 0, 0));
        FillRect(target, &client, diagnosticRed);
        DeleteObject(diagnosticRed);
        if (g_candidates.empty()) return;

        // In diagnostic mode, divide the full-height red window into candidate
        // rows and scale the font to each row, making the strings unmistakable.
        const int rowHeight = MaxInt(1, height / static_cast<int>(g_candidates.size()));
        LOGFONTW largeMetrics{};
        largeMetrics.lfHeight = -MaxInt(18, static_cast<int>(rowHeight * 0.78));
        largeMetrics.lfWeight = FW_BOLD;
        largeMetrics.lfCharSet = DEFAULT_CHARSET;
        largeMetrics.lfQuality = ANTIALIASED_QUALITY;
        wcscpy_s(largeMetrics.lfFaceName, L"Microsoft YaHei UI");
        HFONT largeFont = CreateFontIndirectW(&largeMetrics);
        if (!largeFont) return;
        HGDIOBJ oldFont = SelectObject(target, largeFont);
        SetBkMode(target, TRANSPARENT);
        SetTextColor(target, RGB(255, 255, 255));
        for (size_t i = 0; i < g_candidates.size(); ++i) {
            RECT row{ Scale(12), static_cast<LONG>(i * rowHeight),
                client.right - Scale(12), static_cast<LONG>((i + 1) * rowHeight) };
            if (static_cast<UINT>(i) == g_selection) {
                const HBRUSH selectedBrush = CreateSolidBrush(RGB(120, 0, 0));
                FillRect(target, &row, selectedBrush);
                DeleteObject(selectedBrush);
            }
            DrawTextW(target, g_candidates[i].c_str(), -1, &row,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        SelectObject(target, oldFont);
        DeleteObject(largeFont);
        return;
    }
    if (!g_font) return;

    if (stage == 1) {
        HDC solidMemory = CreateCompatibleDC(target);
        HBITMAP solidBitmap = CreateCompatibleBitmap(target, width, height);
        HGDIOBJ oldSolid = SelectObject(solidMemory, solidBitmap);
        const HBRUSH solidBrush = CreateSolidBrush(RGB(0, 120, 215));
        RECT solid{ 0, 0, width, height };
        FillRect(solidMemory, &solid, solidBrush);
        DeleteObject(solidBrush);
        BitBlt(target, 0, 0, width, height, solidMemory, 0, 0, SRCCOPY);
        SelectObject(solidMemory, oldSolid);
        DeleteObject(solidBitmap);
        DeleteDC(solidMemory);
        return;
    }

    HDC memory = CreateCompatibleDC(target);
    if (!memory) {
        NativeLog::Win32("CandidateWindow CreateCompatibleDC");
        return;
    }
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    if (!bitmap) {
        NativeLog::Win32("CandidateWindow CreateCompatibleBitmap");
        DeleteDC(memory);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    // TEMPORARY DIAGNOSTIC: TSWIME_NO_FONT skips selecting g_font so the harness
    // can prove whether the font handle is what makes the list disappear.
    const bool skipFont = GetEnvironmentVariableW(L"TSWIME_NO_FONT", nullptr, 0) > 0;
    HGDIOBJ oldFont = g_font;
    if (!skipFont) {
        oldFont = SelectObject(memory, g_font);
        if (!oldFont) {
            NativeLog::Write("CandidateWindow SelectObject font failed error=%lu font=%p",
                GetLastError(), reinterpret_cast<void*>(g_font));
            oldFont = SelectObject(memory, GetStockObject(DEFAULT_GUI_FONT));
        }
    }
    SetBkMode(memory, TRANSPARENT);

    if (stage == 2) {
        BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
        if (!skipFont) SelectObject(memory, oldFont);
        SelectObject(memory, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        return;
    }

    const HBRUSH background = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(memory, &client, background);
    DeleteObject(background);

    const int textLeft = Scale(6) + g_numberWidth + Scale(6);
    for (size_t i = 0; i < g_candidates.size(); ++i) {
        const LONG top = static_cast<LONG>(i * g_rowHeight);
        const RECT row{ 0, top, client.right, top + g_rowHeight };
        const bool selected = static_cast<UINT>(i) == g_selection;
        if (selected) {
            const HBRUSH highlight = CreateSolidBrush(RGB(0, 120, 215));
            FillRect(memory, &row, highlight);
            DeleteObject(highlight);
        }

        if (stage >= 5) {
            wchar_t number[8]{};
            _snwprintf_s(number, _TRUNCATE, L"%u", static_cast<unsigned>(i + 1));
            SetTextColor(memory, selected ? RGB(255, 255, 255) : RGB(140, 140, 140));
            RECT numberRect{ Scale(6), row.top, textLeft, row.bottom };
            DrawTextW(memory, number, -1, &numberRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

            SetTextColor(memory, selected ? RGB(255, 255, 255) : RGB(24, 24, 24));
            RECT textRect{ textLeft, row.top, client.right - Scale(6), row.bottom };
            const int drawn = DrawTextW(memory, g_candidates[i].c_str(), -1, &textRect,
                DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            NativeLog::Write("CandidateWindow DrawText row=%zu chars=%u drawn=%d at=%d,%d box=%dx%d",
                i, static_cast<unsigned>(g_candidates[i].size()), drawn, textRect.left, textRect.top,
                textRect.right - textRect.left, textRect.bottom - textRect.top);
        }
    }

    if (stage >= 4) {
        const HBRUSH border = CreateSolidBrush(RGB(120, 120, 120));
        FrameRect(memory, &client, border);
        DeleteObject(border);
    }

    if (!skipFont) SelectObject(memory, oldFont);
    SelectObject(memory, oldBitmap);
    BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    DeleteObject(bitmap);
    DeleteDC(memory);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        Paint(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        // Keep clicks on the composition text working while the list floats.
        return HTTRANSPARENT;
    case WM_DESTROY:
        if (g_window == window) g_window = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool EnsureClass() {
    if (g_classRegistered) return true;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = ModuleHandle();
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(kArrowCursorResource));
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        NativeLog::Win32("CandidateWindow RegisterClassEx");
        return false;
    }
    g_classRegistered = true;
    return true;
}

HWND EnsureWindow() {
    if (g_window && IsWindow(g_window)) return g_window;
    if (!EnsureClass()) return nullptr;
    HWND owner = GetForegroundWindow();
    // The window is created with no owner on purpose. An owned popup is kept
    // below its owner's topmost siblings, and the owner here is another
    // application's window, so the list would end up underneath that
    // application. It is also created hidden and shown by Show so that the first
    // frame is painted after the size is known.
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kWindowClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, ModuleHandle(), nullptr);
    if (!g_window) {
        NativeLog::Win32("CandidateWindow CreateWindowEx");
        return nullptr;
    }
    NativeLog::Write("CandidateWindow created owner=%p foreground=%p",
        static_cast<void*>(owner), static_cast<void*>(GetForegroundWindow()));
    return g_window;
}

bool IsCaretRectEmpty(const RECT& rect) {
    return rect.right <= rect.left && rect.bottom <= rect.top;
}

RECT ApplicationRect() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return RECT{};
    RECT client{};
    if (!GetClientRect(foreground, &client)) return RECT{};
    POINT origin{ client.left, client.top };
    if (!ClientToScreen(foreground, &origin)) return RECT{};
    const RECT rect{ origin.x, origin.y, origin.x + 1, origin.y + 1 };
    NativeLog::Write("CandidateWindow application fallback %ld,%ld,%ld,%ld",
        rect.left, rect.top, rect.right, rect.bottom);
    return rect;
}

void Place(HWND window, RECT caret) {
    if (!window || !IsWindow(window)) return;
    if (IsCaretRectEmpty(caret)) caret = ApplicationRect();
    RECT workArea{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    const HMONITOR monitor = caret.right > caret.left
        ? MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint(POINT{ caret.left, caret.top }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) workArea = monitorInfo.rcMonitor;

    const int x = ClampToWorkArea(caret.left, workArea.left, workArea.right - g_width);
    const int below = workArea.bottom - caret.bottom;
    const int above = caret.top - workArea.top;
    const int y = (g_height <= below || below >= above)
        ? ClampToWorkArea(caret.bottom, workArea.top, workArea.bottom - g_height)
        : ClampToWorkArea(caret.top - g_height, workArea.top, workArea.bottom - g_height);
    const int width = g_width;
    const int height = g_height;
    if (width <= 0 || height <= 0) return;

    SetWindowPos(window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window, nullptr, FALSE);
    RECT placed{};
    GetWindowRect(window, &placed);
    NativeLog::Write("CandidateWindow placed caret=%ld,%ld,%ld,%ld at=%d,%d size=%dx%d rect=%ld,%ld,%ld,%ld visible=%d",
        caret.left, caret.top, caret.right, caret.bottom, x, y, g_width, g_height,
        placed.left, placed.top, placed.right, placed.bottom, IsWindowVisible(window) ? 1 : 0);
}

bool Measure(int& width, int& height) {
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    HGDIOBJ oldFont = SelectObject(dc, g_font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    int widest = 0;
    for (const std::wstring& candidate : g_candidates) {
        SIZE extent{};
        GetTextExtentPoint32W(dc, candidate.c_str(), static_cast<int>(candidate.size()), &extent);
        widest = MaxInt(widest, static_cast<int>(extent.cx));
    }
    g_numberWidth = Scale(22);
    const int textLeft = Scale(6) + g_numberWidth + Scale(6);
    width = MinInt(textLeft + widest + Scale(8), Scale(360));
    g_rowHeight = metrics.tmHeight + Scale(6);
    height = g_rowHeight * static_cast<int>(g_candidates.size()) + Scale(2);
    SelectObject(dc, oldFont);
    ReleaseDC(nullptr, dc);
    NativeLog::Write("CandidateWindow Measure dpi=%u tmHeight=%d tmAveCharWidth=%d widest=%d rowHeight=%d size=%dx%d",
        g_dpi, static_cast<int>(metrics.tmHeight), static_cast<int>(metrics.tmAveCharWidth), widest,
        g_rowHeight, width, height);
    return width > 0 && height > 0;
}

// The window used to be created on whichever thread called Show first, so every
// later caller had to drive a window owned by another thread. SetWindowPos on a
// cross-thread window is a sent message, and the owning thread here was a TSF
// relay thread blocked in ReadFile with no message pump, so the caller blocked
// forever while holding g_renderMutex. The exit path of a session then took
// g_clientsMutex before g_renderMutex and wedged the Host: accept had already
// created the pipe instance but never reached ConnectNamedPipe, which is why
// clients saw error 231 instead of a connection.
//
// One dedicated thread now owns the window and runs the pump. Show, Hide and
// Destroy post a request to it and wait, so every GDI call and every window
// message happens on that one thread and a relay thread can never block on a
// window it does not own.
constexpr UINT kRequestShow = WM_APP + 1;
constexpr UINT kRequestHide = WM_APP + 2;
constexpr UINT kRequestDestroy = WM_APP + 3;
constexpr wchar_t kSinkClass[] = L"TypeScriptWindowsImeCandidateWindowSink";

// The caller owns the request and always frees it: the UI thread fills in
// `result` and signals `completed` as its last act, so the caller can read the
// result after the wait without ever racing the thread.
struct UiRequest {
    UINT message = 0;
    std::vector<std::wstring> candidates;
    UINT selection = 0;
    RECT caret{};
    UINT dpi = 96;
    bool result = false;
    HANDLE completed = nullptr;
};

// The drawing half: runs on the UI thread only, never on a caller's thread.
void HideOnUiThread();

bool ShowOnUiThread(const std::vector<std::wstring>& candidates, UINT selection, RECT caret, UINT dpi) {
    const size_t visible = Visible(candidates.size());
    if (visible == 0) {
        HideOnUiThread();
        return false;
    }
    HWND window = EnsureWindow();
    if (!window) return false;
    if (dpi == 0) dpi = GetDpiForSystem();
    if (dpi == 0) dpi = 96;
    if (!EnsureFont(dpi)) return false;

    g_candidates.assign(candidates.begin(), candidates.begin() + visible);
    g_selection = selection < g_candidates.size() ? selection : 0;

    if (!Measure(g_width, g_height)) return false;

    // Repaint synchronously: the key handler must not return before the list is
    // on screen, and some hosts keep their message loop busy afterwards.
    Place(window, caret);
    // Place already shows the window through SetWindowPos, but ShowWindow is
    // still needed: a window that has never been shown has no DWM redirection
    // surface, and without one nothing this window paints is ever presented,
    // even though IsWindowVisible and the z-order both look correct.
    ShowWindow(window, SW_SHOWNOACTIVATE);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(window, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME | RDW_ALLCHILDREN);

    NativeLog::Write("CandidateWindow Show count=%u selection=%u size=%dx%d dpi=%u",
        static_cast<unsigned>(g_candidates.size()), g_selection, g_width, g_height, g_dpi);
    return true;
}

void HideOnUiThread() {
    if (g_window && IsWindow(g_window)) {
        NativeLog::Write("CandidateWindow Hide");
        ShowWindow(g_window, SW_HIDE);
    }
    g_candidates.clear();
    g_selection = 0;
}

void DestroyOnUiThread() {
    HideOnUiThread();
    if (g_window && IsWindow(g_window)) DestroyWindow(g_window);
    g_window = nullptr;
    if (g_font) {
        DeleteObject(g_font);
        g_font = nullptr;
    }
    g_rowHeight = 0;
    g_numberWidth = 0;
    g_width = 0;
    g_height = 0;
}

void RunRequest(UiRequest* request) {
    switch (request->message) {
    case kRequestShow:
        request->result = ShowOnUiThread(request->candidates, request->selection, request->caret, request->dpi);
        break;
    case kRequestHide:
        HideOnUiThread();
        request->result = true;
        break;
    case kRequestDestroy:
        DestroyOnUiThread();
        request->result = true;
        break;
    default:
        break;
    }
    SetEvent(request->completed);
}

LRESULT CALLBACK SinkProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kRequestShow || message == kRequestHide || message == kRequestDestroy) {
        UiRequest* request = reinterpret_cast<UiRequest*>(lParam);
        if (request) RunRequest(request);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// The thread outlives every window: DestroyWindow only removes the candidate
// window, the pump keeps running so a later Show can create it again. Messages
// aimed at the candidate window arrive on this same queue and are dispatched to
// WindowProc like any other thread message.
DWORD WINAPI UiThreadMain(LPVOID) {
    MSG seed{};
    PeekMessageW(&seed, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    WNDCLASSEXW sinkClass{};
    sinkClass.cbSize = sizeof(sinkClass);
    sinkClass.lpfnWndProc = SinkProc;
    sinkClass.hInstance = ModuleHandle();
    sinkClass.lpszClassName = kSinkClass;
    if (RegisterClassExW(&sinkClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        NativeLog::Win32("CandidateWindow RegisterClassEx sink");
        SetEvent(g_uiReady);
        return 0;
    }
    g_uiSink = CreateWindowExW(0, kSinkClass, nullptr, 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, ModuleHandle(), nullptr);
    SetEvent(g_uiReady);
    if (!g_uiSink) {
        NativeLog::Win32("CandidateWindow CreateWindowEx sink");
        return 0;
    }

    for (;;) {
        MSG message{};
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) break;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

bool EnsureUiThread() {
    std::lock_guard<std::mutex> lock(g_uiMutex);
    if (g_uiSink) return true;
    if (g_uiUnavailable) return false;
    if (!g_uiReady) {
        g_uiReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_uiReady) {
            g_uiUnavailable = true;
            return false;
        }
    }
    if (!g_uiThread) {
        g_uiThread = CreateThread(nullptr, 0, UiThreadMain, nullptr, 0, &g_uiThreadId);
        if (!g_uiThread) {
            NativeLog::Win32("CandidateWindow CreateThread ui");
            g_uiUnavailable = true;
            CloseHandle(g_uiReady);
            g_uiReady = nullptr;
            return false;
        }
    }
    // The sink window exists before the event is set, so this wait is short. The
    // timeout only guards against a UI thread that can never start; in that case
    // the request is drawn inline instead, which is the previous behaviour.
    if (WaitForSingleObject(g_uiReady, 5000) != WAIT_OBJECT_0) {
        NativeLog::Write("CandidateWindow UI thread did not become ready win32=%lu", GetLastError());
        return false;
    }
    if (!g_uiSink) {
        NativeLog::Write("CandidateWindow UI thread created no sink window");
        return false;
    }
    return true;
}

// Posts one request and waits for it. The wait is unbounded on purpose: the UI
// thread is this process's own thread and its only work is the request itself,
// so it always answers. A timeout would have to free the request while the UI
// thread was still writing into it.
bool PostRequest(UINT message, UiRequest* request) {
    if (!EnsureUiThread()) return false;
    return PostMessageW(g_uiSink, message, 0, reinterpret_cast<LPARAM>(request)) != FALSE;
}

} // namespace

bool Show(const std::vector<std::wstring>& candidates, UINT selection, RECT caret, UINT dpi) {
    UiRequest request;
    request.message = kRequestShow;
    request.candidates = candidates;
    request.selection = selection;
    request.caret = caret;
    request.dpi = dpi;
    request.completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!request.completed) {
        NativeLog::Win32("CandidateWindow CreateEvent request");
        return false;
    }
    const bool posted = PostRequest(kRequestShow, &request);
    if (posted) WaitForSingleObject(request.completed, INFINITE);
    const bool result = request.result;
    CloseHandle(request.completed);
    if (!posted) {
        // No UI thread: draw here so the list still appears, even though a
        // caller holding g_renderMutex could then block as it did before.
        return ShowOnUiThread(candidates, selection, caret, dpi);
    }
    return result;
}

void Hide() {
    UiRequest request;
    request.message = kRequestHide;
    request.completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!request.completed) {
        NativeLog::Win32("CandidateWindow CreateEvent request");
        return;
    }
    const bool posted = PostRequest(kRequestHide, &request);
    if (posted) WaitForSingleObject(request.completed, INFINITE);
    CloseHandle(request.completed);
    if (!posted) HideOnUiThread();
}

void Destroy() {
    UiRequest request;
    request.message = kRequestDestroy;
    request.completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!request.completed) {
        NativeLog::Win32("CandidateWindow CreateEvent request");
        return;
    }
    const bool posted = PostRequest(kRequestDestroy, &request);
    if (posted) WaitForSingleObject(request.completed, INFINITE);
    CloseHandle(request.completed);
    if (!posted) DestroyOnUiThread();
    // The pump thread outlives this window and is reused by the next Show.
}

bool Show(const std::vector<std::wstring>& candidates, UINT selection, RECT caret) {
    UINT dpi = GetDpiForSystem();
    return Show(candidates, selection, caret, dpi ? dpi : 96);
}

bool IsVisible() {
    return g_window && IsWindow(g_window) && IsWindowVisible(g_window);
}

} // namespace CandidateWindow