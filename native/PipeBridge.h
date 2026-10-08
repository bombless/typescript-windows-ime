#pragma once

#include <windows.h>
#include <string>
#include <mutex>
#include <thread>

class PipeBridge {
public:
    explicit PipeBridge(DWORD timeoutMs = 250);
    ~PipeBridge();
    PipeBridge(const PipeBridge&) = delete;
    PipeBridge& operator=(const PipeBridge&) = delete;

    // Starts the permanent listener. Returns true when the listener is running.
    bool StartListener(const wchar_t* pipeName);
    // Starts the listener if needed and returns true when a client is active.
    bool Connect(const wchar_t* pipeName);
    bool ConnectToServer(const wchar_t* pipeName, DWORD timeoutMs = 250);
    void Disconnect();
    bool IsConnected();
    bool Call(const std::string& requestJson, std::string& responseJson, unsigned int requestId);

private:
    void AcceptLoop();
    HANDLE CreatePipeInstance();
    void DisconnectLocked();

    bool WriteAll(HANDLE pipe, const char* data, DWORD size);
    bool ReadLine(HANDLE pipe, std::string& line);
    bool WaitForIo(HANDLE pipe, OVERLAPPED& overlapped, DWORD& transferred);

    HANDLE pipe_;
    HANDLE stopEvent_;
    HANDLE listenerReadyEvent_;
    std::thread acceptThread_;
    std::mutex mutex_;
    std::wstring pipeName_;
    bool acceptStarted_;
    DWORD timeoutMs_;
};
