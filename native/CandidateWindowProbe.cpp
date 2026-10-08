// Standalone debug harness for CandidateWindow.
//
// The TSF path is hard to observe: the window is created inside somebody else's
// process, and the only evidence is a log line claiming visible=1. This harness
// drives CandidateWindow directly, then verifies the result against the pixels
// that are actually on the desktop. A scenario passes only when the capture
// contains the candidate highlight where the window claims to be.
//
//   CandidateWindowProbe.exe                      run every scenario
//   CandidateWindowProbe.exe --list                list the scenario names
//   CandidateWindowProbe.exe centered --count 5    run one scenario
//   CandidateWindowProbe.exe caret 534 638         show at a logged caret
//   CandidateWindowProbe.exe --dpi-unaware caret X Y
//   CandidateWindowProbe.exe --dpi-unaware logged-caret
//   CandidateWindowProbe.exe unaware-host
//   CandidateWindowProbe.exe style-variants
//   CandidateWindowProbe.exe show-sequence
//   CandidateWindowProbe.exe present-timing
//   CandidateWindowProbe.exe field-report
//   CandidateWindowProbe.exe pixel-provenance
//
// --dpi-unaware makes the thread DPI unaware for the whole run. The composition
// caret that TSF reports is in the host application's coordinate space, so
// running the window in the same space reproduces the real placement. The
// unaware-host scenario does that switch on its own.
//
// Every scenario prints PASS or FAIL, and the desktop capture is written next to
// the executable for manual inspection. Run it on an idle desktop: the harness
// reads the screen, so a full-screen window or a busy machine hides the very
// pixels it is looking for.

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "CandidateWindow.h"

namespace {

struct Capture {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels; // 0x00RRGGBB, top-down
};

int g_failures = 0;

void Say(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fflush(stdout);
}

void Pump(int milliseconds) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(milliseconds);
    MSG message{};
    while (GetTickCount() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(10);
    }
}

Capture CaptureDesktop() {
    Capture capture;
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    capture.x = left;
    capture.y = top;
    capture.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    capture.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = capture.width;
    info.bmiHeader.biHeight = -capture.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    // The desktop DC starts at the primary monitor, not at the virtual screen
    // origin, so a multi-monitor desktop with a monitor above or to the left
    // needs the negative offset. CAPTUREBLT keeps layered windows in the copy.
    BitBlt(memory, 0, 0, capture.width, capture.height, screen, capture.x, capture.y,
        SRCCOPY | CAPTUREBLT);
    capture.pixels.resize(static_cast<size_t>(capture.width) * capture.height * 4);
    std::memcpy(capture.pixels.data(), bits, capture.pixels.size());
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return capture;
}

// A screen capture can lag behind what the compositor has drawn, so every check
// waits for new frames before it believes what it sees.
void WaitForPresentation(int attempts) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        Pump(30);
        CaptureDesktop();
    }
}

bool SaveCapture(const Capture& capture, const wchar_t* path) {
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(capture.pixels.size());
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Say("  cannot write %ls: win32=%lu\n", path, GetLastError());
        return false;
    }
    DWORD written = 0;
    WriteFile(file, &header, sizeof(header), &written, nullptr);
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = capture.width;
    info.biHeight = -capture.height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(capture.pixels.size());
    WriteFile(file, &info, sizeof(info), &written, nullptr);
    WriteFile(file, capture.pixels.data(), static_cast<DWORD>(capture.pixels.size()),
        &written, nullptr);
    CloseHandle(file);
    return true;
}

// Counts pixels close to `reference` inside a screen rectangle. The selection
// highlight (RGB 0,120,215) is painted by the window itself, so finding it in
// the desktop capture proves the list reached the screen.
int CountColor(const Capture& capture, RECT area, COLORREF reference, int tolerance) {
    int found = 0;
    const int left = area.left > capture.x ? area.left : capture.x;
    const int top = area.top > capture.y ? area.top : capture.y;
    const int captureRight = capture.x + capture.width;
    const int captureBottom = capture.y + capture.height;
    const int right = area.right < captureRight ? area.right : captureRight;
    const int bottom = area.bottom < captureBottom ? area.bottom : captureBottom;
    if (right <= left || bottom <= top) return 0;
    for (int y = top; y < bottom; ++y) {
        const size_t row = static_cast<size_t>(y - capture.y) * capture.width;
        for (int x = left; x < right; ++x) {
            const size_t index = (row + static_cast<size_t>(x - capture.x)) * 4;
            const unsigned char* pixel = &capture.pixels[index];
            if (std::abs(static_cast<int>(pixel[2]) - GetRValue(reference)) <= tolerance &&
                std::abs(static_cast<int>(pixel[1]) - GetGValue(reference)) <= tolerance &&
                std::abs(static_cast<int>(pixel[0]) - GetBValue(reference)) <= tolerance) {
                ++found;
            }
        }
    }
    return found;
}

// The selection highlight CandidateWindow paints, RGB(0, 120, 215).
const COLORREF kHighlight = RGB(0, 120, 215);
const int kTolerance = 24;

bool MatchesHighlight(const unsigned char* pixel) {
    constexpr int kHighlightRed = 0;
    constexpr int kHighlightGreen = 120;
    constexpr int kHighlightBlue = 215;
    return std::abs(static_cast<int>(pixel[2]) - kHighlightRed) <= kTolerance &&
        std::abs(static_cast<int>(pixel[1]) - kHighlightGreen) <= kTolerance &&
        std::abs(static_cast<int>(pixel[0]) - kHighlightBlue) <= kTolerance;
}

std::vector<std::wstring> MakeCandidates(size_t count) {
    std::vector<std::wstring> candidates;
    for (size_t i = 0; i < count; ++i) {
        wchar_t text[32]{};
        _snwprintf_s(text, _TRUNCATE, L"\x5019\x9009 %zu", i + 1);
        candidates.push_back(text);
    }
    return candidates;
}

// shcore is not linked; the display DC reports the same scale for the report.
UINT MonitorDpi(HMONITOR monitor) {
    if (!monitor) return 0;
    HDC display = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
    if (!display) return 0;
    const int dpi = GetDeviceCaps(display, LOGPIXELSX);
    DeleteDC(display);
    return dpi > 0 ? static_cast<UINT>(dpi) : 0;
}

RECT PrimaryWorkArea() {
    HMONITOR monitor = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT fallback{ 0, 0, static_cast<LONG>(GetSystemMetrics(SM_CXSCREEN)),
        static_cast<LONG>(GetSystemMetrics(SM_CYSCREEN)) };
    return fallback;
}

HWND FindCandidateWindow() {
    return FindWindowExW(nullptr, nullptr, L"TypeScriptWindowsImeCandidateWindow", nullptr);
}

// The module that owns this code. CandidateWindow passes its own module handle
// to RegisterClassEx and CreateWindowEx as the window instance, so the harness
// has to match it.
HMODULE SelfModule() {
    return GetModuleHandleW(nullptr);
}

DPI_AWARENESS ThreadAwareness() {
    return GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
}

// GetWindowDpiAwarenessContext arrived in Windows 10 1703. It is resolved
// dynamically so the harness still runs on older hosts.
using GetWindowDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(HWND);

DPI_AWARENESS WindowAwareness(HWND window) {
    static GetWindowDpiAwarenessContextFn resolve = [] {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<GetWindowDpiAwarenessContextFn>(
                            GetProcAddress(user32, "GetWindowDpiAwarenessContext"))
                      : nullptr;
    }();
    if (window && resolve) return GetAwarenessFromDpiAwarenessContext(resolve(window));
    return ThreadAwareness();
}

const char* AwarenessName(DPI_AWARENESS awareness) {
    switch (awareness) {
    case DPI_AWARENESS_UNAWARE: return "unaware";
    case DPI_AWARENESS_SYSTEM_AWARE: return "system-aware";
    case DPI_AWARENESS_PER_MONITOR_AWARE: return "per-monitor-aware";
    default: return "per-monitor-aware-v2";
    }
}

bool ReadWindowPixels(HWND window, int& nonWhite, int& highlight, int& sampled);
void SavePixels(const char* name, HWND window, const unsigned char* pixels, int width, int height);

void SayCompositionFacts(HWND window);
bool VerifyOnScreen(const char* label, bool expectVisible);
void RunPresentTiming();

// Shows the list and reports every measurement that could explain a missing
// window: geometry, styles, the covering window, and whether the window's own
// surface holds any of the colours it paints.
void RunFieldReport() {
    RECT work = PrimaryWorkArea();
    const LONG x = work.left + (work.right - work.left) / 2 - 90;
    const LONG y = work.top + (work.bottom - work.top) / 2 - 120;
    Say("showing the list near %ld,%ld for 10 seconds; look at the screen\n", x, y);
    Say("move nothing and do not click while this runs\n");

    CandidateWindow::SetCaretProvider(nullptr);
    CandidateWindow::Show(MakeCandidates(5), 0, RECT{ x, y, x + 8, y + 20 });

    for (int second = 0; second < 10; ++second) {
        Pump(1000);
        HWND window = FindCandidateWindow();
        if (!window) {
            Say("  second %2d  the window is gone\n", second + 1);
            continue;
        }
        RECT rect{};
        GetWindowRect(window, &rect);
        const Capture capture = CaptureDesktop();
        const int highlight = CountColor(capture, rect, kHighlight, kTolerance);
        const int white = CountColor(capture, rect, RGB(255, 255, 255), 6);
        const int grey = CountColor(capture, rect, RGB(120, 120, 120), 24);
        HWND top = WindowFromPoint(POINT{ (rect.left + rect.right) / 2,
            (rect.top + rect.bottom) / 2 });
        wchar_t topClass[128]{};
        if (top) GetClassNameW(top, topClass, ARRAYSIZE(topClass));
        Say("  second %2d  rect=%ld,%ld,%ld,%ld visible=%d highlight=%d white=%d grey=%d "
            "covering=%ls\n",
            second + 1, rect.left, rect.top, rect.right, rect.bottom,
            IsWindowVisible(window) ? 1 : 0, highlight, white, grey, topClass);
    }
    SayCompositionFacts(FindCandidateWindow());
    VerifyOnScreen("field-report", true);
    CandidateWindow::Hide();
}
void SavePixels(const char* name, HWND window, const unsigned char* pixels, int width, int height);
bool MatchesHighlight(const unsigned char* pixel);

// Experiments with the window styles a candidate list can use. Chromium and
// Electron compose into WS_EX_NOREDIRECTIONBITMAP child surfaces, and which
// combination stays above them is not something the documentation answers, so
// each variant is measured on screen instead of reasoned about.
enum class VariantPaint {
    kBrush,      // FillRect straight onto the window DC
    kMemoryBlt,  // paint into a DIB and BitBlt, exactly like CandidateWindow
    kNothing,    // acknowledge the paint but draw nothing
};

enum class VariantHitTest {
    kDefault,     // let DefWindowProc answer, which gives HTCLIENT
    kNowhere,     // what CandidateWindow answers today
    kTransparent, // what CandidateWindow answered before
};

struct StyleVariant {
    const char* name;
    DWORD extendedStyle;
    bool owned;
    bool bornSized;
    VariantPaint paint;
    VariantHitTest hitTest;
};

// Each variant changes one thing at a time until the failing combination is
// found. The paint path is held constant at the CandidateWindow style.
const StyleVariant kVariants[] = {
    { "baseline", WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, false, true,
        VariantPaint::kMemoryBlt, VariantHitTest::kDefault },
    { "cleartype-font", WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, false, true,
        VariantPaint::kMemoryBlt, VariantHitTest::kDefault },
    { "hittest-transparent", WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, false, true,
        VariantPaint::kMemoryBlt, VariantHitTest::kTransparent },
    { "owned-by-foreground", WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, true, true,
        VariantPaint::kMemoryBlt, VariantHitTest::kDefault },
    { "real-candidate", WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, false, true,
        VariantPaint::kMemoryBlt, VariantHitTest::kDefault },
};

constexpr wchar_t kVariantClass[] = L"TypeScriptProbeVariantWindow";

VariantPaint g_variantPaint = VariantPaint::kBrush;
VariantHitTest g_variantHitTest = VariantHitTest::kDefault;
bool g_variantUseFont = false;
UINT_PTR g_variantArrowCursor = 32512;

// A font built the same way CandidateWindow builds its own, used to find out
// whether the scaled ClearType font is what keeps the list off the screen.
HFONT MakeReplicaFont(UINT dpi) {
    LOGFONTW metrics{};
    metrics.lfHeight = -MulDiv(16, static_cast<int>(dpi), 96);
    metrics.lfWeight = FW_NORMAL;
    metrics.lfCharSet = DEFAULT_CHARSET;
    metrics.lfQuality = CLEARTYPE_QUALITY;
    metrics.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy_s(metrics.lfFaceName, L"Microsoft YaHei UI");
    return CreateFontIndirectW(&metrics);
}

LRESULT CALLBACK VariantProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST && g_variantHitTest != VariantHitTest::kDefault) {
        return g_variantHitTest == VariantHitTest::kNowhere ? HTNOWHERE : HTTRANSPARENT;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (g_variantPaint != VariantPaint::kNothing) {
            RECT client{};
            GetClientRect(window, &client);
            const int width = client.right - client.left;
            const int height = client.bottom - client.top;
            if (g_variantPaint == VariantPaint::kBrush) {
                const HBRUSH brush = CreateSolidBrush(kHighlight);
                FillRect(dc, &client, brush);
                DeleteObject(brush);
            } else {
                // CandidateWindow renders into a compatible bitmap first and then
                // blits it, so a window that only looks correct when painted
                // directly would pass a naive test and fail in production.
                HDC memory = CreateCompatibleDC(dc);
                HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
                HGDIOBJ previous = SelectObject(memory, bitmap);
                const HBRUSH brush = CreateSolidBrush(kHighlight);
                RECT all{ 0, 0, width, height };
                FillRect(memory, &all, brush);
                DeleteObject(brush);
                BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
                SelectObject(memory, previous);
                DeleteObject(bitmap);
                DeleteDC(memory);
            }
            if (g_variantUseFont) {
                const UINT dpi = GetDpiForWindow(window);
                HFONT font = MakeReplicaFont(dpi ? dpi : 96);
                HGDIOBJ oldFont = SelectObject(dc, font);
                RECT line{ 8, static_cast<LONG>(height / 2) - 8, width - 8,
                    static_cast<LONG>(height / 2) + 8 };
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(0, 0, 0));
                DrawTextW(dc, L"cand 1", -1, &line, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                SelectObject(dc, oldFont);
                DeleteObject(font);
            }
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wParam, lParam);
}

bool EnsureVariantClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    // The class of CandidateWindow carries CS_HREDRAW and CS_VREDRAW, which are
    // still untested, so the harness registers with the same style.
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &VariantProc;
    windowClass.hInstance = SelfModule();
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(g_variantArrowCursor));
    windowClass.lpszClassName = kVariantClass;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Say("  variant class registration failed win32=%lu\n", GetLastError());
        return false;
    }
    registered = true;
    return true;
}

void SayCoveringWindowDetails(const LONG centerX, const LONG centerY) {
    HWND top = WindowFromPoint(POINT{ centerX, centerY });
    if (!top) {
        Say("  covering        none\n");
        return;
    }
    wchar_t className[128]{};
    wchar_t title[256]{};
    GetClassNameW(top, className, ARRAYSIZE(className));
    GetWindowTextW(top, title, ARRAYSIZE(title));
    DWORD processId = 0;
    GetWindowThreadProcessId(top, &processId);
    wchar_t image[MAX_PATH]{};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process) {
        DWORD size = ARRAYSIZE(image);
        if (!QueryFullProcessImageNameW(process, 0, image, &size)) image[0] = L'\0';
        CloseHandle(process);
    }
    HWND root = GetAncestor(top, GA_ROOT);
    const LONG_PTR exStyle = GetWindowLongPtrW(root, GWL_EXSTYLE);
    DWORD cloaked = 0;
    DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    RECT rootRect{};
    GetWindowRect(root, &rootRect);
    Say("  covering        class=%ls root=%ls pid=%lu image=%ls topmost=%d cloaked=%lu "
        "rootRect=%ld,%ld,%ld,%ld title=%ls\n",
        className, root == top ? L"(same)" : L"(parent)", processId, image,
        (exStyle & WS_EX_TOPMOST) ? 1 : 0, cloaked, rootRect.left, rootRect.top,
        rootRect.right, rootRect.bottom, title);
}

// Reports whether the pixels inside the candidate window belong to the window
// or to whatever is behind it. Paint stages fill the window with a colour the
// desktop is unlikely to use, and the crop next to this output is the proof.
void RunPixelProvenance() {
    RECT work = PrimaryWorkArea();
    const LONG x = work.left + 60;
    const LONG y = work.top + 60;
    const wchar_t* stages[] = { L"1", L"2", L"3", L"4", L"5" };
    const char* names[] = { "solid", "white", "white+selection", "plus-border", "full-list" };

    for (int index = 0; index < 5; ++index) {
        SetEnvironmentVariableW(L"TSWIME_PAINT_STAGE", stages[index]);
        CandidateWindow::SetCaretProvider(nullptr);
        CandidateWindow::Show(MakeCandidates(5), 0, RECT{ x, y, x + 8, y + 20 });
        WaitForPresentation(8);

        HWND window = FindCandidateWindow();
        RECT rect{};
        if (window) GetWindowRect(window, &rect);
        const Capture capture = CaptureDesktop();
        wchar_t name[64]{};
        _snwprintf_s(name, _TRUNCATE, L"candidate-probe-stage-%d.bmp", index + 1);
        RECT crop{ rect.left - 30, rect.top - 30, rect.right + 30, rect.bottom + 30 };
        const int cropWidth = crop.right - crop.left;
        const int cropHeight = crop.bottom - crop.top;
        std::vector<unsigned char> pixels(static_cast<size_t>(cropWidth) * cropHeight * 4, 0);
        for (int py = 0; py < cropHeight; ++py) {
            const int sourceY = crop.top + py;
            if (sourceY < capture.y || sourceY >= capture.y + capture.height) continue;
            for (int px = 0; px < cropWidth; ++px) {
                const int sourceX = crop.left + px;
                if (sourceX < capture.x || sourceX >= capture.x + capture.width) continue;
                const size_t source = (static_cast<size_t>(sourceY - capture.y) * capture.width +
                                       static_cast<size_t>(sourceX - capture.x)) * 4;
                const size_t target = (static_cast<size_t>(py) * cropWidth +
                                       static_cast<size_t>(px)) * 4;
                std::memcpy(&pixels[target], &capture.pixels[source], 4);
            }
        }
        Capture cropCapture{ crop.left, crop.top, cropWidth, cropHeight, std::move(pixels) };
        SaveCapture(cropCapture, name);

        // Count the three colours CandidateWindow can paint, in the window area
        // only. Anything else in there belongs to a window behind it.
        int blue = 0;
        int white = 0;
        int grey = 0;
        int other = 0;
        for (int py = 0; py < rect.bottom - rect.top; ++py) {
            for (int px = 0; px < rect.right - rect.left; ++px) {
                const size_t source = (static_cast<size_t>(rect.top + py - capture.y) *
                                       capture.width +
                                       static_cast<size_t>(rect.left + px - capture.x)) * 4;
                const unsigned char* pixel = &capture.pixels[source];
                if (MatchesHighlight(pixel)) ++blue;
                else if (pixel[0] > 250 && pixel[1] > 250 && pixel[2] > 250) ++white;
                else if (std::abs(pixel[2] - 120) <= 30 && std::abs(pixel[1] - 120) <= 30 &&
                         std::abs(pixel[0] - 120) <= 30) ++grey;
                else ++other;
            }
        }
        Say("  stage %d %-16s rect=%ld,%ld,%ld,%ld blue=%d white=%d grey=%d other=%d -> %ls\n",
            index + 1, names[index], rect.left, rect.top, rect.right, rect.bottom, blue, white,
            grey, other, name);
        CandidateWindow::Hide();
        WaitForPresentation(4);
    }
    SetEnvironmentVariableW(L"TSWIME_PAINT_STAGE", nullptr);
}

// Captures the window repeatedly while pumping messages. DWM presents a new
// frame some time after a window is shown, so a single capture taken right
// after Show can legitimately miss it; only a series that never shows the
// window proves the window is not reaching the screen.
void RunPresentTiming() {
    RECT work = PrimaryWorkArea();
    const LONG x = work.left + (work.right - work.left) / 2 - 80;
    const LONG y = work.top + (work.bottom - work.top) / 2 - 100;

    CandidateWindow::SetCaretProvider(nullptr);
    CandidateWindow::Show(MakeCandidates(5), 0, RECT{ x, y, x + 8, y + 20 });
    HWND window = FindCandidateWindow();
    if (!window) {
        Say("  no candidate window was created\n");
        return;
    }
    RECT rect{};
    GetWindowRect(window, &rect);
    Say("  window rect=%ld,%ld,%ld,%ld\n", rect.left, rect.top, rect.right, rect.bottom);

    // Ground truth: read the window's own surface with GetDC and BitBlt from it.
    // This needs no window proc cooperation, so unlike PrintWindow it works even
    // though CandidateWindow does not implement WM_PRINTCLIENT.
    RECT client{};
    GetClientRect(window, &client);
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    HDC windowDc = GetDC(window);
    HDC memory = CreateCompatibleDC(windowDc);
    HBITMAP copy = CreateCompatibleBitmap(windowDc, clientWidth, clientHeight);
    HGDIOBJ previous = SelectObject(memory, copy);
    BitBlt(memory, 0, 0, clientWidth, clientHeight, windowDc, 0, 0, SRCCOPY);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = clientWidth;
    info.bmiHeader.biHeight = -clientHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP direct = CreateDIBSection(windowDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    int windowHighlight = 0;
    int windowWhite = 0;
    if (direct) {
        HGDIOBJ oldDirect = SelectObject(memory, direct);
        BitBlt(memory, 0, 0, clientWidth, clientHeight, memory, 0, 0, SRCCOPY);
        SelectObject(memory, oldDirect);
        const unsigned char* pixels = static_cast<const unsigned char*>(bits);
        for (int y = 0; y < clientHeight; ++y) {
            for (int x = 0; x < clientWidth; ++x) {
                const size_t index =
                    (static_cast<size_t>(y) * clientWidth + static_cast<size_t>(x)) * 4;
                if (MatchesHighlight(&pixels[index])) ++windowHighlight;
                if (pixels[index] == 0xFF && pixels[index + 1] == 0xFF && pixels[index + 2] == 0xFF) {
                    ++windowWhite;
                }
            }
        }
        SavePixels("own-surface", window, pixels, clientWidth, clientHeight);
        DeleteObject(direct);
    }
    SelectObject(memory, previous);
    DeleteObject(copy);
    DeleteDC(memory);
    ReleaseDC(window, windowDc);
    Say("  ownSurface      %dx%d highlight=%d white=%d\n", clientWidth, clientHeight,
        windowHighlight, windowWhite);

    for (int attempt = 0; attempt < 10; ++attempt) {
        Pump(100);
        const Capture capture = CaptureDesktop();
        const int highlight = CountColor(capture, rect, kHighlight, kTolerance);
        Say("  attempt %2d after %3dms highlight=%d\n", attempt, (attempt + 1) * 100, highlight);
    }
    VerifyOnScreen("present-timing", true);
    CandidateWindow::Hide();
}

// Shows the real list, then repeats the operations an application or the window
// manager would normally perform, reporting after each one whether the pixels
// reached the screen. The first step that works is the missing one.
void RunShowSequence() {
    RECT work = PrimaryWorkArea();
    const LONG x = work.left + (work.right - work.left) / 2 - 80;
    const LONG y = work.top + (work.bottom - work.top) / 2 - 100;
    const RECT area{ x, y + 22, x + 176, y + 22 + 269 };

    CandidateWindow::SetCaretProvider(nullptr);
    CandidateWindow::Show(MakeCandidates(5), 0, RECT{ x, y, x + 8, y + 20 });
    HWND window = FindCandidateWindow();
    if (!window) {
        Say("  no candidate window was created\n");
        return;
    }
    RECT rect{};
    GetWindowRect(window, &rect);
    const RECT scan{ rect.left, rect.top, rect.right, rect.bottom };

    auto Count = [&scan] { return CountColor(CaptureDesktop(), scan, kHighlight, kTolerance); };
    Say("  step 0 after Show                  pixels=%d\n", Count());
    RedrawWindow(window, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME | RDW_ALLCHILDREN);
    Say("  step 1 RedrawWindow+FRAME          pixels=%d\n", Count());
    InvalidateRect(window, nullptr, FALSE);
    UpdateWindow(window);
    Say("  step 2 InvalidateRect+UpdateWindow pixels=%d\n", Count());
    SetWindowPos(window, HWND_TOPMOST, rect.left, rect.top, rect.right - rect.left,
        rect.bottom - rect.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Say("  step 3 SetWindowPos TOPMOST        pixels=%d\n", Count());
    SetWindowPos(window, HWND_TOP, rect.left, rect.top, rect.right - rect.left,
        rect.bottom - rect.top, SWP_NOACTIVATE);
    Say("  step 4 SetWindowPos HWND_TOP       pixels=%d\n", Count());
    ShowWindow(window, SW_SHOWNOACTIVATE);
    Say("  step 5 ShowWindow SW_SHOWNOACTIVATE pixels=%d\n", Count());
    HWND foreground = GetForegroundWindow();
    SetWindowPos(window, HWND_TOPMOST, rect.left, rect.top, rect.right - rect.left,
        rect.bottom - rect.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    WaitForPresentation(8);
    Say("  step 6 final redraw                pixels=%d foregroundUnchanged=%d\n", Count(),
        GetForegroundWindow() == foreground ? 1 : 0);
    VerifyOnScreen("show-sequence", true);
    CandidateWindow::Hide();
    (void)area;
}

void RunStyleVariants() {
    if (!EnsureVariantClass()) return;
    RECT work = PrimaryWorkArea();
    const LONG x = work.left + (work.right - work.left) / 2 - 80;
    const LONG y = work.top + (work.bottom - work.top) / 2 - 100;
    const LONG width = 160;
    const LONG height = 200;

    Say("style variants at %ld,%ld %ldx%d; each reports the window that owns its pixels\n",
        x, y, width, height);
    SayCoveringWindowDetails(x + width / 2, y + height / 2);

    // The real window goes first, at the same coordinates, so the two are
    // directly comparable. If the real one is missing where the baseline shows,
    // the difference is inside CandidateWindow and not in the environment.
    CandidateWindow::SetCaretProvider(nullptr);
    CandidateWindow::Show(MakeCandidates(5), 0, RECT{ x, y - 20, x + 8, y });
    Say("  --- real CandidateWindow at the same spot ---\n");
    VerifyOnScreen("real-at-variant-spot", true);
    CandidateWindow::Hide();
    WaitForPresentation(4);

    for (const StyleVariant& variant : kVariants) {
        HWND owner = variant.owned ? GetForegroundWindow() : nullptr;
        // The real candidate window is measured here too, so both kinds of window
        // are reported from the same run and the same coordinates.
        if (strcmp(variant.name, "real-candidate") == 0) {
            CandidateWindow::Show(MakeCandidates(5), 0, RECT{ x, y, x + 8, y + 20 });
            VerifyOnScreen("real-candidate", true);
            CandidateWindow::Hide();
            WaitForPresentation(4);
            continue;
        }
        const bool bornSized = variant.bornSized;
        g_variantPaint = variant.paint;
        g_variantHitTest = variant.hitTest;
        g_variantUseFont = strcmp(variant.name, "cleartype-font") == 0;
        HWND probe = CreateWindowExW(variant.extendedStyle, kVariantClass, L"", WS_POPUP,
            x, y, bornSized ? width : 0, bornSized ? height : 0, owner, nullptr,
            SelfModule(), nullptr);
        if (!probe) {
            Say("  %-24s CreateWindowEx failed win32=%lu\n", variant.name, GetLastError());
            continue;
        }
        if (!bornSized) {
            SetWindowPos(probe, HWND_TOPMOST, x, y, width, height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(probe, SW_SHOWNOACTIVATE);
        }
        RedrawWindow(probe, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        WaitForPresentation(8);

        const Capture capture = CaptureDesktop();
        const RECT area{ x, y, x + width, y + height };
        const int hits = CountColor(capture, area, kHighlight, kTolerance);
        HWND top = WindowFromPoint(POINT{ x + width / 2, y + height / 2 });
        wchar_t topClass[128]{};
        DWORD topProcess = 0;
        if (top) {
            GetClassNameW(top, topClass, ARRAYSIZE(topClass));
            GetWindowThreadProcessId(top, &topProcess);
        }
        Say("  %-24s visible=%d pixels=%d self=%d covering=%ls pid=%lu %s\n", variant.name,
            IsWindowVisible(probe) ? 1 : 0, hits, top == probe ? 1 : 0, topClass, topProcess,
            hits > 0 ? "VISIBLE" : "hidden");
        DestroyWindow(probe);
        WaitForPresentation(4);
    }
}

// A window can be visible, painted, and topmost and still never reach the screen
// when DWM excludes it from composition. These are the exact facts that decide
// it, so the harness prints them instead of guessing.
void SayCompositionFacts(HWND window) {
    DWORD cloaked = 0;
    HRESULT cloakedResult = E_FAIL;
    if (window) {
        cloakedResult = DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    }
    BOOL compositionEnabled = FALSE;
    const HRESULT compositionResult = DwmIsCompositionEnabled(&compositionEnabled);
    RECT attributes{};
    HRESULT boundsResult = E_FAIL;
    LONG_PTR style = 0;
    if (window) {
        GetWindowRect(window, &attributes);
        boundsResult = DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
            &attributes, sizeof(attributes));
        style = GetWindowLongPtrW(window, GWL_STYLE);
    }
    Say("  composition    enabled=%d hr=0x%08lX cloaked=%lu hr=0x%08lX\n",
        compositionEnabled ? 1 : 0, static_cast<unsigned long>(compositionResult),
        cloaked, static_cast<unsigned long>(cloakedResult));
    Say("  windowStyle    style=0x%llX visibleBit=%d bounds=%ld,%ld,%ld,%ld hr=0x%08lX\n",
        static_cast<unsigned long long>(style), (style & WS_VISIBLE) ? 1 : 0, attributes.left,
        attributes.top, attributes.right, attributes.bottom,
        static_cast<unsigned long>(boundsResult));
}

// Prints the image path of the process that owns a window, so the report names
// the application that is covering the candidate list.
void SayProcessImage(DWORD processId, const char* label) {
    wchar_t image[MAX_PATH]{};
    if (processId) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (process) {
            DWORD size = ARRAYSIZE(image);
            if (!QueryFullProcessImageNameW(process, 0, image, &size)) image[0] = L'\0';
            CloseHandle(process);
        }
    }
    Say("  %-16s pid=%lu image=%ls\n", label, processId, image);
}

// Reports where the window ended up and whether the desktop shows it. Returns
// true when the highlight is visible in the capture.
bool VerifyOnScreen(const char* label, bool expectVisible) {
    Pump(400);
    WaitForPresentation(8);
    HWND window = FindCandidateWindow();
    if (!window) {
        Say("  %-18s no candidate window exists (expectVisible=%d)\n", label, expectVisible ? 1 : 0);
        const bool ok = !expectVisible;
        if (!ok) ++g_failures;
        return ok;
    }

    RECT rect{};
    GetWindowRect(window, &rect);
    const bool visible = IsWindowVisible(window) != 0;
    const bool iconic = IsIconic(window) != 0;
    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    RECT dwm{};
    const HRESULT dwmResult = DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &dwm, sizeof(dwm));
    const bool hasDwm = SUCCEEDED(dwmResult);

    Capture capture = CaptureDesktop();
    int hits = 0;
    if (visible) hits = CountColor(capture, rect, kHighlight, kTolerance);
    int white = 0;
    int grey = 0;
    int paintedNonWhite = 0;
    int paintedHighlight = 0;
    int paintedSampled = 0;
    if (visible) ReadWindowPixels(window, paintedNonWhite, paintedHighlight, paintedSampled);

    Say("  %-18s visible=%d iconic=%d noactivate=%d topmost=%d rect=%ld,%ld,%ld,%ld",
        label, visible ? 1 : 0, iconic ? 1 : 0, (exStyle & WS_EX_NOACTIVATE) ? 1 : 0,
        (exStyle & WS_EX_TOPMOST) ? 1 : 0, rect.left, rect.top, rect.right, rect.bottom);
    if (hasDwm) Say(" dwm=%ld,%ld,%ld,%ld", dwm.left, dwm.top, dwm.right, dwm.bottom);
    Say(" dpi=%u awareness=%s highlightPixels=%d\n", GetDpiForWindow(window),
        AwarenessName(WindowAwareness(window)), hits);
const bool intersects = rect.right > capture.x && rect.bottom > capture.y &&
        rect.left < capture.x + capture.width && rect.top < capture.y + capture.height;
    // The list is white with grey text, so counting only the blue highlight can
    // report "nothing there" for a window that is plainly on screen. Count every
    // colour that the window itself paints: white rows, the grey border and the
    // blue selection.
    white = CountColor(capture, rect, RGB(255, 255, 255), 4);
    grey = CountColor(capture, rect, RGB(120, 120, 120), 24);
    (void)white;
    (void)grey;
    Say("  desktopCapture  rectIntersects=%s desktop=%d,%d,%dx%d highlight=%d white=%d grey=%d\n",
        intersects ? "yes" : "no", capture.x, capture.y, capture.width, capture.height, hits, white,
        grey);
    {
        const LONG sampleX = rect.left + rect.right - rect.left / 2;
        const LONG sampleY = rect.top + rect.bottom - rect.top / 2;
        const int row = static_cast<int>(sampleY - capture.y);
        const int column = static_cast<int>(sampleX - capture.x);
        if (row >= 0 && row < capture.height && column >= 0 && column < capture.width) {
            const size_t index = (static_cast<size_t>(row) * capture.width +
                                  static_cast<size_t>(column)) * 4;
            Say("  desktopSample   at %ld,%ld = %d,%d,%d\n", sampleX, sampleY,
                capture.pixels[index + 2], capture.pixels[index + 1], capture.pixels[index]);
        }
    }

    // Who actually owns the pixels the candidate window claims? A covered window
    // is the difference between "drawn in the wrong place" and "drawn but hidden".
    if (visible) {
        const LONG centerX = (rect.left + rect.right) / 2;
        const LONG centerY = (rect.top + rect.bottom) / 2;
        HWND top = WindowFromPoint(POINT{ centerX, centerY });
        wchar_t className[128]{};
        DWORD topProcess = 0;
        if (top) {
            GetClassNameW(top, className, ARRAYSIZE(className));
            GetWindowThreadProcessId(top, &topProcess);
        }
        HWND root = GetAncestor(top, GA_ROOT);
        wchar_t rootClass[128]{};
        DWORD rootProcess = 0;
        if (root) {
            GetClassNameW(root, rootClass, ARRAYSIZE(rootClass));
            GetWindowThreadProcessId(root, &rootProcess);
        }
        DWORD foregroundProcess = 0;
        HWND foreground = GetForegroundWindow();
        if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
        Say("  topmostAtCentre  hwnd=%p class=%ls root=%p rootClass=%ls self=%d "
            "candidatePid=%lu foregroundPid=%lu\n",
            static_cast<void*>(top), className, static_cast<void*>(root), rootClass,
            top == window ? 1 : 0, GetCurrentProcessId(), foregroundProcess);
SayProcessImage(topProcess, "coversWindow");
    SayProcessImage(rootProcess, "coversRoot");
    SayProcessImage(foregroundProcess, "foreground");
    SayCompositionFacts(window);
        // Walk the top of the z-order from the candidate window upwards: every
        // window above it can cover it. Chromium and Electron paint into child
        // HWNDs that sit above a TOPMOST popup, which is why the covering window
        // is reported here even though the candidate window is topmost itself.
        HWND walk = window;
        for (int index = 0; walk && index < 6; ++index) {
            wchar_t className[128]{};
            GetClassNameW(walk, className, ARRAYSIZE(className));
            const LONG_PTR exStyle = GetWindowLongPtrW(walk, GWL_EXSTYLE);
            DWORD processId = 0;
            GetWindowThreadProcessId(walk, &processId);
            Say("  zorder[%d] hwnd=%p pid=%lu class=%ls topmost=%d layered=%d norc=%d\n",
                index, static_cast<void*>(walk), processId, className,
                (exStyle & WS_EX_TOPMOST) ? 1 : 0, (exStyle & WS_EX_LAYERED) ? 1 : 0,
                (exStyle & WS_EX_NOREDIRECTIONBITMAP) ? 1 : 0);
            walk = GetWindow(walk, GW_HWNDPREV);
        }
    }

    wchar_t path[MAX_PATH]{};
    _snwprintf_s(path, _TRUNCATE, L"candidate-probe-%hs.bmp", label);
    SaveCapture(capture, path);
    Say("  capture -> %ls\n", path);
    // Save a crop around the window as well: the full virtual desktop is large
    // enough that the interesting pixels are hard to find by eye.
    RECT crop{ rect.left - 20, rect.top - 20, rect.right + 20, rect.bottom + 20 };
    const int cropWidth = crop.right - crop.left;
    const int cropHeight = crop.bottom - crop.top;
    if (cropWidth > 0 && cropHeight > 0) {
        std::vector<unsigned char> cropped(static_cast<size_t>(cropWidth) * cropHeight * 4);
        for (int y = 0; y < cropHeight; ++y) {
            const int sourceY = crop.top + y;
            if (sourceY < capture.y || sourceY >= capture.y + capture.height) continue;
            for (int x = 0; x < cropWidth; ++x) {
                const int sourceX = crop.left + x;
                if (sourceX < capture.x || sourceX >= capture.x + capture.width) continue;
                const size_t source = (static_cast<size_t>(sourceY - capture.y) * capture.width +
                                       static_cast<size_t>(sourceX - capture.x)) * 4;
                const size_t target = (static_cast<size_t>(y) * cropWidth +
                                       static_cast<size_t>(x)) * 4;
                std::memcpy(&cropped[target], &capture.pixels[source], 4);
            }
        }
        Capture cropCapture{ crop.left, crop.top, cropWidth, cropHeight, std::move(cropped) };
        wchar_t cropPath[MAX_PATH]{};
        _snwprintf_s(cropPath, _TRUNCATE, L"candidate-probe-%hs-crop.bmp", label);
        SaveCapture(cropCapture, cropPath);
        Say("  crop    -> %ls\n", cropPath);
    }

    // A window can be visible and painted yet show nothing on the desktop, so both
// facts are reported: the desktop capture proves the user sees it, PrintWindow
// proves the window itself produced pixels.
//   The list paints a blue selected row; that colour is the proof it reached
    // the screen. White is never used as evidence because a white window behind
    // the list produces the same pixels.
    const bool painted = paintedNonWhite > 0;
    const bool onScreen = hits > 200;
    Say("  %-18s painted=%s onScreen=%s\n", label, painted ? "yes" : "no", onScreen ? "yes" : "no");
    const bool ok = expectVisible ? (visible && painted && onScreen) : !visible;
    Say("  %-18s %s\n", label, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
    return ok;
}

// The failure the user reported: the composition caret that TSF reports sits in
// the host application's coordinate space. Reproduce that with a caret taken
// straight from the log, both in an aware and an unaware thread.
void ScenarioLoggedCaret(size_t count) {
    Say("  using the caret from the native log: 534,638,552,709\n");
    RECT caret{ 534, 638, 552, 709 };
    CandidateWindow::SetCaretProvider(nullptr);
    CandidateWindow::Show(MakeCandidates(count), 0, caret);
    Pump(400);
    HWND window = FindCandidateWindow();
    RECT rect{};
    if (window) GetWindowRect(window, &rect);
    const int hits = window ? CountColor(CaptureDesktop(), rect, kHighlight, kTolerance) : 0;
    RECT work = PrimaryWorkArea();
    const bool onScreen = hits > 0;
    const bool inside = rect.top >= work.top && rect.bottom <= work.bottom;
    Say("  logged-caret    visible=%d rect=%ld,%ld,%ld,%ld workArea=%ld,%ld,%ld,%ld "
        "highlightPixels=%d insideWorkArea=%s %s\n",
        window && IsWindowVisible(window) != 0 ? 1 : 0, rect.left, rect.top, rect.right, rect.bottom,
        work.left, work.top, work.right, work.bottom, hits, inside ? "yes" : "no",
        onScreen ? "PASS" : "FAIL");
    if (!onScreen) ++g_failures;
    Say("  holding for 3 seconds so the list can be inspected by hand...\n");
    Pump(3000);
    CandidateWindow::Hide();
}

void ScenarioCentered(size_t count) {
    RECT work = PrimaryWorkArea();
    const LONG centerX = work.left + (work.right - work.left) / 2;
    const LONG centerY = work.top + (work.bottom - work.top) / 2;
    RECT caret{ centerX, centerY, centerX + 8, centerY + 18 };
    Say("  caret=%ld,%ld,%ld,%ld\n", caret.left, caret.top, caret.right, caret.bottom);
    CandidateWindow::Show(MakeCandidates(count), 0, caret);
    VerifyOnScreen("centered", true);
    CandidateWindow::Hide();
}

void ScenarioBottomEdge(size_t count) {
    RECT work = PrimaryWorkArea();
    RECT caret{ work.left + 200, work.bottom - 40, work.left + 208, work.bottom - 20 };
    Say("  caret=%ld,%ld,%ld,%ld work=%ld,%ld,%ld,%ld\n", caret.left, caret.top, caret.right,
        caret.bottom, work.left, work.top, work.right, work.bottom);
    CandidateWindow::Show(MakeCandidates(count), 0, caret);
    VerifyOnScreen("bottom-edge", true);
    CandidateWindow::Hide();
}

void ScenarioOffScreen(size_t count) {
    RECT caret{ -600, -400, -592, -380 };
    Say("  caret=%ld,%ld,%ld,%ld is on no display\n", caret.left, caret.top, caret.right, caret.bottom);
    CandidateWindow::Show(MakeCandidates(count), 0, caret);
    VerifyOnScreen("off-screen", true);
    CandidateWindow::Hide();
}

void ScenarioEmpty() {
    CandidateWindow::Show({}, 0, RECT{ 100, 100, 108, 120 });
    VerifyOnScreen("empty-list", false);
}

int CountRowsFromCapture(const Capture& capture) {
    // Only the selected row is blue, so the number of highlight bands tells how
    // many rows of the list actually reached the desktop. Anything else on screen
    // using the same colour would only inflate the count, never hide a failure.
    int rows = 0;
    bool inside = false;
    const int step = capture.width > 2000 ? 4 : 2;
    for (int y = 0; y < capture.height; ++y) {
        int hits = 0;
        const size_t row = static_cast<size_t>(y) * capture.width;
        for (int x = 0; x < capture.width; x += step) {
            if (MatchesHighlight(&capture.pixels[(row + static_cast<size_t>(x)) * 4])) ++hits;
        }
        if (hits < 5) {
        inside = false;
        continue;
    }
        if (!inside) ++rows;
        inside = true;
    }
    return rows;
}

bool g_retryCaretReady = false;
bool RetryCaretProvider(RECT& caret) {
    if (!g_retryCaretReady) return false;
    RECT work = PrimaryWorkArea();
    caret = RECT{ work.left + 400, work.top + 300, work.left + 408, work.top + 320 };
    return true;
}

void ScenarioCaretRetry(size_t count) {
    RECT empty{};
    (void)count;
    g_retryCaretReady = false;
    CandidateWindow::SetCaretProvider(&RetryCaretProvider);
    Say("  caret provider returns nothing yet\n");
    CandidateWindow::Show(MakeCandidates(count), 0, empty);
    VerifyOnScreen("retry-initial", true);
    g_retryCaretReady = true;
    Say("  caret provider now returns a rectangle; waiting for the retry timer\n");
    VerifyOnScreen("retry-moved", true);
    CandidateWindow::SetCaretProvider(nullptr);
    CandidateWindow::Hide();
}

DWORD WINAPI NoPumpThread(LPVOID parameter) {
    const RECT caret = *static_cast<const RECT*>(parameter);
    CandidateWindow::Show(MakeCandidates(4), 0, caret);
    Sleep(1500);
    return 0;
}

void ScenarioNoPump(size_t count) {
    // Some hosts deliver key events on a thread that never pumps messages. The
    // window is created there, so WM_PAINT may never run and the list stays
    // blank. This scenario documents whether that actually happens.
    (void)count;
    RECT work = PrimaryWorkArea();
    RECT caret{ work.left + 600, work.top + 200, work.left + 608, work.top + 220 };
    Say("  caret=%ld,%ld,%ld,%ld\n", caret.left, caret.top, caret.right, caret.bottom);
    HANDLE thread = CreateThread(nullptr, 0, &NoPumpThread, &caret, 0, nullptr);
    if (thread) WaitForSingleObject(thread, 2000);
    if (thread) CloseHandle(thread);
    Pump(200);
    HWND window = FindCandidateWindow();
    RECT rect{};
    if (window) GetWindowRect(window, &rect);
    const int hits = window ? CountColor(CaptureDesktop(), rect, kHighlight, kTolerance) : 0;
    Say("  no-pump         visible=%d rect=%ld,%ld,%ld,%ld highlightPixels=%d %s\n",
        window && IsWindowVisible(window) != 0 ? 1 : 0, rect.left, rect.top, rect.right, rect.bottom,
        hits, hits > 0 ? "PASS" : "FAIL");
    if (hits == 0) ++g_failures;
    CandidateWindow::Hide();
}

DPI_AWARENESS_CONTEXT g_previousAwareness = nullptr;

// A real host hands its composition caret to CandidateWindow in the coordinate
// space of the host window. When the host is DPI unaware those coordinates are
// logical, and a thread that is aware of DPI then places the list in the wrong
// spot. This scenario reproduces it.
void ScenarioUnawareHost() {
    const RECT work = PrimaryWorkArea();
    const LONG logicalX = work.left + (work.right - work.left) / 4;
    const LONG logicalY = work.top + (work.bottom - work.top) / 4;
    const RECT logicalCaret{ logicalX, logicalY, logicalX + 8, logicalY + 20 };

    g_previousAwareness = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    Say("  host reports a logical caret %ld,%ld,%ld,%ld while the thread is DPI unaware\n",
        logicalCaret.left, logicalCaret.top, logicalCaret.right, logicalCaret.bottom);
    CandidateWindow::SetCaretProvider(nullptr);
    CandidateWindow::Show(MakeCandidates(9), 0, logicalCaret);
    Pump(500);

    const Capture capture = CaptureDesktop();
    HWND window = FindCandidateWindow();
    RECT rect{};
    if (window) GetWindowRect(window, &rect);
    const int hits = window ? CountColor(capture, rect, kHighlight, kTolerance) : 0;
    // An unaware thread is virtualized: the coordinates it passes to
    // SetWindowPos are logical and the system scales them by the system DPI.
    const int scale = static_cast<int>(GetDpiForSystem()) / 96;
    const LONG expectedLeft = logicalCaret.left * scale;
    const LONG expectedTop = (logicalCaret.top + 20) * scale + scale * 2;
    const long offsetX = rect.left - expectedLeft;
    const long offsetY = rect.top - expectedTop;
    const bool placed = std::abs(offsetX) <= 8 && std::abs(offsetY) <= 8;
    Say("  unaware-host    rect=%ld,%ld,%ld,%ld scale=%d expectedTopLeft=%ld,%ld offset=%ld,%ld "
        "highlightPixels=%d %s\n",
        rect.left, rect.top, rect.right, rect.bottom, scale, expectedLeft, expectedTop,
        offsetX, offsetY, hits, (hits > 0 && placed) ? "PASS" : "FAIL");
    if (hits == 0 || !placed) ++g_failures;
    SaveCapture(capture, L"candidate-probe-unaware-host.bmp");
    Say("  capture -> candidate-probe-unaware-host.bmp\n");
    CandidateWindow::Hide();
    if (g_previousAwareness) SetThreadDpiAwarenessContext(g_previousAwareness);
}

// Scenario names for --list and the usage text.
const char* const kScenarios[] = {
    "centered", "bottom-edge", "off-screen", "empty-list", "overflow",
    "caret-retry", "no-pump", "logged-caret", "unaware-host", "present-timing",
    "show-sequence", "style-variants", "field-report", "pixel-provenance",
};

void RunOverflow() {
    // 12 candidates must be clipped to 9 rows, stay inside the work area, and
    // reach the desktop with a single highlight band for the selected row.
    CandidateWindow::Show(MakeCandidates(12), 3, RECT{ 300, 200, 308, 220 });
    Pump(400);
    HWND window = FindCandidateWindow();
    RECT rect{};
    if (window) GetWindowRect(window, &rect);
    const Capture capture = CaptureDesktop();
    const int bands = CountRowsFromCapture(capture);
    const RECT work = PrimaryWorkArea();
    const bool onScreen = window && IsWindowVisible(window) != 0 &&
        CountColor(capture, rect, kHighlight, kTolerance) > 0;
    const bool inside = rect.top >= work.top && rect.bottom <= work.bottom &&
        rect.left >= work.left && rect.right <= work.right;
    Say("  candidates=12 bandsOnScreen=%d visible=%d rect=%ld,%ld,%ld,%ld "
        "workArea=%ld,%ld,%ld,%ld insideWorkArea=%s %s\n",
        bands, IsWindowVisible(window) != 0 ? 1 : 0, rect.left, rect.top, rect.right, rect.bottom,
        work.left, work.top, work.right, work.bottom, inside ? "yes" : "no",
        (onScreen && inside && bands == 1) ? "PASS" : "FAIL");
    if (!onScreen || !inside || bands != 1) ++g_failures;
    CandidateWindow::Hide();
}

void RunAll(size_t count) {
    Say("running all scenarios with %zu candidates\n", count);
    ScenarioCentered(count);
    ScenarioBottomEdge(count);
    ScenarioOffScreen(count);
    ScenarioEmpty();
    RunOverflow();
    ScenarioCaretRetry(count);
    ScenarioNoPump(count);
    ScenarioLoggedCaret(count);
    ScenarioUnawareHost();
    RunPresentTiming();
    RunPixelProvenance();
    RunShowSequence();
    RunStyleVariants();
    Say("all scenarios finished; the captures stay next to this executable\n");
}

// Writes the pixels of the candidate window itself so a human can look at what
// the text service drew.
void SavePixels(const char* name, HWND window, const unsigned char* pixels, int width, int height) {
    RECT windowRect{};
    if (window) GetWindowRect(window, &windowRect);
    Capture capture{ windowRect.left, windowRect.top, width, height,
        std::vector<unsigned char>(pixels, pixels + static_cast<size_t>(width) * height * 4) };
    wchar_t path[MAX_PATH]{};
    _snwprintf_s(path, _TRUNCATE, L"candidate-probe-%hs.bmp", name);
    SaveCapture(capture, path);
    Say("  windowBitmap -> %ls (%dx%d)\n", path, width, height);
}

// Reads the window's own pixels with PrintWindow instead of trusting the desktop
// capture. PrintWindow asks the window to paint into a DC we own, so it answers
// "did this window paint anything" without any question of z-order, DPI
// virtualization, or another window covering it.
bool ReadWindowPixels(HWND window, int& nonWhite, int& highlight, int& sampled) {
    nonWhite = 0;
    highlight = 0;
    sampled = 0;
    if (!window || !IsWindowVisible(window)) return false;
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return false;

    HDC windowDc = GetDC(window);
    HDC memory = CreateCompatibleDC(windowDc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP direct = CreateDIBSection(windowDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!direct) {
        DeleteDC(memory);
        ReleaseDC(window, windowDc);
        return false;
    }
    HGDIOBJ previous = SelectObject(memory, direct);
    const BOOL ok = PrintWindow(window, memory, PW_CLIENTONLY);
    GdiFlush();

    const unsigned char* pixels = static_cast<const unsigned char*>(bits);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4;
            const unsigned char* pixel = &pixels[index];
            if (pixel[0] != 0xFF || pixel[1] != 0xFF || pixel[2] != 0xFF) ++nonWhite;
            if (MatchesHighlight(pixel)) ++highlight;
            ++sampled;
        }
    }
    SavePixels("window-paint", window, pixels, width, height);
    Say("  windowPixels   printWindowOk=%d nonWhite=%d highlight=%d sampled=%d\n",
        ok ? 1 : 0, nonWhite, highlight, sampled);
    SelectObject(memory, previous);
    DeleteObject(direct);
    DeleteDC(memory);
    ReleaseDC(window, windowDc);
    return ok != 0;
}

void DumpEnvironment() {
    Say("thread awareness : %s\n", AwarenessName(ThreadAwareness()));
    Say("system dpi       : %u\n", GetDpiForSystem());
    RECT virtualScreen{ static_cast<LONG>(GetSystemMetrics(SM_XVIRTUALSCREEN)),
        static_cast<LONG>(GetSystemMetrics(SM_YVIRTUALSCREEN)),
        static_cast<LONG>(GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN)),
        static_cast<LONG>(GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)) };
    Say("virtual screen   : %ld,%ld,%ld,%ld\n", virtualScreen.left, virtualScreen.top,
        virtualScreen.right, virtualScreen.bottom);
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR handle, HDC, LPRECT, LPARAM) -> BOOL {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(handle, &info)) {
            Say("monitor work area: %ld,%ld,%ld,%ld flags=%lu dpi=%u\n", info.rcWork.left,
                info.rcWork.top, info.rcWork.right, info.rcWork.bottom, info.dwFlags, MonitorDpi(handle));
        }
        return TRUE;
    }, 0);
    RECT work = PrimaryWorkArea();
    Say("primary work area: %ld,%ld,%ld,%ld\n", work.left, work.top, work.right, work.bottom);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetLastError(0);
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        GetLastError() != ERROR_ACCESS_DENIED) {
        Say("SetProcessDpiAwarenessContext failed: %lu\n", GetLastError());
    }

    std::vector<std::wstring> arguments(argv + 1, argv + argc);

    if (!arguments.empty() && arguments[0] == L"--list") {
        for (const char* name : kScenarios) Say("%s\n", name);
        return 0;
    }

    if (!arguments.empty() && arguments[0] == L"--dpi-unaware") {
        // Every coordinate CandidateWindow uses follows the thread context, so
        // switching it here reproduces an unaware host application.
        g_previousAwareness = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
        if (!g_previousAwareness) {
            Say("failed to switch the thread to DPI unaware: %lu\n", GetLastError());
            return 2;
        }
        Say("thread switched to DPI unaware (simulates an unaware host application)\n");
    }

    DumpEnvironment();

    size_t count = 9;
    RECT caret{};
    bool haveCaret = false;
    std::wstring scenario = L"all";
    for (const std::wstring& argument : arguments) {
        const bool isOption = !argument.empty() && argument[0] == L'-';
        if (!isOption && scenario == L"all") scenario = argument;
    }
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] == L"--count" && i + 1 < arguments.size()) {
            count = static_cast<size_t>(_wtoi(arguments[++i].c_str()));
        } else if (arguments[i] == L"caret" && i + 2 < arguments.size()) {
            caret.left = _wtoi(arguments[++i].c_str());
            caret.top = _wtoi(arguments[++i].c_str());
            caret.right = caret.left + 8;
            caret.bottom = caret.top + 20;
            haveCaret = true;
        }
    }
    if (count == 0) count = 1;
    if (count > 64) count = 64;

    if (haveCaret) {
        Say("caret=%ld,%ld,%ld,%ld\n", caret.left, caret.top, caret.right, caret.bottom);
        CandidateWindow::SetCaretProvider(nullptr);
        CandidateWindow::Show(MakeCandidates(count), 0, caret);
        VerifyOnScreen("explicit-caret", true);
        // Leave the list on screen briefly so it can be inspected by hand.
        Say("holding the window for 4 seconds...\n");
        Pump(4000);
        CandidateWindow::Hide();
    } else if (scenario == L"all") {
        RunAll(count);
    } else if (scenario == L"centered") {
        ScenarioCentered(count);
    } else if (scenario == L"bottom-edge") {
        ScenarioBottomEdge(count);
    } else if (scenario == L"off-screen") {
        ScenarioOffScreen(count);
    } else if (scenario == L"empty-list") {
        ScenarioEmpty();
} else if (scenario == L"overflow") {
        RunOverflow();
    } else if (scenario == L"caret-retry") {
        ScenarioCaretRetry(count);
    } else if (scenario == L"no-pump") {
        ScenarioNoPump(count);
    } else if (scenario == L"logged-caret") {
        ScenarioLoggedCaret(count);
    } else if (scenario == L"unaware-host") {
        ScenarioUnawareHost();
    } else if (scenario == L"present-timing") {
        RunPresentTiming();
    } else if (scenario == L"field-report") {
        RunFieldReport();
    } else if (scenario == L"pixel-provenance") {
        RunPixelProvenance();
    } else if (scenario == L"show-sequence") {
        RunShowSequence();
    } else if (scenario == L"style-variants") {
        RunStyleVariants();
    } else {
        Say("unknown scenario '%ls'\n", scenario.c_str());
        Say("known scenarios:");
        for (const char* name : kScenarios) Say("  %s\n", name);
        return 2;
    }

    CandidateWindow::Destroy();
    Say("failures: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}