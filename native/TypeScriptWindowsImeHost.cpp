#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include "NativeLog.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace {
constexpr DWORD kBufferSize = 16 * 1024;
constexpr wchar_t kNodePipe[] = L"\\\\.\\pipe\\TypeScriptWindowsIME.Host";
constexpr wchar_t kTsfPipe[] = L"\\\\.\\pipe\\TypeScriptWindowsIME.Tsf";

std::atomic<bool> g_running{true};
std::mutex g_clientsMutex;
std::condition_variable g_clientsChanged;

// The Node business client. Exactly one may be connected: a newer one replaces
// the older, which is told to exit by the broken pipe. Node keeps one IME state
// per session, so a single Node process serves every application.
HANDLE g_nodeClient = INVALID_HANDLE_VALUE;

// Every TSF client is kept. TSF activates one text service per application
// process, so this map routinely holds several entries at once. Routing is by
// the session id inside each JSONL line, never by arrival order: displacing the
// previous client would hand its responses to the wrong application.
std::map<unsigned long long, HANDLE> g_tsfClients;

// Every session thread writes to the one Node pipe, so those writes need a lock
// to stay line-atomic.
std::mutex g_nodeWriteMutex;

// Relay threads stay joinable and are tracked here so shutdown can stop them
// before any pipe handle is closed.
std::vector<std::thread> g_relayThreads;

// The pending ConnectNamedPipe of each listener, so shutdown can cancel it.
// Without this the listeners would keep blocking and the process could not exit.
struct PendingConnect {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
};
PendingConnect g_nodeConnect;
PendingConnect g_tsfConnect;

// Must be called with g_clientsMutex held.
void StartRelayThread(std::thread thread) {
    g_relayThreads.push_back(std::move(thread));
}

BOOL WINAPI HandleConsole(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        g_running = false;
        g_clientsChanged.notify_all();
        return TRUE;
    }
    return FALSE;
}

SECURITY_ATTRIBUTES* LowIntegritySecurity() {
    static PSECURITY_DESCRIPTOR descriptor = []() {
        PSECURITY_DESCRIPTOR created = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &created, nullptr)) {
            NativeLog::Write("ConvertStringSecurityDescriptorToSecurityDescriptorW failed win32=%lu",
                static_cast<unsigned long>(GetLastError()));
            return static_cast<PSECURITY_DESCRIPTOR>(nullptr);
        }
        return created;
    }();
    static SECURITY_ATTRIBUTES attributes = []() {
        SECURITY_ATTRIBUTES initial{};
        initial.nLength = sizeof(SECURITY_ATTRIBUTES);
        initial.bInheritHandle = FALSE;
        initial.lpSecurityDescriptor = descriptor;
        return initial;
    }();
    return &attributes;
}

// Every pipe here is opened FILE_FLAG_OVERLAPPED and every operation waits on
// its own event. A synchronous ReadFile on a named pipe blocks WriteFile on the
// same pipe instance, so a host that reads the Node pipe on one thread while
// another thread answers a request deadlocks on the first keystroke. Overlapped
// operations are queued independently and never stall each other.
bool WaitForOverlapped(HANDLE pipe, OVERLAPPED& overlapped, DWORD& transferred) {
    if (WaitForSingleObject(overlapped.hEvent, INFINITE) != WAIT_OBJECT_0) return false;
    return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
}

bool ReadLine(HANDLE pipe, std::string& line) {
    line.clear();
    char ch;
    for (;;) {
        if (line.size() > kBufferSize) return false;
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;

        DWORD read = 0;
        const BOOL started = ReadFile(pipe, &ch, 1, nullptr, &overlapped);
        bool ok = started != FALSE;
        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WaitForOverlapped(pipe, overlapped, read);
        else if (ok) ok = WaitForOverlapped(pipe, overlapped, read);

        CloseHandle(overlapped.hEvent);
        if (!ok || read == 0) return false;
        if (ch == '\n') return true;
        line.push_back(ch);
    }
}

bool WriteAll(HANDLE pipe, const char* data, DWORD size) {
    while (size > 0) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;

        DWORD written = 0;
        const BOOL started = WriteFile(pipe, data, size, nullptr, &overlapped);
        bool ok = started != FALSE;
        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WaitForOverlapped(pipe, overlapped, written);
        else if (ok) ok = WaitForOverlapped(pipe, overlapped, written);

        CloseHandle(overlapped.hEvent);
        if (!ok || written == 0) return false;
        data += written;
        size -= written;
    }
    return true;
}

bool WriteLine(HANDLE pipe, const std::string& line) {
    const std::string framed = line + '\n';
    return WriteAll(pipe, framed.data(), static_cast<DWORD>(framed.size()));
}

// Minimal reader for the numeric session field. The Host is a router and not a
// JSON parser: it only needs the one number that says which client a line
// belongs to. A line without a usable session id is dropped rather than guessed
// at, so a malformed message can never reach the wrong application.
bool ReadSessionId(const std::string& line, unsigned long long& session) {
    const std::string needle = "\"session\":";
    const size_t pos = line.find(needle);
    if (pos == std::string::npos) return false;
    size_t i = pos + needle.size();
    if (i >= line.size() || line[i] < '0' || line[i] > '9') return false;
    unsigned long long value = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        value = value * 10 + static_cast<unsigned long long>(line[i] - '0');
        if (value > 0xffffffffffffull) return false;
        ++i;
    }
    session = value;
    return true;
}

// A named pipe has a single owner, so a second Host silently breaks the first:
// both processes accept connections and each believes it owns the relay.
bool PipeIsOwned(const wchar_t* name) {
    HANDLE probe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (probe == INVALID_HANDLE_VALUE) return false;
    CloseHandle(probe);
    return true;
}

bool ClaimExclusiveOwnership() {
    for (const wchar_t* name : { kNodePipe, kTsfPipe }) {
        if (!PipeIsOwned(name)) continue;
        std::wcerr << L"Another TypeScriptWindowsImeHost already owns " << name << L"\r\n"
                   << L"Stop it first: Get-Process -Name 'TypeScriptWindowsImeHost*' | Stop-Process -Force"
                   << std::endl;
        NativeLog::Write("Host startup aborted pipe=%ls already owned", name);
        return false;
    }
    return true;
}

// Accepts one client. The connect is overlapped so shutdown can cancel it with
// CancelIoEx; a synchronous ConnectNamedPipe would ignore that and hang.
HANDLE AcceptClient(const wchar_t* name, PendingConnect* pending) {
    while (g_running) {
        HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
            kBufferSize, kBufferSize, 0, LowIntegritySecurity());
        if (pipe == INVALID_HANDLE_VALUE) {
            NativeLog::Win32("Host CreateNamedPipeW");
            Sleep(50);
            continue;
        }

        HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!connectEvent) {
            CloseHandle(pipe);
            Sleep(50);
            continue;
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = connectEvent;

        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            pending->pipe = pipe;
            pending->event = connectEvent;
        }

        DWORD transferred = 0;
        bool connected = false;
        const BOOL started = ConnectNamedPipe(pipe, &overlapped);
        if (started) {
            connected = true;
        } else {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (error == ERROR_IO_PENDING) {
                connected = WaitForOverlapped(pipe, overlapped, transferred);
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            pending->pipe = INVALID_HANDLE_VALUE;
            pending->event = nullptr;
        }
        CloseHandle(connectEvent);

        if (!connected) {
            CloseHandle(pipe);
            if (!g_running) return INVALID_HANDLE_VALUE;
            continue;
        }
        return pipe;
    }
    return INVALID_HANDLE_VALUE;
}

// One thread per TSF client: it reads its own pipe and forwards to Node. The
// session id is learned from the first line, because it lives in the payload and
// not in the accept call. Requests never block each other, so one unresponsive
// application cannot stall the others. This thread owns the accepted handle and
// closes it; the session map only borrows it for writing responses.
void ServeTsfClient(HANDLE pipe) {
    std::string line;
    unsigned long long session = 0;
    bool registered = false;
    while (g_running && ReadLine(pipe, line)) {
        if (!ReadSessionId(line, session)) {
            NativeLog::Write("Host dropped TSF line without a session id bytes=%zu", line.size());
            continue;
        }
        if (!registered) {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            auto existing = g_tsfClients.find(session);
            if (existing != g_tsfClients.end()) {
                // Same process reconnected after a dropped pipe. Retire the
                // stale handle so responses do not go to a dead client.
                NativeLog::Write("Host replacing stale TSF session=%llu old=%p new=%p",
                    session, existing->second, pipe);
                DisconnectNamedPipe(existing->second);
                CloseHandle(existing->second);
            }
            g_tsfClients[session] = pipe;
            registered = true;
            NativeLog::Write("Host TSF session active session=%llu pipe=%p clients=%zu",
                session, pipe, g_tsfClients.size());
        }

        HANDLE node = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            node = g_nodeClient;
        }
        if (node == INVALID_HANDLE_VALUE) {
            NativeLog::Write("Host dropped TSF request session=%llu reason=no Node client", session);
            continue;
        }
        std::lock_guard<std::mutex> lock(g_nodeWriteMutex);
        if (!WriteLine(node, line)) {
            NativeLog::Write("Host failed to forward TSF request session=%llu", session);
            break;
        }
    }

    if (registered) {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        auto current = g_tsfClients.find(session);
        if (current != g_tsfClients.end() && current->second == pipe) {
            g_tsfClients.erase(current);
            NativeLog::Write("Host TSF session closed session=%llu clients=%zu", session, g_tsfClients.size());
        }
    }
    CancelIoEx(pipe, nullptr);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

void ListenForTsfClients() {
    while (g_running) {
        HANDLE client = AcceptClient(kTsfPipe, &g_tsfConnect);
        if (client == INVALID_HANDLE_VALUE) return;
        NativeLog::Write("Host accepted TSF client pipe=%p", client);
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        StartRelayThread(std::thread(&ServeTsfClient, client));
    }
}

// Single reader for Node. Responses are dispatched by session id, so every
// application gets its own answer even though they share one connection.
void DispatchNodeToTsf(HANDLE node) {
    std::string line;
    while (ReadLine(node, line)) {
        unsigned long long session = 0;
        if (!ReadSessionId(line, session)) {
            NativeLog::Write("Host dropped Node line without a session id bytes=%zu", line.size());
            continue;
        }
        HANDLE target = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            auto found = g_tsfClients.find(session);
            if (found != g_tsfClients.end()) target = found->second;
        }
        if (target == INVALID_HANDLE_VALUE) {
            NativeLog::Write("Host dropped Node response session=%llu reason=no such client", session);
            continue;
        }
        if (!WriteLine(target, line)) {
            NativeLog::Write("Host failed to deliver response session=%llu", session);
        }
    }
    NativeLog::Write("Host Node dispatch ended pipe=%p", node);
}

// Node replacement is the one place newest-wins is correct: only one business
// client can hold the IME state, and the displaced process exits on its own once
// it sees the broken pipe.
void ListenForNodeClient() {
    while (g_running) {
        HANDLE client = AcceptClient(kNodePipe, &g_nodeConnect);
        if (client == INVALID_HANDLE_VALUE) return;

        HANDLE previous = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            previous = g_nodeClient;
            g_nodeClient = client;
            StartRelayThread(std::thread(&DispatchNodeToTsf, client));
        }
        NativeLog::Write("Host Node client active pipe=%p previous=%p", client, previous);
        g_clientsChanged.notify_all();
        // The displaced client sees the broken pipe and exits on its own.
        if (previous != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(previous);
            CloseHandle(previous);
        }
    }
}

void CancelPendingConnect(PendingConnect* pending) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        pipe = pending->pipe;
        event = pending->event;
    }
    if (pipe != INVALID_HANDLE_VALUE) CancelIoEx(pipe, nullptr);
    if (event != nullptr) SetEvent(event);
}

} // namespace

int wmain() {
    NativeLog::Write("Host startup");
    SetConsoleCtrlHandler(HandleConsole, TRUE);

    if (!ClaimExclusiveOwnership()) return 1;

    std::wcout << L"TypeScriptWindowsImeHost listening on Node and TSF pipes" << std::endl;
    NativeLog::Write("Host listening node=%ls tsf=%ls", kNodePipe, kTsfPipe);

    std::thread nodeListener(ListenForNodeClient);
    std::thread tsfListener(ListenForTsfClients);

    // Routing looks state up per line, so neither listener replacing a client
    // nor a session appearing needs the main thread to do anything.
    while (g_running) {
        std::unique_lock<std::mutex> lock(g_clientsMutex);
        g_clientsChanged.wait_for(lock, std::chrono::milliseconds(200), [] { return !g_running; });
    }

    g_running = false;
    g_clientsChanged.notify_all();

    // Stop every blocking operation before joining. CancelIoEx aborts the
    // pending overlapped reads and connects; closing a handle while a session
    // thread is still reading it corrupts that thread's I/O instead of stopping
    // it, and each session thread closes its own handle on the way out.
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        for (auto& entry : g_tsfClients) CancelIoEx(entry.second, nullptr);
        if (g_nodeClient != INVALID_HANDLE_VALUE) CancelIoEx(g_nodeClient, nullptr);
    }
    CancelPendingConnect(&g_nodeConnect);
    CancelPendingConnect(&g_tsfConnect);

    nodeListener.join();
    tsfListener.join();

    std::vector<std::thread> relayThreads;
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        relayThreads.swap(g_relayThreads);
    }
    for (std::thread& relay : relayThreads) {
        if (relay.joinable()) relay.join();
    }

    // Every relay thread has returned, so nothing is reading these handles now.
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        g_tsfClients.clear();
        if (g_nodeClient != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(g_nodeClient);
            CloseHandle(g_nodeClient);
            g_nodeClient = INVALID_HANDLE_VALUE;
        }
    }
    NativeLog::Write("Host shutdown complete");
    return 0;
}
