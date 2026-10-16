// Regression test for the Host deadlock recorded in PLAN.md 6.1.
//
// The reported chain: the first session that shows the candidate list creates
// the window on its own relay thread; a second session's Show then does
// SetWindowPos across threads, which blocks until the first thread pumps
// messages, and that thread is blocked in ReadFile. The second relay thread
// therefore never returns from Show and keeps g_renderMutex; the next session
// to disconnect takes g_clientsMutex before g_renderMutex and wedges, after
// which the listener can never reach ConnectNamedPipe and every new client is
// answered with ERROR_PIPE_BUSY (231) instead of a connection.
//
// This test reproduces that sequence against a Host started on private pipes
// and asserts that the third session still connects and gets its reply.
#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::wstring PipePath(const wchar_t* base, const std::wstring& suffix) {
    return std::wstring(base) + L"." + suffix;
}

bool Expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << "\n";
    return condition;
}

// Overlapped read so a missing reply fails the test instead of hanging it.
bool ReadLine(HANDLE pipe, std::string& line, DWORD timeoutMs) {
    line.clear();
    char buffer[512];
    for (;;) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        DWORD read = 0;
        const BOOL started = ReadFile(pipe, buffer, sizeof(buffer), nullptr, &overlapped);
        bool ok = started != FALSE;
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            ok = WaitForSingleObject(overlapped.hEvent, timeoutMs) == WAIT_OBJECT_0;
            if (ok) ok = GetOverlappedResult(pipe, &overlapped, &read, FALSE) != FALSE;
        } else if (ok) {
            ok = WaitForSingleObject(overlapped.hEvent, timeoutMs) == WAIT_OBJECT_0;
            if (ok) ok = GetOverlappedResult(pipe, &overlapped, &read, FALSE) != FALSE;
        }
        CancelIoEx(pipe, &overlapped);
        CloseHandle(overlapped.hEvent);
        if (!ok || read == 0) return false;
        for (DWORD i = 0; i < read; ++i) {
            if (buffer[i] == '\n') return true;
            line.push_back(buffer[i]);
        }
        if (line.size() > 16 * 1024) return false;
    }
}

bool WriteLine(HANDLE pipe, const std::string& line) {
    const std::string framed = line + "\n";
    DWORD offset = 0;
    while (offset < framed.size()) {
        // The handle is overlapped, so every operation needs its own event and
        // its own wait: a NULL OVERLAPPED on an async handle lets the write
        // outlive the buffer.
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        DWORD written = 0;
        const char* data = framed.data() + offset;
        const DWORD remaining = static_cast<DWORD>(framed.size()) - offset;
        const BOOL started = WriteFile(pipe, data, remaining, nullptr, &overlapped);
        bool ok = started != FALSE;
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            ok = WaitForSingleObject(overlapped.hEvent, 10000) == WAIT_OBJECT_0;
            if (ok) ok = GetOverlappedResult(pipe, &overlapped, &written, FALSE) != FALSE;
        } else if (ok) {
            ok = WaitForSingleObject(overlapped.hEvent, 10000) == WAIT_OBJECT_0;
            if (ok) ok = GetOverlappedResult(pipe, &overlapped, &written, FALSE) != FALSE;
        }
        CloseHandle(overlapped.hEvent);
        if (!ok || written == 0) return false;
        offset += written;
    }
    return true;
}

// Waits until the named pipe instance is waiting for a client. ERROR_PIPE_BUSY
// means the instance exists but nobody called ConnectNamedPipe yet, which is
// exactly the wedged state this test has to detect.
bool WaitForPipe(const std::wstring& name) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (WaitNamedPipeW(name.c_str(), 100) != FALSE) return true;
        Sleep(25);
    }
    return false;
}

// A wedged listener never calls ConnectNamedPipe, and CreateFileW on a pipe
// instance that exists but is not accepting blocks instead of failing. The
// connect therefore runs on its own thread and gives up after a timeout: a
// blocked connect is reported the same way as an error code, because both mean
// the Host stopped listening.
struct ConnectResult {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD error = 0;
    bool blocked = false;
};

ConnectResult ConnectBounded(const std::wstring& name, DWORD timeoutMs) {
    ConnectResult result;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return result;
    std::thread worker([&]() {
        result.pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        result.error = result.pipe == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        SetEvent(done);
    });
    if (WaitForSingleObject(done, timeoutMs) == WAIT_OBJECT_0) {
        worker.join();
    } else {
        // The thread stays blocked in CreateFileW for the life of this test
        // process; the handle it would return is never used.
        worker.detach();
        result.blocked = true;
    }
    CloseHandle(done);
    return result;
}

std::string Field(const std::string& line, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = line.find(needle);
    if (pos == std::string::npos) return "";
    size_t begin = pos + needle.size(), end = begin;
    while (end < line.size() && line[end] >= '0' && line[end] <= '9') ++end;
    return line.substr(begin, end - begin);
}

// Plays the Node business client: answers every forwarded request so the Host
// has somewhere to route, and the sessions below get real replies.
void FakeNodeClient(const std::wstring& name, const std::atomic<bool>& stop) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 200 && !stop.load(); ++attempt) {
        const ConnectResult result = ConnectBounded(name, 1500);
        pipe = result.pipe;
        if (pipe != INVALID_HANDLE_VALUE) break;
        Sleep(25);
    }
    if (pipe == INVALID_HANDLE_VALUE) return;
    while (!stop.load()) {
        std::string request;
        if (!ReadLine(pipe, request, 500)) continue;
        const std::string reply = "{\"session\":" + Field(request, "session") +
            ",\"id\":" + Field(request, "id") + ",\"consume\":false}";
        if (!WriteLine(pipe, reply)) break;
    }
    CancelIoEx(pipe, nullptr);
    CloseHandle(pipe);
}

std::string ShowCandidatesLine(const char* session, int id, int caretLeft) {
    return std::string("{\"id\":") + std::to_string(id) + ",\"session\":" + session +
        ",\"type\":\"showCandidates\",\"candidates\":[\"a\",\"b\"],\"selection\":0,\"caret\":{\"left\":" +
        std::to_string(caretLeft) + ",\"top\":200,\"right\":" + std::to_string(caretLeft + 8) +
        ",\"bottom\":220},\"dpi\":96}";
}

struct Session {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::string id;
    DWORD connectError = 0;
};

bool Hello(Session& session, int id) {
    const std::string request = "{\"id\":" + std::to_string(id) + ",\"session\":" + session.id +
        ",\"type\":\"hello\",\"protocol\":3}";
    if (!WriteLine(session.pipe, request)) return false;
    std::string reply;
    return ReadLine(session.pipe, reply, 10000) && reply.find("\"consume\"") != std::string::npos;
}

} // namespace

int wmain() {
    const std::wstring suffix = std::to_wstring(GetCurrentProcessId());
    const std::wstring tsfPipe = PipePath(L"\\\\.\\pipe\\TypeScriptWindowsIME.Tsf.Dev", suffix);
    const std::wstring nodePipe = PipePath(L"\\\\.\\pipe\\TypeScriptWindowsIME.Host.Dev", suffix);

    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring host = path;
    const size_t slash = host.find_last_of(L"\\/");
    if (slash != std::wstring::npos) host = host.substr(0, slash + 1);
    host += L"TypeScriptWindowsImeHost.exe";

    std::wstring log = L"%TEMP%\\opencode\\HostDeadlockTest.log";
    for (auto& ch : log) if (ch == L'%') ch = L' ';
    // Expand the temp folder the simple way; the exact path only has to work.
    wchar_t temp[MAX_PATH]{};
    GetEnvironmentVariableW(L"TEMP", temp, ARRAYSIZE(temp));
    log = std::wstring(temp) + L"\\opencode\\HostDeadlockTest-" + suffix + L".log";

    SECURITY_ATTRIBUTES inherit{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    const HANDLE logFile = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstring command = L"\"" + host + L"\"";
    std::vector<wchar_t> commandBuffer(command.begin(), command.end());
    commandBuffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (logFile != INVALID_HANDLE_VALUE) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = logFile;
        startup.hStdError = logFile;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION process{};
    SetEnvironmentVariableW(L"TypeScriptWindowsIMEPipeSuffix", suffix.c_str());
    const BOOL started = CreateProcessW(host.c_str(), commandBuffer.data(), nullptr, nullptr,
        logFile != INVALID_HANDLE_VALUE ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!started) {
        std::cerr << "FAIL: could not start the Host: " << GetLastError() << "\n";
        return EXIT_FAILURE;
    }
    CloseHandle(process.hThread);

    const bool tsfReady = WaitForPipe(tsfPipe);
    const bool nodeReady = WaitForPipe(nodePipe);
    std::cout << "tsfReady=" << tsfReady << " nodeReady=" << nodeReady << "\n" << std::flush;

    std::atomic<bool> stopNode{false};
    std::thread nodeThread;
    if (nodeReady) nodeThread = std::thread(FakeNodeClient, nodePipe, std::ref(stopNode));
    Sleep(200); // Let the fake Node client be accepted before any session speaks.

    Session a;
    a.id = std::to_string(GetCurrentProcessId()) + "1";
    Session b;
    b.id = std::to_string(GetCurrentProcessId()) + "2";
    Session c;
    c.id = std::to_string(GetCurrentProcessId()) + "3";

    const ConnectResult aConnect = tsfReady ? ConnectBounded(tsfPipe, 15000) : ConnectResult{};
    a.pipe = aConnect.pipe;
    a.connectError = aConnect.error;
    const bool aHello = a.pipe != INVALID_HANDLE_VALUE && Hello(a, 1);
    const bool aShow = aHello && WriteLine(a.pipe, ShowCandidatesLine(a.id.c_str(), 2, 200));
    std::cout << "aHello=" << (aHello ? 1 : 0) << " aShow=" << (aShow ? 1 : 0) << std::endl;

    const ConnectResult bConnect = a.pipe != INVALID_HANDLE_VALUE ? ConnectBounded(tsfPipe, 15000) : ConnectResult{};
    b.pipe = bConnect.pipe;
    b.connectError = bConnect.error;
    const bool bHello = b.pipe != INVALID_HANDLE_VALUE && Hello(b, 3);
    // The cross-thread window call that used to block for good. The Host
    // answers nothing to showCandidates, so there is no reply to read here;
    // the proof that Show returned is the third session connecting below.
    const bool bShow = bHello && WriteLine(b.pipe, ShowCandidatesLine(b.id.c_str(), 4, 400));
    std::cout << "bHello=" << (bHello ? 1 : 0) << " bShow=" << (bShow ? 1 : 0) << std::endl;

    // B goes away while the list is up, the way a closing application does.
    if (b.pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(b.pipe);
        b.pipe = INVALID_HANDLE_VALUE;
    }
    // A goes away too, so a relay thread runs the disconnect path that used to
    // nest g_clientsMutex inside g_renderMutex.
    if (a.pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(a.pipe);
        a.pipe = INVALID_HANDLE_VALUE;
    }
    Sleep(500);

    const ConnectResult cConnect = ConnectBounded(tsfPipe, 15000);
    c.pipe = cConnect.pipe;
    c.connectError = cConnect.error;
    const bool cConnected = Expect(c.pipe != INVALID_HANDLE_VALUE,
        "a new TSF session must still be able to connect after two sessions came and went");
    if (c.pipe == INVALID_HANDLE_VALUE) {
        if (cConnect.blocked) {
            std::cerr << "  CreateFileW never returned: an instance exists but the listener is not accepting\n";
        } else {
            std::cerr << "  CreateFileW error " << c.connectError
                      << (cConnect.error == 231 ? " (ERROR_PIPE_BUSY: no instance is accepting)" : "") << "\n";
        }
    }
    const bool cHello = cConnected && Hello(c, 5);
    std::cout << "cConnected=" << (cConnected ? 1 : 0) << " cHello=" << (cHello ? 1 : 0) << std::endl;

    stopNode.store(true);
    if (nodeThread.joinable()) nodeThread.join();
    if (c.pipe != INVALID_HANDLE_VALUE) CloseHandle(c.pipe);

    TerminateProcess(process.hProcess, 0);
    CloseHandle(process.hProcess);
    if (logFile != INVALID_HANDLE_VALUE) CloseHandle(logFile);
    SetEnvironmentVariableW(L"TypeScriptWindowsIMEPipeSuffix", nullptr);

    const bool ok = tsfReady && nodeReady && aHello && aShow && bShow && cConnected && cHello;
    if (!ok) return EXIT_FAILURE;
    std::cout << "HostDeadlockTest: all checks passed\n";
    return EXIT_SUCCESS;
}
