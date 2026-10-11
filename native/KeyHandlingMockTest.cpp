// Mock-core tests for the TSF key sink's Backspace/Delete handling.
//
// The key path in native/TsIme.cpp asks the TypeScript core over the named pipe
// whether a key belongs to the IME, then honors the consume flag of the reply.
// These tests replace that core with a scripted named-pipe server that keeps
// one pinyin composition using the same rules as src/ime/reducer.ts, so
// VK_BACK and VK_DELETE can be checked without TSF, an application, or a
// running Host. TsIme.cpp is included because TextService and KeyEventSink live
// in its anonymous namespace.
#include "TsIme.cpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr DWORD kReadBufferSize = 4096;

std::wstring TestPipeName() {
    return L"\\\\.\\pipe\\TypeScriptWindowsIME.KeyHandlingMockTest_" +
           std::to_wstring(GetCurrentProcessId());
}

bool Expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << "\n";
    return condition;
}

// A stand-in for the Node core: it keeps one pinyin composition and answers
// key requests with the consume rules of src/ime/reducer.ts. Every key request
// is recorded so the test can inspect the JSON the C++ service put on the wire.
class MockCore {
public:
    explicit MockCore(const std::wstring& pipeName)
        : pipeName_(pipeName), stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        thread_ = std::thread(&MockCore::Run, this);
    }

    ~MockCore() {
        if (stopEvent_) SetEvent(stopEvent_);
        if (thread_.joinable()) thread_.join();
        if (client_ != INVALID_HANDLE_VALUE) {
            CloseHandle(client_);
            client_ = INVALID_HANDLE_VALUE;
        }
        if (stopEvent_) CloseHandle(stopEvent_);
    }

    // WaitNamedPipeW returns as soon as the pipe is absent, so readiness is
    // reported by the mock itself instead of by the pipe namespace.
    bool WaitUntilReady(DWORD timeoutMs) {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < deadline) {
            if (ready_) return true;
            Sleep(5);
        }
        return false;
    }

    std::string Composition() {
        std::lock_guard<std::mutex> lock(mutex_);
        return composition_;
    }

    size_t KeyRequestCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }

    std::string LastKeyRequest() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.empty() ? std::string() : requests_.back();
    }

private:
    // Mirrors PipeBridge's overlapped waits so the mock can be stopped while an
    // operation is still in flight.
    bool WaitForIo(HANDLE pipe, OVERLAPPED& overlapped, DWORD& transferred) {
        HANDLE waits[] = { stopEvent_, overlapped.hEvent };
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait != WAIT_OBJECT_0 + 1) {
            CancelIoEx(pipe, &overlapped);
            return false;
        }
        return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
    }

    bool Write(const std::string& reply) {
        const std::string framed = reply + "\n";
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const BOOL started = WriteFile(client_, framed.data(), static_cast<DWORD>(framed.size()),
            nullptr, &overlapped);
        bool ok = started || GetLastError() == ERROR_IO_PENDING;
        if (ok) {
            DWORD written = 0;
            ok = WaitForIo(client_, overlapped, written);
        }
        CloseHandle(overlapped.hEvent);
        return ok;
    }

    bool ReadLine(std::string& line) {
        line.clear();
        char buffer[kReadBufferSize];
        for (;;) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            const BOOL started = ReadFile(client_, buffer, sizeof(buffer), nullptr, &overlapped);
            bool ok = started || GetLastError() == ERROR_IO_PENDING;
            DWORD read = 0;
            if (ok) ok = WaitForIo(client_, overlapped, read);
            CloseHandle(overlapped.hEvent);
            if (!ok || read == 0) return false;
            line.append(buffer, buffer + read);
            const size_t newline = line.find('\n');
            if (newline != std::string::npos) {
                line.resize(newline);
                return true;
            }
        }
    }

    // Replays the reducer rules of src/ime/reducer.ts closely enough to decide
    // consume: letters extend the composition, Backspace drops the last
    // letter, Delete drops the first one, and anything else belongs to the
    // application. The test pass asks without mutating, like engine.testKey.
    std::string Reply(const std::string& request) {
        const std::string type = JsonString(request, "type");
        const std::string key = JsonString(request, "key");
        std::lock_guard<std::mutex> lock(mutex_);
        if (type != "testKeyDown" && type != "keyDown" && type != "keyUp") return std::string();
        requests_.push_back(request);

        bool consume = false;
        if (type == "testKeyDown") {
            consume = !composition_.empty();
        } else if (type == "keyDown") {
            if (key.size() == 1 && ((key[0] >= 'a' && key[0] <= 'z') || (key[0] >= 'A' && key[0] <= 'Z'))) {
                composition_ += static_cast<char>(key[0] | 0x20);
                consume = true;
            } else if (key == "Backspace" && !composition_.empty()) {
                composition_.pop_back();
                consume = true;
            } else if (key == "Delete" && !composition_.empty()) {
                composition_.erase(composition_.begin());
                consume = true;
            }
        }
        // A keyUp carries no decision: reduceKey only ever consumes keyDown.
        return "{\"id\":" + std::to_string(JsonUInt(request, "id", 0)) +
               ",\"consume\":" + (consume ? "true" : "false") +
               ",\"composition\":\"" + composition_ + "\"}";
    }

    void Run() {
        HANDLE pipe = CreateNamedPipeW(pipeName_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, kReadBufferSize, kReadBufferSize, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return;
        ready_ = true;

        OVERLAPPED connect{};
        connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = ConnectNamedPipe(pipe, &connect) != FALSE;
        if (!connected) {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (error == ERROR_IO_PENDING) {
                DWORD transferred = 0;
                connected = WaitForIo(pipe, connect, transferred);
            }
        }
        CloseHandle(connect.hEvent);
        if (!connected) {
            CloseHandle(pipe);
            return;
        }
        client_ = pipe;

        std::string line;
        while (ReadLine(line)) {
            const std::string reply = Reply(line);
            // Notifications such as hideCandidates expect no answer, and a
            // stray reply would desynchronize the next Call id check.
            if (!reply.empty() && !Write(reply)) break;
        }

        CancelIoEx(client_, nullptr);
        CloseHandle(client_);
        client_ = INVALID_HANDLE_VALUE;
    }

    std::wstring pipeName_;
    HANDLE stopEvent_;
    HANDLE client_ = INVALID_HANDLE_VALUE;
    std::thread thread_;
    std::atomic<bool> ready_{false};
    std::mutex mutex_;
    std::string composition_;
    std::vector<std::string> requests_;
};
} // namespace

int main() {
    const std::wstring pipeName = TestPipeName();
    // TextService::CallCore reads the seam at call time, so the mock owns the
    // pipe the service connects to instead of racing the real Host.
    g_servicePipeName = pipeName.c_str();

    MockCore core(pipeName);
    if (!core.WaitUntilReady(2000)) {
        std::cerr << "FAIL: the mock core did not create its pipe\n";
        return EXIT_FAILURE;
    }

    bool ok = true;
    {
        TextService service;
        KeyEventSink sink(&service);

        // TSF asks the sink before it routes the key, so both calls have to
        // agree or the application sees the key twice.
        const auto testKey = [&sink](WPARAM vk) {
            BOOL eaten = FALSE;
            sink.OnTestKeyDown(nullptr, vk, 0, &eaten);
            return eaten == TRUE;
        };
        const auto keyDown = [&sink](WPARAM vk) {
            BOOL eaten = FALSE;
            sink.OnKeyDown(nullptr, vk, 0, &eaten);
            return eaten == TRUE;
        };

        ok = Expect(keyDown('N') && keyDown('I'), "letters during composition must be consumed") && ok;
        ok = Expect(core.Composition() == "ni", "the mock core must hold the pinyin composition 'ni'") && ok;

        ok = Expect(testKey(VK_BACK), "backspace during pinyin must be consumed in the test pass") && ok;
        ok = Expect(core.Composition() == "ni", "the test pass must not change the composition") && ok;
        ok = Expect(core.LastKeyRequest().find("\"type\":\"testKeyDown\"") != std::string::npos,
            "backspace must be forwarded as a testKeyDown request") && ok;

        ok = Expect(keyDown(VK_BACK), "backspace during pinyin must be consumed") && ok;
        ok = Expect(core.Composition() == "n", "backspace must drop the last pinyin letter") && ok;
        std::string request = core.LastKeyRequest();
        ok = Expect(request.find("\"type\":\"keyDown\"") != std::string::npos,
            "backspace must be forwarded as a keyDown request") && ok;
        ok = Expect(request.find("\"vk\":8") != std::string::npos, "backspace must carry vk 8") && ok;
        ok = Expect(request.find("\"key\":\"Backspace\"") != std::string::npos,
            "backspace must carry the key name Backspace") && ok;
        ok = Expect(request.find("\"session\":") != std::string::npos,
            "backspace must carry the session id the core routes on") && ok;

        // Delete used to be dropped in TextService::KeyName, so it never
        // reached the core: the composition kept its letters while the
        // application deleted the text behind the IME.
        ok = Expect(testKey(VK_DELETE), "delete during pinyin must be consumed in the test pass") && ok;
        ok = Expect(keyDown(VK_DELETE), "delete during pinyin must be consumed") && ok;
        ok = Expect(core.Composition() == "", "delete must drop a pinyin letter") && ok;
        request = core.LastKeyRequest();
        ok = Expect(request.find("\"vk\":46") != std::string::npos, "delete must carry vk 46") && ok;
        ok = Expect(request.find("\"key\":\"Delete\"") != std::string::npos,
            "delete must carry the key name Delete") && ok;

        ok = Expect(!testKey(VK_BACK), "backspace with no composition must not be consumed in the test pass") && ok;
        ok = Expect(!keyDown(VK_BACK), "backspace with no composition must reach the application") && ok;
        ok = Expect(!testKey(VK_DELETE), "delete with no composition must not be consumed in the test pass") && ok;
        ok = Expect(!keyDown(VK_DELETE), "delete with no composition must reach the application") && ok;

        // Keys the sink cannot name are neither swallowed nor forwarded. VK_HOME
        // stays outside TextService::KeyName, unlike the F-keys, whose vk values
        // overlap the lowercase letter range.
        const size_t requestsBeforeUnmappedKey = core.KeyRequestCount();
        ok = Expect(!keyDown(VK_HOME), "a key without a name must reach the application") && ok;
        ok = Expect(core.KeyRequestCount() == requestsBeforeUnmappedKey,
            "a key without a name must not be sent to the core") && ok;
    }

    if (!ok) return EXIT_FAILURE;
    std::cout << "KeyHandlingMockTest: all tests passed\n";
    return EXIT_SUCCESS;
}
