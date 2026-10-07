#pragma once

#include <windows.h>
#include <string>

class PipeBridge {
public:
    explicit PipeBridge(DWORD timeoutMs = 250);
    ~PipeBridge();
    PipeBridge(const PipeBridge&) = delete;
    PipeBridge& operator=(const PipeBridge&) = delete;
    bool Connect(const wchar_t* pipeName);
    void Disconnect();
    bool IsConnected() const;
    bool Call(const std::string& requestJson, std::string& responseJson, unsigned int requestId);
private:
    bool WriteAll(const char* data, DWORD size);
    bool ReadLine(std::string& line);
    bool WaitForIo(OVERLAPPED& overlapped, DWORD& transferred);
    HANDLE pipe_;
    DWORD timeoutMs_;
};