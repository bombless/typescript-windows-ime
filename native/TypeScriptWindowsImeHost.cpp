#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include "NativeLog.h"
#include <atomic>
#include <iostream>
#include <string>
#include <thread>

#pragma comment(lib, "advapi32.lib")

namespace {
constexpr DWORD kBufferSize = 16 * 1024;
std::atomic<bool> g_running{true};

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
        return TRUE;
    }
    return FALSE;
}

HANDLE AcceptClient(const wchar_t* name) {
    NativeLog::Write("Host AcceptClient begin pipe=%ls", name);
    while (g_running) {
        HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
            kBufferSize, kBufferSize, 0, LowIntegritySecurity());
        if (pipe == INVALID_HANDLE_VALUE) {
            NativeLog::Win32("Host CreateNamedPipeW");
            std::wcerr << L"CreateNamedPipe failed: " << GetLastError() << std::endl;
            Sleep(500);
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        const DWORD error = GetLastError();
        NativeLog::Write("Host ConnectNamedPipe pipe=%p connected=%d error=%lu", pipe, connected ? 1 : 0, static_cast<unsigned long>(error));
        if (connected || error == ERROR_PIPE_CONNECTED) return pipe;
        CloseHandle(pipe);
    }
    return INVALID_HANDLE_VALUE;
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

void Relay(HANDLE from, HANDLE to) {
    NativeLog::Write("Host Relay begin from=%p to=%p", from, to);
    std::string line;
    while (ReadLine(from, line)) {
        NativeLog::Write("Host Relay frame bytes=%zu", line.size());
        if (!WriteLine(to, line)) break;
    }
    NativeLog::Write("Host Relay ended from=%p to=%p", from, to);
    CancelIoEx(from, nullptr);
    CancelIoEx(to, nullptr);
    DisconnectNamedPipe(from);
    DisconnectNamedPipe(to);
}
}

int wmain() {
    NativeLog::Write("Host startup");
    HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\TypeScriptWindowsImeHost.Singleton");
    if (!instance) {
        NativeLog::Win32("Host CreateMutexW");
        std::wcerr << L"Cannot create Host singleton mutex: " << GetLastError() << std::endl;
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        NativeLog::Write("Host singleton already exists");
        std::wcerr << L"TypeScriptWindowsImeHost is already running. Stop the existing process before starting another instance." << std::endl;
        CloseHandle(instance);
        return 2;
    }
    SetConsoleCtrlHandler(HandleConsole, TRUE);
    constexpr wchar_t kNodePipe[] = L"\\\\.\\pipe\\TypeScriptWindowsIME.Host";
    constexpr wchar_t kTsfPipe[] = L"\\\\.\\pipe\\TypeScriptWindowsIME.Tsf";
    std::wcout << L"TypeScriptWindowsImeHost listening on Node and TSF pipes" << std::endl;
    NativeLog::Write("Host listening node=%ls tsf=%ls", kNodePipe, kTsfPipe);

    while (g_running) {
        HANDLE node = INVALID_HANDLE_VALUE;
        HANDLE tsf = INVALID_HANDLE_VALUE;
        std::thread acceptNode([&] { node = AcceptClient(kNodePipe); });
        std::thread acceptTsf([&] { tsf = AcceptClient(kTsfPipe); });
        acceptNode.join();
        acceptTsf.join();
        if (node == INVALID_HANDLE_VALUE || tsf == INVALID_HANDLE_VALUE) {
            NativeLog::Write("Host accept failed node=%p tsf=%p", node, tsf);
            if (node != INVALID_HANDLE_VALUE) { DisconnectNamedPipe(node); CloseHandle(node); }
            if (tsf != INVALID_HANDLE_VALUE) { DisconnectNamedPipe(tsf); CloseHandle(tsf); }
            continue;
        }

        std::wcout << L"Node and TSF connected; relaying JSONL" << std::endl;
        NativeLog::Write("Host Node and TSF connected node=%p tsf=%p", node, tsf);
        std::thread nodeToTsf(Relay, node, tsf);
        std::thread tsfToNode(Relay, tsf, node);
        nodeToTsf.join();
        tsfToNode.join();
        CloseHandle(node);
        CloseHandle(tsf);
        NativeLog::Write("Host client pair closed");
    }
    ReleaseMutex(instance);
    CloseHandle(instance);
    return 0;
}
