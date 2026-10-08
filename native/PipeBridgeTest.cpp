#include "PipeBridge.h"

#include <windows.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::wstring MakePipeName(const wchar_t* suffix) {
    return std::wstring(L"\\\\.\\pipe\\TypeScriptWindowsIME_Test_") + std::to_wstring(GetCurrentProcessId()) + L"_" + suffix;
}

bool Expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << "\n";
    return condition;
}

bool ConnectEventually(PipeBridge& bridge, const std::wstring& name) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (bridge.Connect(name.c_str())) return true;
        Sleep(10);
    }
    return false;
}

void StartClient(const std::wstring& name, const std::string& response, bool respond, std::thread& thread) {
    thread = std::thread([name, response, respond]() {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 100 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
            pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) Sleep(10);
        }
        if (pipe == INVALID_HANDLE_VALUE) return;

        DWORD mode = PIPE_READMODE_BYTE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

        if (respond) {
            char buffer[256]{};
            DWORD read = 0;
            ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr);

            DWORD written = 0;
            const std::string framed = response + "\n";
            WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr);
        }

        CloseHandle(pipe);
    });
}

bool TestConnectAndCall() {
    const auto name = MakePipeName(L"call");
    std::thread client;
    StartClient(name, R"({"id":1,"consume":false})", true, client);

    PipeBridge bridge(500);
    const bool connected = ConnectEventually(bridge, name);
    std::string response;
    const bool called = connected && bridge.Call(R"({"id":1,"type":"hello","protocol":1})", response, 1);
    client.join();

    return Expect(connected && called && response.find(R"("id":1)") != std::string::npos,
        "server-side bridge should accept a client and round-trip a response");
}

bool TestPendingClient() {
    const auto name = MakePipeName(L"pending");
    PipeBridge bridge(100);

    const bool listenerStarted = bridge.StartListener(name.c_str());
    const bool initiallyPending = !bridge.IsConnected();

    std::thread client;
    StartClient(name, R"({"id":1,"consume":false})", true, client);

    bool connected = false;
    for (int attempt = 0; attempt < 20 && !connected; ++attempt) {
        connected = bridge.Connect(name.c_str());
        if (!connected) Sleep(10);
    }
    client.join();

    return Expect(listenerStarted && initiallyPending && connected,
        "listener should start before a client arrives and become connected after a client arrives");
}

bool TestClientDisconnect() {
    const auto name = MakePipeName(L"disconnect");
    std::thread client;
    StartClient(name, "", false, client);

    PipeBridge bridge(100);
    const bool connected = ConnectEventually(bridge, name);
    std::string response;
    const bool called = connected && bridge.Call(R"({"id":1})", response, 1);
    client.join();

    return Expect(connected && !called && !bridge.IsConnected(), "client disconnect should be detected and clear the connection");
}

bool TestReconnect() {
    const auto name = MakePipeName(L"reconnect");
    std::thread client1;
    StartClient(name, R"({"id":1,"consume":false})", true, client1);

    PipeBridge bridge(500);
    std::string response;
    const bool first = ConnectEventually(bridge, name) && bridge.Call(R"({"id":1})", response, 1);
    client1.join();

    const bool disconnected = !bridge.IsConnected();

    std::thread client2;
    StartClient(name, R"({"id":1,"consume":false})", true, client2);
    bool secondConnected = false;
    for (int attempt = 0; attempt < 20 && !secondConnected; ++attempt) {
        secondConnected = bridge.Connect(name.c_str());
        if (!secondConnected) Sleep(10);
    }
    const bool second = secondConnected && bridge.Call(R"({"id":1})", response, 1);
    client2.join();

    return Expect(first && disconnected && second, "bridge should accept a new TypeScript client after restart");
}

bool TestLatestClientWins() {
    const auto name = MakePipeName(L"latest");

    PipeBridge bridge(500);

    std::thread client1([&]() {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 100 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
            pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) Sleep(10);
        }
        if (pipe == INVALID_HANDLE_VALUE) return;

        DWORD mode = PIPE_READMODE_BYTE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

        char buffer[256]{};
        DWORD read = 0;
        const BOOL readResult = ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr);
        if (!readResult) {
            // Expected: the bridge kicks this client when client2 arrives.
        }
        CloseHandle(pipe);
    });

    const bool firstConnected = ConnectEventually(bridge, name);

    std::thread client2([&]() {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 100 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
            pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) Sleep(10);
        }
        if (pipe == INVALID_HANDLE_VALUE) return;

        DWORD mode = PIPE_READMODE_BYTE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

        char buffer[256]{};
        DWORD read = 0;
        if (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr)) {
            const std::string response = R"({"id":7,"consume":false})" + std::string("\n");
            DWORD written = 0;
            WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written, nullptr);
        }
        CloseHandle(pipe);
    });

    Sleep(100);

    std::string response;
    const bool called = firstConnected &&
        bridge.Call(R"({"id":7})", response, 7);

    client2.join();
    client1.join();

    return Expect(firstConnected && called && response.find(R"("id":7)") != std::string::npos,
        "newest client should replace the previous active client");
}
}

int main() {
    bool ok = true;
    ok = TestConnectAndCall() && ok;
    ok = TestPendingClient() && ok;
    ok = TestClientDisconnect() && ok;
    ok = TestReconnect() && ok;
    ok = TestLatestClientWins() && ok;
    if (!ok) return EXIT_FAILURE;
    std::cout << "PipeBridgeTest: all tests passed\n";
    return EXIT_SUCCESS;
}
