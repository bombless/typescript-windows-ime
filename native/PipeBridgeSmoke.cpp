#include "PipeBridge.h"

#include <iostream>
#include <string>
#include <thread>

int main() {
    constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\TypeScriptWindowsIME";
    PipeBridge bridge(1000);
    bool connected = false;
    for (int attempt = 0; attempt < 40 && !connected; ++attempt) {
        connected = bridge.Connect(kPipeName);
        if (!connected) Sleep(50);
    }
    if (!connected) {
        std::cerr << "PipeBridge: TypeScript client did not connect.\n";
        return 2;
    }
    std::string response;
    if (!bridge.Call(R"({"id":1,"type":"hello","protocol":1})", response, 1)) {
        std::cerr << "PipeBridge: hello failed\n";
        return 3;
    }
    std::cout << response << "\n";
    if (!bridge.Call(R"({"id":2,"type":"keyDown","vk":65,"scanCode":30,"key":"A","modifiers":0})", response, 2)) {
        std::cerr << "PipeBridge: keyDown failed\n";
        return 4;
    }
    std::cout << response << "\n";
    return 0;
}