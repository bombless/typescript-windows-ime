#include "PipeBridge.h"
#include "NativeLog.h"

#include <string>

namespace {
constexpr DWORD kBufferSize = 16 * 1024;
}

PipeBridge::PipeBridge(DWORD timeoutMs)
    : pipe_(INVALID_HANDLE_VALUE),
      stopEvent_(nullptr),
      listenerReadyEvent_(nullptr),
      acceptStarted_(false),
      timeoutMs_(timeoutMs) {
    NativeLog::Write("PipeBridge ctor this=%p timeoutMs=%lu", this, static_cast<unsigned long>(timeoutMs_));}

PipeBridge::~PipeBridge() {
    NativeLog::Write("PipeBridge dtor this=%p", this);
    if (stopEvent_) {
        SetEvent(stopEvent_);
    }

    Disconnect();

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }

    if (listenerReadyEvent_) {
        CloseHandle(listenerReadyEvent_);
        listenerReadyEvent_ = nullptr;
    }
}

HANDLE PipeBridge::CreatePipeInstance() {
    return CreateNamedPipeW(
        pipeName_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        kBufferSize,
        kBufferSize,
        0,
        nullptr);
}

void PipeBridge::AcceptLoop() {
    for (;;) {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) return;

        HANDLE listener = CreatePipeInstance();
        if (listener == INVALID_HANDLE_VALUE) {
            NativeLog::Win32("PipeBridge::AcceptLoop CreateNamedPipeW");
            Sleep(25);
            continue;
        }

        SetEvent(listenerReadyEvent_);

        HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!connectEvent) {
            CloseHandle(listener);
            Sleep(25);
            continue;
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = connectEvent;

        const BOOL started = ConnectNamedPipe(listener, &overlapped);
        bool connected = started != FALSE;

        if (!connected) {
            const DWORD error = GetLastError();
            NativeLog::Write("PipeBridge::AcceptLoop ConnectNamedPipe started=%d error=%lu", started ? 1 : 0, static_cast<unsigned long>(error));
            if (error == ERROR_PIPE_CONNECTED) {
                SetEvent(connectEvent);
                connected = true;
            } else if (error == ERROR_IO_PENDING) {
                HANDLE waits[] = { stopEvent_, connectEvent };
                const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                NativeLog::Write("PipeBridge::AcceptLoop connect wait=%lu", static_cast<unsigned long>(wait));
                if (wait == WAIT_OBJECT_0) {
                    CancelIoEx(listener, &overlapped);
                    CloseHandle(connectEvent);
                    CloseHandle(listener);
                    return;
                }

                DWORD transferred = 0;
                connected = GetOverlappedResult(listener, &overlapped, &transferred, FALSE) != FALSE;
                NativeLog::Write("PipeBridge::AcceptLoop GetOverlappedResult connected=%d transferred=%lu error=%lu",
                    connected ? 1 : 0, static_cast<unsigned long>(transferred), connected ? 0UL : GetLastError());
            }
        }

        CloseHandle(connectEvent);

        if (!connected) {
            CloseHandle(listener);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // The newest client always wins. The previous active client is
            // deliberately disconnected so it can immediately know it lost.
            if (pipe_ != INVALID_HANDLE_VALUE) {
                CancelIoEx(pipe_, nullptr);
                DisconnectNamedPipe(pipe_);
                CloseHandle(pipe_);
            }
            pipe_ = listener;
        }
        NativeLog::Write("PipeBridge::AcceptLoop active client pipe=%p", listener);

        // Do not wait for the active client here. Immediately create another
        // pipe instance so a newer client can replace it at any time.
    }
}

bool PipeBridge::StartListener(const wchar_t* pipeName) {
    NativeLog::Write("PipeBridge::StartListener this=%p pipe=%ls", this, pipeName ? pipeName : L"<null>");
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!acceptStarted_) {
            pipeName_ = pipeName;

            stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!stopEvent_) return false;

            listenerReadyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!listenerReadyEvent_) {
                CloseHandle(stopEvent_);
                stopEvent_ = nullptr;
                return false;
            }

            acceptStarted_ = true;
            acceptThread_ = std::thread(&PipeBridge::AcceptLoop, this);
        }
    }

    const bool ready = WaitForSingleObject(listenerReadyEvent_, timeoutMs_) == WAIT_OBJECT_0;
    NativeLog::Write("PipeBridge::StartListener ready=%d", ready ? 1 : 0);
    return ready;
}

bool PipeBridge::Connect(const wchar_t* pipeName) {
    if (!StartListener(pipeName)) return false;
    if (IsConnected()) return true;

    // Give the accept loop a short head start. Subsequent calls are cheap and
    // simply observe the currently active client.
    Sleep(timeoutMs_);
    return IsConnected();
}

bool PipeBridge::ConnectToServer(const wchar_t* pipeName, DWORD timeoutMs) {
    NativeLog::Write("PipeBridge::ConnectToServer pipe=%ls timeoutMs=%lu", pipeName, static_cast<unsigned long>(timeoutMs));
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pipe_ != INVALID_HANDLE_VALUE) return true;
        }
        HANDLE candidate = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (candidate != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(candidate, &mode, nullptr, nullptr);
            std::lock_guard<std::mutex> lock(mutex_);
            pipe_ = candidate;
            NativeLog::Write("PipeBridge::ConnectToServer success pipe=%p", candidate);
            return true;
        }
        const DWORD error = GetLastError();
        NativeLog::Write("PipeBridge::ConnectToServer CreateFileW failed error=%lu", static_cast<unsigned long>(error));
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) break;
        WaitNamedPipeW(pipeName, 25);
    }
    NativeLog::Write("PipeBridge::ConnectToServer timeout/failure");
    return false;
}

void PipeBridge::DisconnectLocked() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe_, nullptr);
        DisconnectNamedPipe(pipe_);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

void PipeBridge::Disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    DisconnectLocked();
}

bool PipeBridge::IsConnected() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    DWORD available = 0;
    DWORD total = 0;
    if (PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, &total)) return true;

    const DWORD error = GetLastError();
    if (error == ERROR_BROKEN_PIPE ||
        error == ERROR_NO_DATA ||
        error == ERROR_PIPE_NOT_CONNECTED) {
        DisconnectLocked();
        NativeLog::Write("PipeBridge::IsConnected disconnected error=%lu", static_cast<unsigned long>(error));
    }
    return false;
}

bool PipeBridge::WaitForIo(HANDLE pipe, OVERLAPPED& overlapped, DWORD& transferred) {
    if (WaitForSingleObject(overlapped.hEvent, timeoutMs_) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        return false;
    }
    return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
}

bool PipeBridge::WriteAll(HANDLE pipe, const char* data, DWORD size) {
    while (size > 0) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;

        DWORD written = 0;
        const BOOL started = WriteFile(pipe, data, size, nullptr, &overlapped);
        bool ok = started || GetLastError() == ERROR_IO_PENDING;
        if (ok) ok = WaitForIo(pipe, overlapped, written);

        CloseHandle(overlapped.hEvent);

        if (!ok || written == 0) return false;
        data += written;
        size -= written;
    }
    return true;
}

bool PipeBridge::ReadLine(HANDLE pipe, std::string& line) {
    line.clear();
    char buffer[kBufferSize];

    while (line.size() < kBufferSize) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;

        DWORD read = 0;
        const BOOL started = ReadFile(pipe, buffer, sizeof(buffer), nullptr, &overlapped);
        bool ok = started || GetLastError() == ERROR_IO_PENDING;
        if (ok) ok = WaitForIo(pipe, overlapped, read);

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

bool PipeBridge::Call(
    const std::string& requestJson,
    std::string& responseJson,
    unsigned int requestId) {

    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pipe = pipe_;
    }

    if (pipe == INVALID_HANDLE_VALUE || requestJson.size() > kBufferSize - 1) {
        NativeLog::Write("PipeBridge::Call rejected pipe=%p requestBytes=%zu", pipe, requestJson.size());
        return false;
    }

    const std::string framed = requestJson + "\n";
    if (!WriteAll(pipe, framed.data(), static_cast<DWORD>(framed.size()))) {
        NativeLog::Write("PipeBridge::Call WriteAll failed pipe=%p id=%u", pipe, requestId);
        std::lock_guard<std::mutex> lock(mutex_);
        if (pipe_ == pipe) DisconnectLocked();
        return false;
    }

    std::string response;
    if (!ReadLine(pipe, response)) {
        NativeLog::Write("PipeBridge::Call ReadLine failed pipe=%p id=%u", pipe, requestId);
        std::lock_guard<std::mutex> lock(mutex_);
        if (pipe_ == pipe) DisconnectLocked();
        return false;
    }

    // Compare the id field exactly. A substring search would accept a response
    // for request 1 as the answer to request 10, because "\"id\":1" is a prefix
    // of "\"id\":10".
    const std::string needle = "\"id\":" + std::to_string(requestId);
    const size_t found = response.find(needle);
    const bool exact = found != std::string::npos &&
        (found + needle.size() == response.size() || response[found + needle.size()] == ',' ||
            response[found + needle.size()] == '}');
    if (!exact) {
        NativeLog::Write("PipeBridge::Call response id mismatch expected=%u responseBytes=%zu", requestId, response.size());
        return false;
    }

    responseJson = std::move(response);
    NativeLog::Write("PipeBridge::Call success id=%u responseBytes=%zu", requestId, responseJson.size());
    return true;
}
