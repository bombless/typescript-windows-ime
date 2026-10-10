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
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"github.com/zzl/go-com/com"
	"github.com/zzl/go-com/com/comimpl"
	"github.com/zzl/go-win32api/v2/win32"
)

// The same pipe the C++ text service uses. The host accepts every TSF client at
// once, so switching to this input method shares the running host.
const (
	tsfPipeName     = `\\.\pipe\TypeScriptWindowsIME.Tsf`
	pipeDialTimeout = 250 * time.Millisecond
	pipeReadWaitMs  = 200
)

const errorPipeBusy = syscall.Errno(231)

var errPipeStall = errors.New("timed out")

var (
	kernel32                    = syscall.NewLazyDLL("kernel32.dll")
	procCreateEventW            = kernel32.NewProc("CreateEventW")
	procGetCurrentThreadId      = kernel32.NewProc("GetCurrentThreadId")
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

func strategyLog(format string, args ...any) {
	message := fmt.Sprintf(format, args...)
	line := fmt.Sprintf("[%s][pid=%d tid=%d] %s\r\n",
		time.Now().Format("2006-01-02 15:04:05.000"),
		os.Getpid(), threadID(), message)
	path, err := syscall.UTF16PtrFromString(filepath.Join(os.TempDir(), "TypeScriptWindowsIME.log"))
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
		_ = syscall.CancelIoEx(handle, overlapped)
		_, _ = overlappedResult(handle, overlapped)
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

func readSome(handle syscall.Handle, buffer []byte, timeoutMs uint32) (int, error) {
	event, err := createEvent()
	if err != nil {
		return 0, err
	}
	defer func() { _ = syscall.CloseHandle(event) }()
	overlapped := syscall.Overlapped{HEvent: event}
	var done uint32
	err = syscall.ReadFile(handle, buffer, &done, &overlapped)
	if err == syscall.ERROR_IO_PENDING {
		done, err = waitIO(handle, &overlapped, event, timeoutMs)
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
	return true
}

func releaseIfOwner(handle syscall.Handle) {
	pipeMu.Lock()
	if pipeHandle == handle {
		pipeHandle = syscall.InvalidHandle
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

func drainPipe(generation uint64, handle syscall.Handle) {
	defer func() {
		releaseIfOwner(handle)
		strategyLog("StrategyTip pipe closed generation=%d", generation)
	}()
	buffer := make([]byte, 512)
	var pending []byte
	for pipeGeneration.Load() == generation {
		count, err := readSome(handle, buffer, pipeReadWaitMs)
		if pipeGeneration.Load() != generation {
			return
		}
		if errors.Is(err, errPipeStall) {
			continue
		}
		if err != nil || count == 0 {
			strategyLog("StrategyTip read ended generation=%d win32=%v", generation, err)
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
			if len(text) > 200 {
				text = text[:200]
			}
			strategyLog("StrategyTip reply bytes=%d text=%s", len(text), text)
		}
	}
}

func connectAndProbe(generation uint64) {
	session := strategySessionID()
	strategyLog("StrategyTip connect start generation=%d session=%d", generation, session)
	handle, err := dialTsf(pipeDialTimeout)
	if err != nil {
		strategyLog("StrategyTip connect failed generation=%d win32=%v", generation, err)
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
	drainPipe(generation, handle)
}

// {B18F4C27-9A63-4E15-8D70-2F6C1A5B9E34}
var clsidStrategy = syscall.GUID{Data1: 0xB18F4C27, Data2: 0x9A63, Data3: 0x4E15, Data4: [8]byte{0x8D, 0x70, 0x2F, 0x6C, 0x1A, 0x5B, 0x9E, 0x34}}

var iidTextInputProcessor = syscall.GUID{Data1: 0xAA80E7F7, Data2: 0x2021, Data3: 0x11D2, Data4: [8]byte{0x93, 0xE0, 0x00, 0x60, 0xB0, 0x67, 0xB8, 0x6E}}

type classFactoryImpl struct {
	comimpl.IClassFactoryImpl
	locks atomic.Int32
}

func (f *classFactoryImpl) CreateInstance(outer *win32.IUnknown, iid *syscall.GUID, out unsafe.Pointer) win32.HRESULT {
	if out == nil {
		return win32.E_POINTER
	}
	*(*unsafe.Pointer)(out) = nil
	if iid == nil {
		return win32.E_POINTER
	}
	if outer != nil {
		return win32.CLASS_E_NOAGGREGATION
	}
	obj := com.NewComObj[tipComObj](&tipImpl{})
	hr := obj.QueryInterface(iid, out)
	obj.Release()
	return win32.HRESULT(hr)
}

func (f *classFactoryImpl) LockServer(lock win32.BOOL) win32.HRESULT {
	if lock != 0 {
		f.locks.Add(1)
	} else if f.locks.Load() > 0 {
		f.locks.Add(-1)
	}
	return win32.S_OK
}

type tipImpl struct{ com.IUnknownImpl }

func (t *tipImpl) Activate(_ *win32.IUnknown, _ uint32) win32.HRESULT {
	generation := pipeGeneration.Add(1)
	stopPipe()
	strategyLog("StrategyTip activate generation=%d session=%d", generation, strategySessionID())
	go connectAndProbe(generation)
	return win32.S_OK
}

func (t *tipImpl) Deactivate() win32.HRESULT {
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

type tipComObj struct{ com.IUnknownComObj }

var tipVTable *tipVtbl

func (t *tipComObj) IID() *syscall.GUID { return &iidTextInputProcessor }

func (t *tipComObj) BuildTipVtbl() *tipVtbl {
	com.MuVtbl.Lock()
	defer com.MuVtbl.Unlock()
	if tipVTable == nil {
		v := (*tipVtbl)(com.Malloc(unsafe.Sizeof(tipVtbl{})))
		v.IUnknownVtbl = *t.IUnknownComObj.BuildVtbl(false)
		v.Activate = syscall.NewCallback((*tipComObj).activateCallback)
		v.Deactivate = syscall.NewCallback((*tipComObj).deactivateCallback)
		tipVTable = v
	}
	return tipVTable
}

func (t *tipComObj) GetVtbl() *win32.IUnknownVtbl {
	return (*win32.IUnknownVtbl)(unsafe.Pointer(t.BuildTipVtbl()))
}

func (t *tipComObj) activateCallback(_ uintptr, _ uintptr, _ uintptr) uintptr {
	return uintptr(t.Impl().(*tipImpl).Activate(nil, 0))
}

func (t *tipComObj) deactivateCallback(_ uintptr) uintptr {
	return uintptr(t.Impl().(*tipImpl).Deactivate())
}

//export DllGetClassObject
func DllGetClassObject(clsid unsafe.Pointer, iid unsafe.Pointer, out unsafe.Pointer) C.int32_t {
	if out == nil {
		return C.int32_t(win32.E_POINTER)
	}
	*(*unsafe.Pointer)(out) = nil
	if clsid == nil || iid == nil {
		return C.int32_t(win32.E_POINTER)
	}
	if *(*syscall.GUID)(clsid) != clsidStrategy {
		return C.int32_t(win32.CLASS_E_CLASSNOTAVAILABLE)
	}
	factory := com.NewComObj[comimpl.IClassFactoryComObj](&classFactoryImpl{})
	hr := factory.QueryInterface((*syscall.GUID)(iid), out)
	factory.Release()
	return C.int32_t(win32.HRESULT(hr))
}

//export DllCanUnloadNow
func DllCanUnloadNow() C.int32_t { return C.int32_t(win32.S_FALSE) }

//export DllRegisterServer
func DllRegisterServer() C.int32_t { return C.int32_t(C.runRegistration(0)) }

//export DllUnregisterServer
func DllUnregisterServer() C.int32_t { return C.int32_t(C.runRegistration(1)) }

func main() {}
