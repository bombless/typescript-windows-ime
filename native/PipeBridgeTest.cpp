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

void StartServer(const std::wstring& name, const std::string& response, DWORD delayMs,
                 bool closeWithoutResponse, std::thread& thread) {
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 16384, 16384, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        thread = std::thread([] {});
        return;
    }
    thread = std::thread([pipe, response, delayMs, closeWithoutResponse]() {
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) {
            if (delayMs) Sleep(delayMs);
            if (!closeWithoutResponse) {
                const std::string framed = response + "\n";
                DWORD written = 0;
                WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr);
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    });
}

bool Expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << "\n";
    return condition;
}

bool TestConnectFailure() {
    PipeBridge bridge(100);
    return Expect(!bridge.Connect(MakePipeName(L"missing").c_str()), "connect failure should return false");
}

bool TestResponseTimeout() {
    const auto name = MakePipeName(L"timeout"); std::thread server;
    StartServer(name, "", 300, true, server);
    PipeBridge bridge(50); const bool connected = bridge.Connect(name.c_str());
    std::string response; const bool called = connected && bridge.Call(R"({"id":1})", response, 1);
    server.join();
    return Expect(connected && !called && !bridge.IsConnected(), "response timeout should fail and disconnect");
}

bool TestServerDisconnect() {
    const auto name = MakePipeName(L"disconnect"); std::thread server;
    StartServer(name, "", 0, true, server);
    PipeBridge bridge(500); const bool connected = bridge.Connect(name.c_str());
    std::string response; const bool called = connected && bridge.Call(R"({"id":2})", response, 2);
    server.join();
    return Expect(connected && !called && !bridge.IsConnected(), "server disconnect should fail and disconnect");
}

bool TestMalformedResponse() {
    const auto name = MakePipeName(L"malformed"); std::thread server;
    StartServer(name, "hello", 0, false, server);
    PipeBridge bridge(500); const bool connected = bridge.Connect(name.c_str());
    std::string response; const bool called = connected && bridge.Call(R"({"id":3})", response, 3);
    server.join();
    return Expect(connected && !called, "malformed response should be rejected");
}

bool TestWrongResponseId() {
    const auto name = MakePipeName(L"wrong-id"); std::thread server;
    StartServer(name, R"({"id":999,"consume":false})", 0, false, server);
    PipeBridge bridge(500); const bool connected = bridge.Connect(name.c_str());
    std::string response; const bool called = connected && bridge.Call(R"({"id":4})", response, 4);
    server.join();
    return Expect(connected && !called, "wrong response id should be rejected");
}

bool TestReconnect() {
    const auto name1 = MakePipeName(L"reconnect-1"); std::thread server1;
    StartServer(name1, R"({"id":5,"consume":false})", 0, false, server1);
    PipeBridge bridge(500); std::string response;
    const bool first = bridge.Connect(name1.c_str()) && bridge.Call(R"({"id":5})", response, 5);
    server1.join();
    const auto name2 = MakePipeName(L"reconnect-2"); std::thread server2;
    StartServer(name2, R"({"id":6,"consume":false})", 0, false, server2);
    const bool second = bridge.Connect(name2.c_str()) && bridge.Call(R"({"id":6})", response, 6);
    server2.join();
    return Expect(first && second && bridge.IsConnected(), "bridge should reconnect and call successfully");
}
}

int main() {
    bool ok = true;
    ok = TestConnectFailure() && ok;
    ok = TestResponseTimeout() && ok;
    ok = TestServerDisconnect() && ok;
    ok = TestMalformedResponse() && ok;
    ok = TestWrongResponseId() && ok;
    ok = TestReconnect() && ok;
    if (!ok) return EXIT_FAILURE;
    std::cout << "PipeBridgeTest: all tests passed\n";
    return EXIT_SUCCESS;
}