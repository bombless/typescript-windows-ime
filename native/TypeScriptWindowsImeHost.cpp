#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include "NativeLog.h"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "advapi32.lib")

namespace {
constexpr DWORD kBufferSize = 16 * 1024;
std::atomic<bool> g_running{true};
std::mutex g_clientsMutex;
std::condition_variable g_clientsChanged;
HANDLE g_nodeClient = INVALID_HANDLE_VALUE;
HANDLE g_tsfClient = INVALID_HANDLE_VALUE;
unsigned long long g_clientGeneration = 0;

SECURITY_ATTRIBUTES* LowIntegritySecurity() {
    static SECURITY_ATTRIBUTES attributes = []() {
        SECURITY_ATTRIBUTES initial{};
        initial.nLength = sizeof(SECURITY_ATTRIBUTES);
        initial.bInheritHandle = FALSE;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &descriptor, nullptr)) {
            NativeLog::Write("ConvertStringSecurityDescriptorToSecurityDescriptorW failed win32=%lu",
                static_cast<unsigned long>(GetLastError()));
            return initial;
        }
        initial.lpSecurityDescriptor = descriptor;
        return initial;
    }();
    return &attributes;
}

BOOL WINAPI HandleConsole(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        g_running = false;
        g_clientsChanged.notify_all();
        return TRUE;
    }
    return FALSE;
}

HANDLE AcceptClient(const wchar_t* name) {
    while (g_running) {
        HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
            kBufferSize, kBufferSize, 0, LowIntegritySecurity());
        if (pipe == INVALID_HANDLE_VALUE) {
            NativeLog::Win32("Host CreateNamedPipeW");
            Sleep(50);
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        if (connected || error == ERROR_PIPE_CONNECTED) return pipe;
        CloseHandle(pipe);
    }
    return INVALID_HANDLE_VALUE;
}

void ListenForClients(const wchar_t* name, bool isNode) {
    while (g_running) {
        HANDLE client = AcceptClient(name);
        if (client == INVALID_HANDLE_VALUE) return;

        HANDLE previous = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            HANDLE& active = isNode ? g_nodeClient : g_tsfClient;
            previous = active;
            active = client;
            ++g_clientGeneration;
        }
        if (previous != INVALID_HANDLE_VALUE) {
            CancelIoEx(previous, nullptr);
            DisconnectNamedPipe(previous);
            CloseHandle(previous);
        }
        NativeLog::Write("Host newest %ls client active pipe=%p",
            isNode ? L"Node" : L"TSF", client);
        g_clientsChanged.notify_one();
    }
}

bool ReadLine(HANDLE pipe, std::string& line) {
    line.clear();
    char ch;
    DWORD read = 0;
    while (g_running && ReadFile(pipe, &ch, 1, &read, nullptr) && read != 0) {
        if (ch == '\n') return true;
        line.push_back(ch);
        if (line.size() > kBufferSize) return false;
    }
    return false;
}

bool WriteLine(HANDLE pipe, const std::string& line) {
    const std::string framed = line + '\n';
    size_t offset = 0;
    while (offset < framed.size()) {
        DWORD written = 0;
        if (!WriteFile(pipe, framed.data() + offset,
            static_cast<DWORD>(framed.size() - offset), &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}

void Relay(HANDLE from, HANDLE to, std::atomic<DWORD>* otherThreadId) {
    NativeLog::Write("Host Relay begin from=%p to=%p", from, to);
    std::string line;
    while (ReadLine(from, line)) {
        if (!WriteLine(to, line)) break;
    }
    NativeLog::Write("Host Relay ended from=%p to=%p", from, to);
    const DWORD otherId = otherThreadId->load();
    if (otherId != 0) {
        HANDLE otherThread = OpenThread(THREAD_TERMINATE, FALSE, otherId);
        if (otherThread) {
            CancelSynchronousIo(otherThread);
            CloseHandle(otherThread);
        }
    }
}
}

int wmain() {
    NativeLog::Write("Host startup");
    SetConsoleCtrlHandler(HandleConsole, TRUE);
    constexpr wchar_t kNodePipe[] = L"\\\\.\\pipe\\TypeScriptWindowsIME.Host";
    constexpr wchar_t kTsfPipe[] = L"\\\\.\\pipe\\TypeScriptWindowsIME.Tsf";
    std::wcout << L"TypeScriptWindowsImeHost listening on Node and TSF pipes" << std::endl;
    NativeLog::Write("Host listening node=%ls tsf=%ls", kNodePipe, kTsfPipe);

    std::thread nodeListener(ListenForClients, kNodePipe, true);
    std::thread tsfListener(ListenForClients, kTsfPipe, false);
    unsigned long long relayedGeneration = ~0ULL;
    while (g_running) {
        HANDLE node = INVALID_HANDLE_VALUE;
        HANDLE tsf = INVALID_HANDLE_VALUE;
        unsigned long long generation = 0;
        {
            std::unique_lock<std::mutex> lock(g_clientsMutex);
            g_clientsChanged.wait(lock, [&] {
                return !g_running || (g_nodeClient != INVALID_HANDLE_VALUE &&
                    g_tsfClient != INVALID_HANDLE_VALUE && g_clientGeneration != relayedGeneration);
            });
            if (!g_running) break;
            node = g_nodeClient;
            tsf = g_tsfClient;
            generation = g_clientGeneration;
        }
        if (generation == relayedGeneration) continue;
        relayedGeneration = generation;

        // Listener threads own the accepted handles and may replace/close
        // them at any time. Give this relay generation private handle values
        // so a recycled HANDLE value can never redirect stale I/O to a new
        // client's pipe instance.
        HANDLE relayNode = INVALID_HANDLE_VALUE;
        HANDLE relayTsf = INVALID_HANDLE_VALUE;
        if (!DuplicateHandle(GetCurrentProcess(), node, GetCurrentProcess(), &relayNode,
                0, FALSE, DUPLICATE_SAME_ACCESS) ||
            !DuplicateHandle(GetCurrentProcess(), tsf, GetCurrentProcess(), &relayTsf,
                0, FALSE, DUPLICATE_SAME_ACCESS)) {
            if (relayNode != INVALID_HANDLE_VALUE) CloseHandle(relayNode);
            if (relayTsf != INVALID_HANDLE_VALUE) CloseHandle(relayTsf);
            NativeLog::Win32("Host DuplicateHandle relay generation");
            continue;
        }
        NativeLog::Write("Host relay pair node=%p tsf=%p generation=%llu", relayNode, relayTsf, generation);

        std::atomic<DWORD> nodeToTsfId{0};
        std::atomic<DWORD> tsfToNodeId{0};
        std::thread nodeToTsf([&] {
            nodeToTsfId = GetCurrentThreadId();
            Relay(relayNode, relayTsf, &tsfToNodeId);
        });
        std::thread tsfToNode([&] {
            tsfToNodeId = GetCurrentThreadId();
            Relay(relayTsf, relayNode, &nodeToTsfId);
        });
        nodeToTsf.join();
        tsfToNode.join();
        CancelIoEx(relayNode, nullptr);
        CancelIoEx(relayTsf, nullptr);
        CloseHandle(relayNode);
        CloseHandle(relayTsf);
        NativeLog::Write("Host relay pair ended generation=%llu", generation);
    }

    g_running = false;
    g_clientsChanged.notify_all();
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        if (g_nodeClient != INVALID_HANDLE_VALUE) {
            CancelIoEx(g_nodeClient, nullptr);
            CloseHandle(g_nodeClient);
            g_nodeClient = INVALID_HANDLE_VALUE;
        }
        if (g_tsfClient != INVALID_HANDLE_VALUE) {
            CancelIoEx(g_tsfClient, nullptr);
            CloseHandle(g_tsfClient);
            g_tsfClient = INVALID_HANDLE_VALUE;
        }
    }
    // Cancel blocking ConnectNamedPipe calls and join listeners; never leave
    // detached threads accessing process-global state during shutdown.
    if (nodeListener.joinable()) {
        CancelSynchronousIo(nodeListener.native_handle());
        nodeListener.join();
    }
    if (tsfListener.joinable()) {
        CancelSynchronousIo(tsfListener.native_handle());
        tsfListener.join();
    }
    return 0;
}
