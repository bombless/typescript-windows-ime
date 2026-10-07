#include "PipeBridge.h"

#include <iostream>
#include <string>

int main() {
    constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\TypeScriptWindowsIME";
    PipeBridge bridge(1000);
    if (!bridge.Connect(kPipeName)) {
        std::cerr << "PipeBridge: connect failed; start npm run dev first.\n";
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