#include "PipeBridge.h"

#include <string>

namespace { constexpr DWORD kBufferSize = 16 * 1024; }

PipeBridge::PipeBridge(DWORD timeoutMs) : pipe_(INVALID_HANDLE_VALUE), timeoutMs_(timeoutMs) {}
PipeBridge::~PipeBridge() { Disconnect(); }

bool PipeBridge::Connect(const wchar_t* pipeName) {
    Disconnect();
    pipe_ = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr)) {
        Disconnect();
        return false;
    }
    return true;
}

void PipeBridge::Disconnect() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe_, nullptr);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

bool PipeBridge::IsConnected() const { return pipe_ != INVALID_HANDLE_VALUE; }

bool PipeBridge::WaitForIo(OVERLAPPED& overlapped, DWORD& transferred) {
    if (WaitForSingleObject(overlapped.hEvent, timeoutMs_) != WAIT_OBJECT_0) {
        CancelIoEx(pipe_, &overlapped);
        return false;
    }
    return GetOverlappedResult(pipe_, &overlapped, &transferred, FALSE) != FALSE;
}

bool PipeBridge::WriteAll(const char* data, DWORD size) {
    while (size > 0) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        DWORD written = 0;
        BOOL started = WriteFile(pipe_, data, size, nullptr, &overlapped);
        bool ok = started || GetLastError() == ERROR_IO_PENDING;
        if (ok) ok = WaitForIo(overlapped, written);
        CloseHandle(overlapped.hEvent);
        if (!ok || written == 0) return false;
        data += written;
        size -= written;
    }
    return true;
}

bool PipeBridge::ReadLine(std::string& line) {
    line.clear();
    char buffer[kBufferSize];
    while (line.size() < kBufferSize) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        DWORD read = 0;
        BOOL started = ReadFile(pipe_, buffer, sizeof(buffer), nullptr, &overlapped);
        bool ok = started || GetLastError() == ERROR_IO_PENDING;
        if (ok) ok = WaitForIo(overlapped, read);
        CloseHandle(overlapped.hEvent);
        if (!ok || read == 0) return false;
        line.append(buffer, buffer + read);
        const auto newline = line.find('\n');
        if (newline != std::string::npos) {
            line.resize(newline);
            return true;
        }
    }
    return false;
}

bool PipeBridge::Call(const std::string& requestJson, std::string& responseJson, unsigned int requestId) {
    if (!IsConnected() || requestJson.size() > kBufferSize - 1) return false;
    const std::string framed = requestJson + "\n";
    if (!WriteAll(framed.data(), static_cast<DWORD>(framed.size()))) {
        Disconnect();
        return false;
    }
    std::string response;
    if (!ReadLine(response)) {
        Disconnect();
        return false;
    }
    const std::string needle = "\"id\":" + std::to_string(requestId);
    if (response.find(needle) == std::string::npos) return false;
    responseJson = std::move(response);
    return true;
}