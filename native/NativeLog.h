#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace NativeLog {
inline std::wstring LogPath() {
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
    if (length == 0 || length >= ARRAYSIZE(temp)) return L"TypeScriptWindowsIME.log";
    return std::wstring(temp) + L"TypeScriptWindowsIME.log";
}
inline void Write(const char* fmt, ...) {
    char message[4096]{};
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
    va_end(args);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[4608]{};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u][pid=%lu tid=%lu] %s\r\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond, now.wMilliseconds,
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()), message);
    OutputDebugStringA(line);
    const std::wstring path = LogPath();
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    CloseHandle(file);
}
inline void Hr(const char* operation, HRESULT hr) {
    Write("%s -> hr=0x%08lX", operation, static_cast<unsigned long>(hr));
}
inline void Win32(const char* operation, DWORD error = GetLastError()) {
    Write("%s -> win32=%lu", operation, static_cast<unsigned long>(error));
}
} // namespace NativeLog