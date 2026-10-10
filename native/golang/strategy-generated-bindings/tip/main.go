// In-process text service for the strategy-generated-bindings input method.
// Its CLSID and profile GUID are not the C++ or Go TIP identities.
package main

/*
#include <stdint.h>
#define DllGetClassObject WindowsSdkDllGetClassObject
#define DllCanUnloadNow WindowsSdkDllCanUnloadNow
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#undef DllGetClassObject
#undef DllCanUnloadNow

static const CLSID kClsid = {0xb18f4c27, 0x9a63, 0x4e15, {0x8d, 0x70, 0x2f, 0x6c, 0x1a, 0x5b, 0x9e, 0x34}};
static const GUID kProfile = {0x4d92e7a1, 0x6b08, 0x4c53, {0xa1, 0xf6, 0x8e, 0x3d, 0x0c, 0x7b, 0x5a, 0x29}};
static const wchar_t kName[] = L"strategy-generated-bindings";

static HRESULT adviseKeySink(void* threadMgr, DWORD clientId, void* sink) {
    ITfKeystrokeMgr* keys = NULL;
    HRESULT hr = ((IUnknown*)threadMgr)->lpVtbl->QueryInterface((IUnknown*)threadMgr, &IID_ITfKeystrokeMgr, (void**)&keys);
    if (FAILED(hr)) return hr;
    ITfKeyEventSink* checkedSink = NULL; hr = ((IUnknown*)sink)->lpVtbl->QueryInterface((IUnknown*)sink, &IID_ITfKeyEventSink, (void**)&checkedSink); if (FAILED(hr)) { keys->lpVtbl->Release(keys); return hr; } checkedSink->lpVtbl->Release(checkedSink); hr = keys->lpVtbl->AdviseKeyEventSink(keys, clientId, (ITfKeyEventSink*)sink, TRUE);
    keys->lpVtbl->Release(keys);
    return hr;
}

static HRESULT unadviseKeySink(void* threadMgr, DWORD clientId) {
    ITfKeystrokeMgr* keys = NULL;
    HRESULT hr = ((IUnknown*)threadMgr)->lpVtbl->QueryInterface((IUnknown*)threadMgr, &IID_ITfKeystrokeMgr, (void**)&keys);
    if (FAILED(hr)) return hr;
    hr = keys->lpVtbl->UnadviseKeyEventSink(keys, clientId);
    keys->lpVtbl->Release(keys);
    return hr;
}
static const wchar_t kClsidKey[] = L"CLSID\\{B18F4C27-9A63-4E15-8D70-2F6C1A5B9E34}";
static const wchar_t kInprocKey[] = L"CLSID\\{B18F4C27-9A63-4E15-8D70-2F6C1A5B9E34}\\InprocServer32";

static HRESULT writeString(HKEY root, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* value) {
    HKEY key = NULL;
    LONG result = RegCreateKeyExW(root, subKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL);
    if (result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);
    DWORD bytes = (DWORD)((wcslen(value) + 1) * sizeof(wchar_t));
    result = RegSetValueExW(key, valueName, 0, REG_SZ, (const BYTE*)value, bytes);
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(result);
}

static HRESULT registerComServer(void) {
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    DWORD length;
    HRESULT hr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(uintptr_t)registerComServer, &module)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) return HRESULT_FROM_WIN32(GetLastError());
    hr = writeString(HKEY_CLASSES_ROOT, kClsidKey, NULL, kName);
    if (FAILED(hr)) return hr;
    hr = writeString(HKEY_CLASSES_ROOT, kInprocKey, NULL, path);
    if (FAILED(hr)) return hr;
    return writeString(HKEY_CLASSES_ROOT, kInprocKey, L"ThreadingModel", L"Apartment");
}

static HRESULT unregisterComServer(void) {
    LSTATUS result = RegDeleteTreeW(HKEY_CLASSES_ROOT, kClsidKey);
    if (result == ERROR_FILE_NOT_FOUND) return S_OK;
    return HRESULT_FROM_WIN32(result);
}

static HRESULT registerTip(void) {
    ITfInputProcessorProfiles* profiles = NULL;
    ITfCategoryMgr* categories = NULL;
    const LANGID language = 0x0804; // Simplified Chinese (zh-CN) only.
    HRESULT hr = CoCreateInstance(&CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
        &IID_ITfInputProcessorProfiles, (void**)&profiles);
    if (FAILED(hr)) return hr;
    hr = profiles->lpVtbl->Register(profiles, &kClsid);
    if (SUCCEEDED(hr)) {
        // Remove legacy profiles from previous builds so they cannot remain
        // visible under English or Japanese after upgrading.
        profiles->lpVtbl->RemoveLanguageProfile(profiles, &kClsid, 0x0409, &kProfile);
        profiles->lpVtbl->RemoveLanguageProfile(profiles, &kClsid, 0x0411, &kProfile);
        hr = profiles->lpVtbl->AddLanguageProfile(profiles, &kClsid, language, &kProfile,
            kName, (ULONG)(wcslen(kName)), NULL, 0, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(&CLSID_TF_CategoryMgr, NULL, CLSCTX_INPROC_SERVER,
            &IID_ITfCategoryMgr, (void**)&categories);
        if (SUCCEEDED(hr)) {
            hr = categories->lpVtbl->RegisterCategory(categories, &kClsid, &GUID_TFCAT_TIP_KEYBOARD, &kClsid);
            categories->lpVtbl->Release(categories);
        }
    }
    profiles->lpVtbl->Release(profiles);
    return hr;
}

static HRESULT unregisterTip(void) {
    ITfInputProcessorProfiles* profiles = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
        &IID_ITfInputProcessorProfiles, (void**)&profiles);
    if (FAILED(hr)) return hr;
    hr = profiles->lpVtbl->Unregister(profiles, &kClsid);
    profiles->lpVtbl->Release(profiles);
    return hr;
}

static HRESULT runRegistration(BOOL unregister) {
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HRESULT hr;
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return init;
    if (unregister) {
        hr = unregisterTip();
        if (SUCCEEDED(hr)) hr = unregisterComServer();
    } else {
        hr = registerComServer();
        if (SUCCEEDED(hr)) hr = registerTip();
        if (FAILED(hr)) unregisterComServer();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return hr;
}
*/
import "C"

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"runtime/debug"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"github.com/zzl/go-com/com"
	"github.com/zzl/go-com/com/comimpl"
	"github.com/zzl/go-win32api/v2/win32"
	"strategy-generated-bindings/closedtsf"
)

// The same pipe the C++ text service uses. The host accepts every TSF client at
// once, so switching to this input method shares the running host.
// tsfPipeName is the same pipe the C++ text service uses. The host accepts
// every TSF client at once, so switching to this input method shares the running
// host. It is a variable, not a constant, so a test can point the TIP at its own
// pipe instead of racing a real host for the shared one.
var tsfPipeName = `\\.\pipe\TypeScriptWindowsIME.Tsf`

const (
	pipeDialTimeout = 250 * time.Millisecond
	// pipeReplyTimeout bounds one round trip taken inside a TSF key callback.
	// The Host drops a request outright when no Node client is attached, so in
	// that state no reply ever arrives: an unbounded wait would freeze the
	// application's typing thread until the process is killed. 250ms matches the
	// C++ PipeBridge default and still leaves an order of magnitude over the
	// ~30ms a healthy round trip takes.
	pipeReplyTimeout = 250 * time.Millisecond
)

const errorPipeBusy = syscall.Errno(231)

var errPipeStall = errors.New("timed out")

var (
	// GetKeyState is a user32 export, not a kernel32 one. Looking it up in the
	// wrong DLL panics on the first key event, which a guarded callback turns
	// into a silent E_FAIL for every keystroke.
	kernel32                    = syscall.NewLazyDLL("kernel32.dll")
	user32                      = syscall.NewLazyDLL("user32.dll")
	procCreateEventW            = kernel32.NewProc("CreateEventW")
	procGetCurrentThreadId      = kernel32.NewProc("GetCurrentThreadId")
	procGetKeyState             = user32.NewProc("GetKeyState")
	procGetOverlappedResult     = kernel32.NewProc("GetOverlappedResult")
	procSetNamedPipeHandleState = kernel32.NewProc("SetNamedPipeHandleState")
	procWaitNamedPipeW          = kernel32.NewProc("WaitNamedPipeW")
)

// One pipe per process. A later Activate or Deactivate bumps the generation so
// the goroutine that still owns the previous handle closes it.
var (
	pipeMu         sync.Mutex
	pipeHandle     = syscall.InvalidHandle
	pipeGeneration atomic.Uint64
	pipeRequestID  atomic.Uint64
	pipeReady      atomic.Bool
)

func threadID() uintptr {
	id, _, _ := procGetCurrentThreadId.Call()
	return id
}

func strategySessionID() uint64 {
	// Low bits stay clear of the C++ per-process counter. The value stays inside
	// a JavaScript safe integer and inside the host's 48-bit session field.
	return uint64(os.Getpid())<<20 | 0x80001
}

// Everything the TIP has to say lands in one shared file so a fault inside a
// host application can be reconstructed after the fact. The Host writes to the
// same path by default, which means one file interleaves both sides of the
// conversation. TypeScriptWindowsIMELogPath overrides it, which is how a test
// or a second TIP instance is kept out of the shared file.
const strategyLogPath = "TypeScriptWindowsIME.log"

const strategyLogPathEnvVar = "TypeScriptWindowsIMELogPath"

func strategyLogPathFull() string {
	if override := os.Getenv(strategyLogPathEnvVar); override != "" {
		return override
	}
	return filepath.Join(os.TempDir(), strategyLogPath)
}

func strategyLog(format string, args ...any) {
	message := fmt.Sprintf(format, args...)
	line := fmt.Sprintf("[%s][pid=%d tid=%d] %s\r\n",
		time.Now().Format("2006-01-02 15:04:05.000"),
		os.Getpid(), threadID(), message)
	path, err := syscall.UTF16PtrFromString(strategyLogPathFull())
	if err != nil {
		return
	}
	file, err := syscall.CreateFile(path, syscall.FILE_APPEND_DATA,
		syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE|syscall.FILE_SHARE_DELETE,
		nil, syscall.OPEN_ALWAYS, syscall.FILE_ATTRIBUTE_NORMAL, 0)
	if err != nil {
		return
	}
	var written uint32
	_ = syscall.WriteFile(file, []byte(line), &written, nil)
	_ = syscall.CloseHandle(file)
}

// Every failure here has to be survivable. This DLL runs inside whatever
// application the user typed into, so a panic that unwinds through a native
// frame takes that process down with it. Callbacks therefore recover, dump every
// goroutine, and answer E_FAIL, which TSF reports as a plain activation or key
// failure instead of a crash.
const strategyEFail int32 = -2147467259 // 0x80004005

// hresultToUintptr converts a signed HRESULT into the unsigned 32-bit value COM
// returns in the register. It has to be a function: Go rejects converting a
// negative constant straight to uintptr at compile time.
func hresultToUintptr(hr int32) uintptr {
	return uintptr(uint32(hr))
}

// writeStackDump appends every goroutine, not just the failing one. The blocked
// pipe reader and a callback waiting on a dead lock are only visible in a full
// dump, and this DLL cannot be inspected with a source-level debugger once it
// is loaded into an application.
func writeStackDump(reason string) {
	size := 1 << 18
	for {
		buffer := make([]byte, size)
		written := runtime.Stack(buffer, true)
		if written < len(buffer) {
			strategyLog("goroutine dump (%s):\r\n%s", reason, string(buffer[:written]))
			return
		}
		size *= 2
		if size > 32<<20 {
			strategyLog("goroutine dump (%s): truncated at %d bytes", reason, size)
			return
		}
	}
}

// guardedCall runs one COM callback and keeps a fault inside it. The deferred
// recover belongs to this frame, so it still fires after run has unwound, and
// the HRESULT written to result is what the caller receives. A recovered panic
// must never escape past here: the frames underneath belong to the OS.
func guardedCall(name string, run func() uintptr) (result uintptr) {
	result = hresultToUintptr(strategyEFail)
	defer func() {
		recovered := recover()
		if recovered == nil {
			return
		}
		strategyLog("PANIC %s: %v", name, recovered)
		writeStackDump("panic in " + name)
		result = hresultToUintptr(strategyEFail)
	}()
	return run()
}

// dumpWatchPath is polled so a stuck TIP inside an application that cannot be
// restarted can still be inspected: create this file and the next poll appends
// every goroutine stack to the log. Deleting it afterwards arms the next dump.
func dumpWatchPath() string {
	return filepath.Join(os.TempDir(), "TypeScriptWindowsIME.dump")
}

func watchForDumpRequests() {
	path := dumpWatchPath()
	for {
		if _, err := os.Stat(path); err == nil {
			writeStackDump("dump requested by " + path)
			_ = os.Remove(path)
		}
		time.Sleep(time.Second)
	}
}

// strategyPauseAt freezes the calling thread at a named point so a dump can be
// taken while the object is still alive. It is off unless the variable is set
// to that exact point name, because a blocked TSF callback hangs the host
// application and that is only ever wanted deliberately.
func strategyPauseAt(point string) {
	wanted := os.Getenv("TypeScriptWindowsIMEPauseAt")
	if wanted == "" || wanted != point {
		return
	}
	strategyLog("StrategyTip paused at %s pid=%d tid=%d", point, os.Getpid(), threadID())
	for {
		time.Sleep(time.Hour)
	}
}

func init() {
	// Default Go traceback behaviour prints to stderr, which a GUI application
	// never shows, and only the crashing goroutine. Both sinks are useless here.
	if os.Getenv("GOTRACEBACK") == "" {
		_ = os.Setenv("GOTRACEBACK", "all")
	}
	debug.SetTraceback("all")
	strategyLog("StrategyTip DLL loaded pid=%d tid=%d GOTRACEBACK=%s dumpWatch=%s",
		os.Getpid(), threadID(), os.Getenv("GOTRACEBACK"), dumpWatchPath())
	go watchForDumpRequests()
}

func probeLines(session uint64) (string, string) {
	hello := fmt.Sprintf(`{"id":1,"session":%d,"type":"hello","protocol":3}`, session)
	show := fmt.Sprintf(`{"id":2,"session":%d,"type":"showCandidates","candidates":["strategy-generated-bindings"],"selection":0,"caret":{"left":200,"top":200,"right":400,"bottom":220},"dpi":96}`, session)
	return hello, show
}

func errnoOf(err error) syscall.Errno {
	var errno syscall.Errno
	if errors.As(err, &errno) {
		return errno
	}
	return 0
}

func dialTsf(timeout time.Duration) (syscall.Handle, error) {
	name, err := syscall.UTF16PtrFromString(tsfPipeName)
	if err != nil {
		return syscall.InvalidHandle, err
	}
	deadline := time.Now().Add(timeout)
	var last error
	for {
		handle, err := syscall.CreateFile(name, syscall.GENERIC_READ|syscall.GENERIC_WRITE,
			0, nil, syscall.OPEN_EXISTING, syscall.FILE_FLAG_OVERLAPPED, 0)
		if err == nil {
			mode := uint32(0) // PIPE_READMODE_BYTE
			result, _, setErr := procSetNamedPipeHandleState.Call(uintptr(handle), uintptr(unsafe.Pointer(&mode)), 0, 0)
			if result == 0 {
				strategyLog("StrategyTip SetNamedPipeHandleState failed win32=%v", setErr)
			}
			return handle, nil
		}
		last = err
		code := errnoOf(err)
		if code != errorPipeBusy && code != syscall.ERROR_FILE_NOT_FOUND {
			return syscall.InvalidHandle, err
		}
		if !time.Now().Before(deadline) {
			return syscall.InvalidHandle, last
		}
		waited, _, _ := procWaitNamedPipeW.Call(uintptr(unsafe.Pointer(name)), 25)
		if waited == 0 {
			time.Sleep(25 * time.Millisecond)
		}
		if !time.Now().Before(deadline) {
			return syscall.InvalidHandle, last
		}
	}
}

func createEvent() (syscall.Handle, error) {
	handle, _, err := procCreateEventW.Call(0, 1, 0, 0)
	if syscall.Handle(handle) == 0 || syscall.Handle(handle) == syscall.InvalidHandle {
		return syscall.InvalidHandle, err
	}
	return syscall.Handle(handle), nil
}

func overlappedResult(handle syscall.Handle, overlapped *syscall.Overlapped) (uint32, error) {
	var transferred uint32
	result, _, err := procGetOverlappedResult.Call(uintptr(handle), uintptr(unsafe.Pointer(overlapped)),
		uintptr(unsafe.Pointer(&transferred)), 0)
	if result == 0 {
		return 0, err
	}
	return transferred, nil
}

func waitIO(handle syscall.Handle, overlapped *syscall.Overlapped, event syscall.Handle, timeoutMs uint32) (uint32, error) {
	wait, err := syscall.WaitForSingleObject(event, timeoutMs)
	if err != nil {
		_ = syscall.CancelIoEx(handle, overlapped)
		_, _ = overlappedResult(handle, overlapped)
		return 0, err
	}
	if wait != syscall.WAIT_OBJECT_0 {
		// Cancelling a read that has just completed throws away the bytes it
		// already delivered, and a byte-mode named pipe never resends them: the
		// stream then resumes in the middle of a line and the reply is torn in
		// two. Only the write path may cancel; the reader waits for the pipe to
		// be closed, which CancelIoEx on the whole handle already arranges.
		if timeoutMs != 0 {
			_ = syscall.CancelIoEx(handle, overlapped)
			_, _ = overlappedResult(handle, overlapped)
		}
		if wait == syscall.WAIT_TIMEOUT {
			return 0, errPipeStall
		}
		return 0, fmt.Errorf("wait %d", wait)
	}
	return overlappedResult(handle, overlapped)
}

func writeLine(handle syscall.Handle, line string) error {
	payload := append([]byte(line), '\n')
	for len(payload) > 0 {
		event, err := createEvent()
		if err != nil {
			return err
		}
		overlapped := syscall.Overlapped{HEvent: event}
		var done uint32
		err = syscall.WriteFile(handle, payload, &done, &overlapped)
		if err == syscall.ERROR_IO_PENDING {
			done, err = waitIO(handle, &overlapped, event, 2000)
		} else if err == nil && done == 0 {
			done, err = overlappedResult(handle, &overlapped)
		}
		_ = syscall.CloseHandle(event)
		if err != nil {
			return err
		}
		if done == 0 {
			return errors.New("empty write")
		}
		payload = payload[done:]
	}
	return nil
}

// readSome reads whatever is available. The wait is unbounded on purpose: a
// read that is cancelled after completing has its bytes thrown away, and the
// pipe will not send them again, so a partial line would be reassembled from
// the middle. Teardown cancels every pending read on the handle with
// CancelIoEx, which ends this wait with an error the caller already handles.
func readSome(handle syscall.Handle, buffer []byte) (int, error) {
	event, err := createEvent()
	if err != nil {
		return 0, err
	}
	defer func() { _ = syscall.CloseHandle(event) }()
	overlapped := syscall.Overlapped{HEvent: event}
	var done uint32
	err = syscall.ReadFile(handle, buffer, &done, &overlapped)
	if err == syscall.ERROR_IO_PENDING {
		done, err = waitIO(handle, &overlapped, event, 0)
	} else if err == nil && done == 0 {
		done, err = overlappedResult(handle, &overlapped)
	}
	if err != nil {
		return 0, err
	}
	return int(done), nil
}

func storePipe(generation uint64, handle syscall.Handle) bool {
	pipeMu.Lock()
	defer pipeMu.Unlock()
	if pipeGeneration.Load() != generation {
		return false
	}
	pipeHandle = handle
	pipeReady.Store(false)
	return true
}

func releaseIfOwner(handle syscall.Handle) {
	pipeMu.Lock()
	if pipeHandle == handle {
		pipeHandle = syscall.InvalidHandle
		pipeReady.Store(false)
	}
	pipeMu.Unlock()
	_ = syscall.CloseHandle(handle)
}

func stopPipe() {
	pipeMu.Lock()
	defer pipeMu.Unlock()
	if pipeHandle != syscall.InvalidHandle {
		// Cancel while the owner cannot close the handle. Cancelling after the
		// handle is closed can hit a recycled handle from the next connection.
		_ = syscall.CancelIoEx(pipeHandle, nil)
	}
}

// A TSF key callback runs on the application's own thread and the value it
// writes into pfEaten decides whether the application ever sees the key, so the
// reply has to be waited for in place. drainPipe is the only reader of the
// pipe, so replies reach the waiting callbacks through a per-id waiter instead
// of a second read loop. Lock order is pipeMu before replyWaitersMu, and
// nothing takes them the other way round.
var (
	replyWaitersMu sync.Mutex
	replyWaiters   = map[uint64]chan string{}
)

func registerReplyWaiter(id uint64) chan string {
	waiter := make(chan string, 1)
	replyWaitersMu.Lock()
	replyWaiters[id] = waiter
	replyWaitersMu.Unlock()
	return waiter
}

func unregisterReplyWaiter(id uint64) {
	replyWaitersMu.Lock()
	delete(replyWaiters, id)
	replyWaitersMu.Unlock()
}

// deliverReply hands one Host line to the callback waiting for its id and
// reports whether it did. Lines nobody waits on (hello, showCandidates and
// keyUp replies, plus error replies that carry id 0) return false and are only
// logged, which is what happened to every reply before key events were made
// synchronous.
func deliverReply(line string) bool {
	id, ok := replyID(line)
	if !ok {
		return false
	}
	replyWaitersMu.Lock()
	defer replyWaitersMu.Unlock()
	waiter, found := replyWaiters[id]
	if !found {
		return false
	}
	waiter <- line // Buffered with room for one line, so this never blocks.
	return true
}

// failReplyWaiters releases every waiting callback without a reply. Reading
// from a closed channel yields the zero value, which the caller treats as "no
// reply". The map is swapped under the lock so a concurrent deliverReply can
// neither miss a waiter nor send on a closed channel.
func failReplyWaiters() {
	replyWaitersMu.Lock()
	waiters := replyWaiters
	replyWaiters = make(map[uint64]chan string)
	replyWaitersMu.Unlock()
	for _, waiter := range waiters {
		close(waiter)
	}
}

// replyFieldValue locates a numeric or boolean protocol field. Only an
// occurrence that starts a field — right after { or , — counts, so a
// composition or candidate string that happens to contain the field text cannot
// be mistaken for one. The remainder is scanned as text rather than parsed
// because these lines are produced by this DLL's own requests.
func replyFieldValue(line, field string) (string, bool) {
	for offset := 0; ; {
		index := strings.Index(line[offset:], field)
		if index < 0 {
			return "", false
		}
		index += offset
		if index == 0 || line[index-1] == ',' || line[index-1] == '{' {
			return line[index+len(field):], true
		}
		offset = index + len(field)
	}
}

// replyID extracts the request id a Host line answers.
func replyID(line string) (uint64, bool) {
	rest, ok := replyFieldValue(line, `"id":`)
	if !ok {
		return 0, false
	}
	start := skipSpaces(rest, 0)
	end := start
	for end < len(rest) && rest[end] >= '0' && rest[end] <= '9' {
		end++
	}
	if end == start {
		return 0, false
	}
	id, err := strconv.ParseUint(rest[start:end], 10, 64)
	if err != nil {
		return 0, false
	}
	return id, true
}

// replyConsume reports whether the Host told this TIP to keep the key. It is the
// field the whole pipeline exists to deliver.
func replyConsume(line string) bool {
	rest, ok := replyFieldValue(line, `"consume":`)
	if !ok {
		return false
	}
	return strings.HasPrefix(rest[skipSpaces(rest, 0):], "true")
}

func skipSpaces(text string, from int) int {
	for from < len(text) && (text[from] == ' ' || text[from] == '\t') {
		from++
	}
	return from
}

// keyDecision is what a key callback writes into pfEaten. Both halves matter:
// a reply that never arrived (timeout, dead pipe, or a Host with no Node client)
// must leave the key with the application, exactly like *eaten = ok && consume
// in the C++ service (native/TsIme.cpp:971).
func keyDecision(reply string, replied bool) bool {
	return replied && replyConsume(reply)
}

// connectLocked reconnects the pipe and must be called with pipeMu held.
// Activate dials once, and a Host that is restarted afterwards would otherwise
// leave this TIP speaking to a pipe nobody serves: every key would be dropped and
// the application would type straight through. One bounded attempt is made per
// key, the same short reconnect path the C++ service uses, so a Host that comes
// back within a keystroke or two is picked up again without reactivating the
// input method.
func connectLocked() bool {
	if pipeHandle != syscall.InvalidHandle && pipeReady.Load() {
		return true
	}
	generation := pipeGeneration.Add(1)
	stopPipe()
	handle, err := dialTsf(pipeDialTimeout)
	if err != nil {
		strategyLog("StrategyTip reconnect failed win32=%v", err)
		return false
	}
	if !storePipe(generation, handle) {
		_ = syscall.CloseHandle(handle)
		return false
	}
	pipeReady.Store(true)
	// No hello or probe here: the Host learns the session from the first line
	// that carries one, which every key message does.
	go drainPipe(generation, handle)
	strategyLog("StrategyTip pipe reconnected session=%d", strategySessionID())
	return true
}

// callCore performs one round trip with the Host and mirrors
// TextService::CallCore: a fresh request id, one line, and the reply's consume
// flag decides whether TSF keeps the key. The second result reports whether a
// reply arrived at all; the caller must not eat the key when it did not.
func callCore(kind string, vk, lParam uintptr) (string, bool) {
	name := keyName(vk)
	if name == "" {
		// Nothing the protocol could answer, so the key stays with the
		// application. The C++ service returns success with consume=false here.
		strategyLog("StrategyTip callCore type=%s vk=0x%x ignored: no key name", kind, vk)
		return "", true
	}
	pipeMu.Lock()
	defer pipeMu.Unlock()
	if (pipeHandle == syscall.InvalidHandle || !pipeReady.Load()) && !connectLocked() {
		strategyLog("StrategyTip callCore dropped type=%s vk=0x%x key=%q: pipe not ready", kind, vk, name)
		return "", false
	}
	id := pipeRequestID.Add(1)
	// Registered before the write so a reply that beats the callback to the
	// wait cannot be missed.
	waiter := registerReplyWaiter(id)
	defer unregisterReplyWaiter(id)
	message := fmt.Sprintf(`{"id":%d,"session":%d,"type":%q,"vk":%d,"scanCode":%d,"key":%q,"modifiers":%d}`,
		id, strategySessionID(), kind, vk, (lParam>>16)&0xff, name, currentModifiers())
	if err := writeLine(pipeHandle, message); err != nil {
		strategyLog("StrategyTip callCore write failed id=%d type=%s vk=0x%x err=%v", id, kind, vk, err)
		return "", false
	}
	strategyLog("StrategyTip callCore request id=%d type=%s vk=0x%x key=%q", id, kind, vk, name)
	timer := time.NewTimer(pipeReplyTimeout)
	defer timer.Stop()
	select {
	case reply, open := <-waiter:
		if !open {
			strategyLog("StrategyTip callCore id=%d reply lost: pipe closed", id)
			return "", false
		}
		return reply, true
	case <-timer.C:
		strategyLog("StrategyTip callCore id=%d timed out after %v (Host without a Node client?)", id, pipeReplyTimeout)
		return "", false
	}
}

func drainPipe(generation uint64, handle syscall.Handle) {
	defer func() {
		// Release the callbacks first: one of them may be holding the typing
		// thread of the application this pipe just died under.
		failReplyWaiters()
		releaseIfOwner(handle)
		strategyLog("StrategyTip pipe closed generation=%d", generation)
	}()
	buffer := make([]byte, 512)
	var pending []byte
	for pipeGeneration.Load() == generation {
		count, err := readSome(handle, buffer)
		if pipeGeneration.Load() != generation {
			return
		}
		if errors.Is(err, errPipeStall) {
			continue
		}
		if err != nil || count == 0 {
			strategyLog("StrategyTip read ended generation=%d win32=%v", generation, err)
			writeStackDump(fmt.Sprintf("pipe read ended generation=%d win32=%v", generation, err))
			return
		}
		pending = append(pending, buffer[:count]...)
		for {
			end := -1
			for i, b := range pending {
				if b == '\n' {
					end = i
					break
				}
			}
			if end < 0 {
				break
			}
			text := string(pending[:end])
			pending = pending[end+1:]
			strategyLog("StrategyTip reply %s", describeReply(text))
			deliverReply(text)
		}
	}
}

// describeReply renders a Host response with the fields that decide behaviour.
// The line used to be truncated at 200 bytes, which cut off exactly the
// consume flag and candidate list that explain why a key was not eaten.
func describeReply(text string) string {
	fields := []struct {
		key   string
		label string
	}{
		{`"id":`, "id"},
		{`"session":`, "session"},
		{`"consume":`, "consume"},
		{`"composition":`, "composition"},
		{`"selectedCandidate":`, "selected"},
	}
	var parts []string
	for _, field := range fields {
		index := strings.Index(text, field.key)
		if index < 0 {
			continue
		}
		rest := text[index+len(field.key):]
		end := strings.IndexAny(rest, ",}")
		if end < 0 {
			end = len(rest)
		}
		parts = append(parts, field.label+"="+rest[:end])
	}
	candidates := 0
	if index := strings.Index(text, `"candidates":[`); index >= 0 {
		candidates = strings.Count(text[index+len(`"candidates":[`):], "{")
	}
	summary := fmt.Sprintf("bytes=%d", len(text))
	if len(parts) > 0 {
		summary += " " + strings.Join(parts, " ")
	}
	summary += fmt.Sprintf(" candidates=%d", candidates)
	if errorIndex := strings.Index(text, `"error":`); errorIndex >= 0 {
		summary += " error=" + text[errorIndex+len(`"error":`):]
	}
	return summary + " raw=" + text
}

func connectAndProbe(generation uint64) {
	session := strategySessionID()
	pipeRequestID.Store(2) // hello and showCandidates use IDs 1 and 2.
	strategyLog("StrategyTip connect start generation=%d session=%d pipe=%s", generation, session, tsfPipeName)
	handle, err := dialTsf(pipeDialTimeout)
	if err != nil {
		// ERROR_PIPE_BUSY here means the Host accepted the connection but never
		// called ConnectNamedPipe, which is the signature of a wedged Host
		// rather than a missing one. A full dump is not useful yet because the
		// pipe reader has not started.
		strategyLog("StrategyTip connect failed generation=%d win32=%v generationState=%d",
			generation, err, pipeGeneration.Load())
		return
	}
	if !storePipe(generation, handle) {
		_ = syscall.CloseHandle(handle)
		strategyLog("StrategyTip connect discarded generation=%d", generation)
		return
	}
	hello, show := probeLines(session)
	if err = writeLine(handle, hello); err != nil {
		strategyLog("StrategyTip hello failed generation=%d win32=%v", generation, err)
		releaseIfOwner(handle)
		return
	}
	strategyLog("StrategyTip sent hello session=%d", session)
	if err = writeLine(handle, show); err != nil {
		strategyLog("StrategyTip showCandidates failed generation=%d win32=%v", generation, err)
		releaseIfOwner(handle)
		return
	}
	strategyLog("StrategyTip sent showCandidates session=%d", session)
	pipeReady.Store(true)
	strategyLog("StrategyTip pipe ready for keyboard events session=%d", session)
	drainPipe(generation, handle)
}

// {B18F4C27-9A63-4E15-8D70-2F6C1A5B9E34}
var clsidStrategy = syscall.GUID{Data1: 0xB18F4C27, Data2: 0x9A63, Data3: 0x4E15, Data4: [8]byte{0x8D, 0x70, 0x2F, 0x6C, 0x1A, 0x5B, 0x9E, 0x34}}

var iidTextInputProcessor = syscall.GUID{Data1: 0xAA80E7F7, Data2: 0x2021, Data3: 0x11D2, Data4: [8]byte{0x93, 0xE0, 0x00, 0x60, 0xB0, 0x67, 0xB8, 0x6E}}

var iidKeyEventSink = closedtsf.IID_ITfKeyEventSink

// guidText names the IIDs that matter and prints the rest, because a bare GUID
// in the log is useless when the question is "which interface did TSF ask for".
func guidText(guid syscall.GUID) string {
	switch guid {
	case clsidStrategy:
		return "CLSID_StrategyTip"
	case iidTextInputProcessor:
		return "IID_ITfTextInputProcessor"
	case iidKeyEventSink:
		return "IID_ITfKeyEventSink"
	}
	return fmt.Sprintf("{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		guid.Data1, guid.Data2, guid.Data3,
		guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
		guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7])
}

type classFactoryImpl struct {
	comimpl.IClassFactoryImpl
	locks atomic.Int32
}

func (f *classFactoryImpl) CreateInstance(outer *win32.IUnknown, iid *syscall.GUID, out unsafe.Pointer) win32.HRESULT {
	if out == nil {
		strategyLog("CreateInstance E_POINTER: null out")
		return win32.E_POINTER
	}
	*(*unsafe.Pointer)(out) = nil
	if iid == nil {
		strategyLog("CreateInstance E_POINTER: null iid")
		return win32.E_POINTER
	}
	if outer != nil {
		strategyLog("CreateInstance CLASS_E_NOAGGREGATION: TSF asked for aggregation")
		return win32.CLASS_E_NOAGGREGATION
	}
	obj := com.NewComObj[tipComObj](&tipImpl{})
	hr := obj.QueryInterface(iid, out)
	obj.Release()
	// A wrong IID here is the classic silent failure: TSF drops the text
	// service without ever calling Activate, so nothing else would report it.
	strategyLog("CreateInstance iid=%s out=%p hr=0x%08x", guidText(*iid), *(*unsafe.Pointer)(out), uint32(hr))
	return win32.HRESULT(hr)
}

func (f *classFactoryImpl) LockServer(lock win32.BOOL) win32.HRESULT {
	if lock != 0 {
		f.locks.Add(1)
	} else if f.locks.Load() > 0 {
		f.locks.Add(-1)
	}
	strategyLog("LockServer lock=%d serverLocks=%d", int32(lock), f.locks.Load())
	return win32.S_OK
}

type tipImpl struct {
	com.IUnknownImpl
	threadMgr   unsafe.Pointer
	clientID    uint32
	sinkAdvised bool

	// Key callbacks are serialized: the session id, the pipe handle and the
	// composition are all single-threaded state, and TSF may well deliver keys
	// from the application's thread while another application types. This
	// mirrors callCoreMutex_ in native/TsIme.cpp.
	keyMu sync.Mutex
	// *ITfComposition of the composition on screen, plus the context it lives
	// in. 0 means there is none.
	composition           atomic.Uintptr
	compositionContext    atomic.Uintptr
	compositionText       atomic.Value
	compositionTerminated atomic.Bool
}

func (t *tipImpl) Activate(threadMgr *win32.IUnknown, clientID uint32) win32.HRESULT {
	// Reachability here decides whether any further logging exists at all, so
	// it has to come before the first thing that can fail.
	strategyLog("StrategyTip Activate enter threadMgr=%p clientId=%d", threadMgr, clientID)
	strategyPauseAt("Activate")
	if threadMgr == nil {
		strategyLog("StrategyTip Activate E_POINTER: no ITfThreadMgr")
		return win32.E_POINTER
	}
	obj := t.ComObj.(*tipComObj)
	hr := C.adviseKeySink(unsafe.Pointer(threadMgr), C.DWORD(clientID), unsafe.Pointer(&obj.keySinkComObj))
	if hr < 0 {
		strategyLog("StrategyTip AdviseKeyEventSink failed hr=0x%08x", uint32(hr))
		writeStackDump("AdviseKeyEventSink failed")
		return win32.HRESULT(hr)
	}
	t.threadMgr = unsafe.Pointer(threadMgr)
	t.clientID = clientID
	t.sinkAdvised = true
	strategyLog("StrategyTip key sink advised clientId=%d", clientID)
	generation := pipeGeneration.Add(1)
	stopPipe()
	strategyLog("StrategyTip Activate starting pipe generation=%d session=%d",
		generation, strategySessionID())
	go connectAndProbe(generation)
	return win32.S_OK
}

func (t *tipImpl) Deactivate() win32.HRESULT {
	strategyLog("StrategyTip Deactivate enter sinkAdvised=%v clientId=%d", t.sinkAdvised, t.clientID)
	strategyPauseAt("Deactivate")
	if t.sinkAdvised && t.threadMgr != nil {
		hr := C.unadviseKeySink(t.threadMgr, C.DWORD(t.clientID))
		strategyLog("StrategyTip UnadviseKeyEventSink hr=0x%08x", uint32(hr))
		if hr < 0 {
			writeStackDump("UnadviseKeyEventSink failed")
		}
		t.sinkAdvised = false
		t.threadMgr = nil
	} else {
		// Activation never completed, so there is nothing to unadvise. Saying so
		// keeps a missing UnadviseKeyEventSink from looking like a silent leak.
		strategyLog("StrategyTip Deactivate had no advised sink to retire")
	}
	pipeReady.Store(false)
	// A composition on screen belongs to a document that is going away; taking
	// it down and dropping our reference keeps a dead ITfComposition from being
	// used by a later activation. This is the one place the remembered context
	// is used on its own, and it runs after the sink is retired so no key
	// callback can be in flight any more.
	if t.composition.Load() != 0 && t.compositionContext.Load() != 0 && t.clientID != 0 {
		t.endComposition(uintptr(t.compositionContext.Load()), "")
	}
	t.dropComposition()
	t.compositionContext.Store(0)
	t.compositionText.Store("")
	generation := pipeGeneration.Add(1)
	strategyLog("StrategyTip deactivate generation=%d", generation)
	stopPipe()
	return win32.S_OK
}

type tipVtbl struct {
	win32.IUnknownVtbl
	Activate   uintptr
	Deactivate uintptr
}

type tipComObj struct {
	com.IUnknownComObj
	keySinkComObj keySinkComObj
}

func (t *tipComObj) GetSubComObjs() []com.ComObjInterface {
	return []com.ComObjInterface{&t.keySinkComObj}
}

// keyName mirrors the native TSF implementation's key-name mapping.
func keyName(vk uintptr) string {
	switch vk {
	case 0x08:
		return "Backspace"
	case 0x1B:
		return "Escape"
	case 0x0D:
		return "Enter"
	case 0x20:
		return " "
	case 0x25:
		return "ArrowLeft"
	case 0x26:
		return "ArrowUp"
	case 0x27:
		return "ArrowRight"
	case 0x28:
		return "ArrowDown"
	default:
		if (vk >= '1' && vk <= '9') || (vk >= 'A' && vk <= 'Z') {
			return string(rune(vk))
		}
		return ""
	}
}

func currentModifiers() int {
	modifiers := 0
	for _, item := range []struct {
		vk  uintptr
		bit int
	}{{0x10, 1}, {0x11, 2}, {0x12, 4}} {
		state, _, _ := procGetKeyState.Call(item.vk)
		if int16(state) < 0 {
			modifiers |= item.bit
		}
	}
	return modifiers
}

// winHeld reports whether either Windows key is currently down.
func winHeld() bool {
	for _, vk := range []uintptr{0x5B, 0x5C} {
		if state, _, _ := procGetKeyState.Call(vk); int16(state) < 0 {
			return true
		}
	}
	return false
}

// winSpaceBypass keeps Win+Space out of the IME. Windows owns that combination
// to switch input methods, and once keys are really consumed a live composition
// would swallow it. This mirrors the guard at the top of
// TextService::TestKey and TextService::HandleKey in native/TsIme.cpp.
func winSpaceBypass(vk uintptr) bool {
	return vk == 0x20 && winHeld()
}

// sendKeyEvent forwards a key event that carries no decision. Only keyUp uses
// it: TSF never lets a sink eat a key-up, so there is no consume flag to wait
// for, and the reply that still comes back is logged by drainPipe.
func sendKeyEvent(kind string, vk, lParam uintptr) {
	name := keyName(vk)
	if name == "" {
		return
	}
	pipeMu.Lock()
	defer pipeMu.Unlock()
	if (pipeHandle == syscall.InvalidHandle || !pipeReady.Load()) && !connectLocked() {
		strategyLog("StrategyTip key event dropped type=%s vk=0x%x: pipe not ready", kind, vk)
		return
	}
	id := pipeRequestID.Add(1)
	message := fmt.Sprintf(`{"id":%d,"session":%d,"type":%q,"vk":%d,"scanCode":%d,"key":%q,"modifiers":%d}`,
		id, strategySessionID(), kind, vk, (lParam>>16)&0xff, name, currentModifiers())
	if err := writeLine(pipeHandle, message); err != nil {
		strategyLog("StrategyTip key event write failed type=%s vk=0x%x err=%v", kind, vk, err)
		return
	}
	strategyLog("StrategyTip sent key event id=%d type=%s vk=0x%x key=%q", id, kind, vk, name)
}

type keySinkComObj struct{ com.IUnknownComObj }

func (k *keySinkComObj) IID() *syscall.GUID { return &iidKeyEventSink }
func (k *keySinkComObj) GetVtbl() *win32.IUnknownVtbl {
	return (*win32.IUnknownVtbl)(unsafe.Pointer(k.BuildKeySinkVtbl(true)))
}

var keySinkVTable *closedtsf.ITfKeyEventSinkVtbl

func (k *keySinkComObj) BuildKeySinkVtbl(lock bool) *closedtsf.ITfKeyEventSinkVtbl {
	if lock {
		com.MuVtbl.Lock()
		defer com.MuVtbl.Unlock()
	}
	if keySinkVTable == nil {
		keySinkVTable = (*closedtsf.ITfKeyEventSinkVtbl)(com.Malloc(unsafe.Sizeof(closedtsf.ITfKeyEventSinkVtbl{})))
		base := k.IUnknownComObj.BuildVtbl(false)
		keySinkVTable.IUnknownVtbl = closedtsf.IUnknownVtbl{QueryInterface: base.QueryInterface, AddRef: base.AddRef, Release: base.Release}
		keySinkVTable.OnSetFocus = syscall.NewCallback(keySinkOnSetFocus)
		keySinkVTable.OnTestKeyDown = syscall.NewCallback(keySinkOnTestKeyDown)
		keySinkVTable.OnTestKeyUp = syscall.NewCallback(keySinkOnTestKeyUp)
		keySinkVTable.OnKeyDown = syscall.NewCallback(keySinkOnKeyDown)
		keySinkVTable.OnKeyUp = syscall.NewCallback(keySinkOnKeyUp)
		keySinkVTable.OnPreservedKey = syscall.NewCallback(keySinkOnPreservedKey)
	}
	return keySinkVTable
}

var tipVTable *tipVtbl

func (t *tipComObj) IID() *syscall.GUID { return &iidTextInputProcessor }

func (t *tipComObj) BuildTipVtbl() *tipVtbl {
	com.MuVtbl.Lock()
	defer com.MuVtbl.Unlock()
	if tipVTable == nil {
		v := (*tipVtbl)(com.Malloc(unsafe.Sizeof(tipVtbl{})))
		v.IUnknownVtbl = *t.IUnknownComObj.BuildVtbl(false)
		v.Activate = syscall.NewCallback(tipActivateCallback)
		v.Deactivate = syscall.NewCallback(tipDeactivateCallback)
		tipVTable = v
	}
	return tipVTable
}

func (t *tipComObj) GetVtbl() *win32.IUnknownVtbl {
	return (*win32.IUnknownVtbl)(unsafe.Pointer(t.BuildTipVtbl()))
}

func (t *tipComObj) activateCallback(_ uintptr, threadMgr uintptr, clientID uintptr) uintptr {
	strategyLog("StrategyTip Activate callback entered threadMgr=0x%x clientId=%d", threadMgr, clientID)
	return uintptr(t.Impl().(*tipImpl).Activate((*win32.IUnknown)(unsafe.Pointer(threadMgr)), uint32(clientID)))
}

func (t *tipComObj) deactivateCallback(_ uintptr) uintptr {
	return uintptr(t.Impl().(*tipImpl).Deactivate())
}

//export DllGetClassObject
func DllGetClassObject(clsid unsafe.Pointer, iid unsafe.Pointer, out unsafe.Pointer) C.int32_t {
	return C.int32_t(guardedCall("DllGetClassObject", func() uintptr {
		if out == nil {
			strategyLog("DllGetClassObject E_POINTER: null out")
			return hresultToUintptr(int32(win32.E_POINTER))
		}
		*(*unsafe.Pointer)(out) = nil
		if clsid == nil || iid == nil {
			strategyLog("DllGetClassObject E_POINTER: null clsid or iid")
			return hresultToUintptr(int32(win32.E_POINTER))
		}
		// TSF asks for a class it does not know as part of normal probing, so
		// this path is expected and only worth a trace, not a dump.
		if *(*syscall.GUID)(clsid) != clsidStrategy {
			strategyLog("DllGetClassObject CLASS_E_CLASSNOTAVAILABLE for %s", guidText(*(*syscall.GUID)(clsid)))
			return hresultToUintptr(int32(win32.CLASS_E_CLASSNOTAVAILABLE))
		}
		factory := com.NewComObj[comimpl.IClassFactoryComObj](&classFactoryImpl{})
		hr := factory.QueryInterface((*syscall.GUID)(iid), out)
		factory.Release()
		strategyLog("DllGetClassObject clsid=StrategyTip iid=%s out=%p hr=0x%08x",
			guidText(*(*syscall.GUID)(iid)), *(*unsafe.Pointer)(out), uint32(hr))
		return uintptr(hr)
	}))
}

//export DllCanUnloadNow
func DllCanUnloadNow() C.int32_t {
	return C.int32_t(guardedCall("DllCanUnloadNow", func() uintptr {
		// Always S_FALSE: the Go runtime owns threads and a package-level
		// registry that outlive the last COM reference, so the image must stay
		// mapped for the life of the host process.
		strategyLog("DllCanUnloadNow -> S_FALSE (module stays mapped for this process)")
		return hresultToUintptr(int32(win32.S_FALSE))
	}))
}

//export DllRegisterServer
func DllRegisterServer() C.int32_t {
	return C.int32_t(guardedCall("DllRegisterServer", func() uintptr {
		hr := int32(C.runRegistration(0))
		strategyLog("DllRegisterServer hr=0x%08x", uint32(hr))
		return hresultToUintptr(hr)
	}))
}

//export DllUnregisterServer
func DllUnregisterServer() C.int32_t {
	return C.int32_t(guardedCall("DllUnregisterServer", func() uintptr {
		hr := int32(C.runRegistration(1))
		strategyLog("DllUnregisterServer hr=0x%08x", uint32(hr))
		return hresultToUintptr(hr)
	}))
}

func main() {}

// Use free functions for syscall callbacks: the native COM this pointer is
// an explicit first ABI argument and must not be confused with a receiver.
// Every one of them is wrapped, because a panic crossing this boundary takes
// the host application down instead of returning a failure HRESULT to TSF.
func tipActivateCallback(this uintptr, threadMgr uintptr, clientID uintptr) uintptr {
	return guardedCall("ITfTextInputProcessor.Activate", func() uintptr {
		obj := (*tipComObj)(unsafe.Pointer(this))
		return uintptr(obj.Impl().(*tipImpl).Activate((*win32.IUnknown)(unsafe.Pointer(threadMgr)), uint32(clientID)))
	})
}

func tipDeactivateCallback(this uintptr) uintptr {
	return guardedCall("ITfTextInputProcessor.Deactivate", func() uintptr {
		return uintptr((*tipComObj)(unsafe.Pointer(this)).Impl().(*tipImpl).Deactivate())
	})
}

// eat writes the BOOL TSF reads back through pfEaten and logs what was decided,
// so a key the pipeline could not consume shows up as a decision rather than as
// silence.
func eat(callback string, pfEaten uintptr, eaten bool) {
	if pfEaten != 0 {
		value := int32(0)
		if eaten {
			value = 1
		}
		*(*int32)(unsafe.Pointer(pfEaten)) = value
	}
	strategyLog("TSF %s pfEaten=0x%x -> eaten=%v", callback, pfEaten, eaten)
}

func keySinkOnSetFocus(this uintptr, foreground uintptr) uintptr {
	return guardedCall("ITfKeyEventSink.OnSetFocus", func() uintptr {
		strategyLog("TSF OnSetFocus foreground=%v pid=%d", foreground != 0, os.Getpid())
		return hresultToUintptr(int32(win32.S_OK))
	})
}

func keySinkOnTestKeyDown(this uintptr, _ uintptr, key uintptr, lParam uintptr, eaten uintptr) uintptr {
	return guardedCall("ITfKeyEventSink.OnTestKeyDown", func() uintptr {
		service := (*keySinkComObj)(unsafe.Pointer(this)).Impl().(*tipImpl)
		service.keyMu.Lock()
		defer service.keyMu.Unlock()
		if winSpaceBypass(key) {
			strategyLog("TSF OnTestKeyDown bypass Win+Space vk=0x%x", key)
			eat("OnTestKeyDown", eaten, false)
			return hresultToUintptr(int32(win32.S_OK))
		}
		reply, replied := callCore("testKeyDown", key, lParam)
		eat("OnTestKeyDown", eaten, keyDecision(reply, replied))
		return hresultToUintptr(int32(win32.S_OK))
	})
}

func keySinkOnTestKeyUp(this uintptr, _ uintptr, key uintptr, _ uintptr, eaten uintptr) uintptr {
	return guardedCall("ITfKeyEventSink.OnTestKeyUp", func() uintptr {
		eat("OnTestKeyUp", eaten, false)
		return hresultToUintptr(int32(win32.S_OK))
	})
}

func keySinkOnKeyDown(this uintptr, context uintptr, key uintptr, lParam uintptr, eaten uintptr) uintptr {
	return guardedCall("ITfKeyEventSink.OnKeyDown", func() uintptr {
		service := (*keySinkComObj)(unsafe.Pointer(this)).Impl().(*tipImpl)
		// One key at a time per service: the request id, the pipe handle and the
		// composition are all single-threaded state, and RequestEditSession
		// blocks until the application accepts the edit session.
		service.keyMu.Lock()
		defer service.keyMu.Unlock()
		if winSpaceBypass(key) {
			strategyLog("TSF OnKeyDown bypass Win+Space vk=0x%x", key)
			eat("OnKeyDown", eaten, false)
			return hresultToUintptr(int32(win32.S_OK))
		}
		reply, replied := callCore("keyDown", key, lParam)
		consume := keyDecision(reply, replied)
		// The composition is only written for a key the engine keeps, and only
		// with the context this very callback was handed.
		if context != 0 {
			if consume {
				caret := service.applyComposition(context, reply)
				notifyCandidates(reply, caret)
			} else {
				service.forgetComposition(context)
			}
		}
		eat("OnKeyDown", eaten, consume)
		return hresultToUintptr(int32(win32.S_OK))
	})
}

func keySinkOnKeyUp(this uintptr, _ uintptr, key uintptr, lParam uintptr, eaten uintptr) uintptr {
	return guardedCall("ITfKeyEventSink.OnKeyUp", func() uintptr {
		service := (*keySinkComObj)(unsafe.Pointer(this)).Impl().(*tipImpl)
		service.keyMu.Lock()
		defer service.keyMu.Unlock()
		sendKeyEvent("keyUp", key, lParam)
		eat("OnKeyUp", eaten, false)
		return hresultToUintptr(int32(win32.S_OK))
	})
}

func keySinkOnPreservedKey(this uintptr, _ uintptr, key uintptr, eaten uintptr) uintptr {
	return guardedCall("ITfKeyEventSink.OnPreservedKey", func() uintptr {
		strategyLog("TSF OnPreservedKey vk=0x%x", key)
		eat("OnPreservedKey", eaten, false)
		return hresultToUintptr(int32(win32.S_OK))
	})
}
