package main

/*
#include <stdint.h>
#include <stdio.h>
#define DllGetClassObject WindowsSdkDllGetClassObject
#define DllCanUnloadNow WindowsSdkDllCanUnloadNow
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include "registration_txn.h"
#undef DllGetClassObject
#undef DllCanUnloadNow

static void StrategyLogRollbackFailure(const wchar_t* operation, HRESULT originalHr, HRESULT cleanupHr) {
    wchar_t message[256] = {0};
    swprintf(message, ARRAYSIZE(message),
        L"strategy-go-com: rollback operation '%s' failed (original=0x%08lX, cleanup=0x%08lX)\n",
        operation, (unsigned long)originalHr, (unsigned long)cleanupHr);
    OutputDebugStringW(message);
}

static HRESULT StrategyWriteString(HKEY root, const wchar_t* subkey, const wchar_t* valueName, const wchar_t* value) {
    HKEY key = NULL;
    LONG e = RegCreateKeyExW(root, subkey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL);
    if (e != ERROR_SUCCESS) return HRESULT_FROM_WIN32(e);
    DWORD bytes = (DWORD)((wcslen(value) + 1) * sizeof(wchar_t));
    e = RegSetValueExW(key, valueName, 0, REG_SZ, (const BYTE*)value, bytes);
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(e);
}

static HRESULT StrategyCreateProfiles(ITfInputProcessorProfiles** out) {
    *out = NULL;
    return CoCreateInstance(&CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
        &IID_ITfInputProcessorProfiles, (void**)out);
}

static HRESULT StrategyCreateCategories(ITfCategoryMgr** out) {
    *out = NULL;
    return CoCreateInstance(&CLSID_TF_CategoryMgr, NULL, CLSCTX_INPROC_SERVER,
        &IID_ITfCategoryMgr, (void**)out);
}

// S_FALSE from IEnumGUID::Next means the list ended. Any other failure is returned
// so the caller can retry instead of treating a broken enumeration as absence.
static HRESULT StrategyEnumContains(IEnumGUID* enumerator, const GUID* needle, BOOL* found) {
    *found = FALSE;
    if (!enumerator) return E_POINTER;
    for (;;) {
        GUID item;
        ULONG fetched = 0;
        HRESULT hr = enumerator->lpVtbl->Next(enumerator, 1, &item, &fetched);
        if (hr != S_OK || fetched != 1) return FAILED(hr) ? hr : S_OK;
        if (IsEqualGUID(&item, needle)) {
            *found = TRUE;
            return S_OK;
        }
    }
}

static const wchar_t* kStrategyClsidKey = L"SOFTWARE\\Classes\\CLSID\\{A4C7D921-6B35-4E8A-91F2-7D0B6C3E5A14}";
static const wchar_t* kStrategyInprocKey = L"SOFTWARE\\Classes\\CLSID\\{A4C7D921-6B35-4E8A-91F2-7D0B6C3E5A14}\\InprocServer32";

static HRESULT StrategyTxnProbeClass(void* context) {
    HKEY probe = NULL;
    LONG e;
    (void)context;
    e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kStrategyClsidKey, 0, KEY_READ, &probe);
    if (e == ERROR_SUCCESS) { RegCloseKey(probe); return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS); }
    if (e == ERROR_FILE_NOT_FOUND) return S_OK;
    return HRESULT_FROM_WIN32(e);
}
static HRESULT StrategyTxnWriteClassName(void* context) {
    (void)context;
    return StrategyWriteString(HKEY_LOCAL_MACHINE, kStrategyClsidKey, NULL, kStrategyAName);
}
static HRESULT StrategyTxnWriteModulePath(void* context) {
    return StrategyWriteString(HKEY_LOCAL_MACHINE, kStrategyInprocKey, NULL, (const wchar_t*)context);
}
static HRESULT StrategyTxnWriteThreadingModel(void* context) {
    (void)context;
    return StrategyWriteString(HKEY_LOCAL_MACHINE, kStrategyInprocKey, L"ThreadingModel", L"Apartment");
}
static HRESULT StrategyTxnRegisterProfile(void* context) {
    ITfInputProcessorProfiles* profiles = NULL;
    HRESULT hr;
    (void)context;
    hr = StrategyCreateProfiles(&profiles);
    if (FAILED(hr)) return hr;
    hr = profiles->lpVtbl->Register(profiles, &kStrategyAClsid);
    profiles->lpVtbl->Release(profiles);
    return hr;
}
static HRESULT StrategyTxnAddLanguage(void* context, LANGID lang) {
    ITfInputProcessorProfiles* profiles = NULL;
    HRESULT hr;
    (void)context;
    hr = StrategyCreateProfiles(&profiles);
    if (FAILED(hr)) return hr;
    hr = profiles->lpVtbl->AddLanguageProfile(profiles, &kStrategyAClsid, lang, &kStrategyAProfile,
        kStrategyAName, (ULONG)(sizeof(kStrategyAName) / sizeof(kStrategyAName[0]) - 1), NULL, 0, 0);
    profiles->lpVtbl->Release(profiles);
    return hr;
}
static HRESULT StrategyTxnEnableLanguage(void* context, LANGID lang) {
    ITfInputProcessorProfiles* profiles = NULL;
    HRESULT hr;
    (void)context;
    hr = StrategyCreateProfiles(&profiles);
    if (FAILED(hr)) return hr;
    hr = profiles->lpVtbl->EnableLanguageProfile(profiles, &kStrategyAClsid, lang, &kStrategyAProfile, TRUE);
    profiles->lpVtbl->Release(profiles);
    return hr;
}
static HRESULT StrategyTxnRegisterCategory(void* context) {
    ITfCategoryMgr* categories = NULL;
    HRESULT hr;
    (void)context;
    hr = StrategyCreateCategories(&categories);
    if (FAILED(hr)) return hr;
    hr = categories->lpVtbl->RegisterCategory(categories, &kStrategyAClsid, &GUID_TFCAT_TIP_KEYBOARD, &kStrategyAClsid);
    categories->lpVtbl->Release(categories);
    return hr;
}
// Absence is success: enumerate first and skip the mutating call when this
// strategy's CLSID is not present. Unrelated category items are never removed.
static HRESULT StrategyTxnUnregisterCategory(void* context) {
    ITfCategoryMgr* categories = NULL;
    IEnumGUID* enumerator = NULL;
    BOOL found = FALSE;
    HRESULT hr;
    (void)context;
    hr = StrategyCreateCategories(&categories);
    if (FAILED(hr)) return hr;
    hr = categories->lpVtbl->EnumItemsInCategory(categories, &GUID_TFCAT_TIP_KEYBOARD, &enumerator);
    if (SUCCEEDED(hr) && enumerator) hr = StrategyEnumContains(enumerator, &kStrategyAClsid, &found);
    if (enumerator) enumerator->lpVtbl->Release(enumerator);
    if (SUCCEEDED(hr) && found) {
        hr = categories->lpVtbl->UnregisterCategory(categories, &kStrategyAClsid, &GUID_TFCAT_TIP_KEYBOARD, &kStrategyAClsid);
    }
    categories->lpVtbl->Release(categories);
    return hr;
}
static HRESULT StrategyTxnUnregisterProfiles(void* context) {
    ITfInputProcessorProfiles* profiles = NULL;
    IEnumGUID* enumerator = NULL;
    BOOL found = FALSE;
    HRESULT hr;
    (void)context;
    hr = StrategyCreateProfiles(&profiles);
    if (FAILED(hr)) return hr;
    hr = profiles->lpVtbl->EnumInputProcessorInfo(profiles, &enumerator);
    if (SUCCEEDED(hr) && enumerator) hr = StrategyEnumContains(enumerator, &kStrategyAClsid, &found);
    if (enumerator) enumerator->lpVtbl->Release(enumerator);
    if (SUCCEEDED(hr) && found) hr = profiles->lpVtbl->Unregister(profiles, &kStrategyAClsid);
    profiles->lpVtbl->Release(profiles);
    return hr;
}
static HRESULT StrategyTxnDeleteClass(void* context) {
    LONG e;
    (void)context;
    e = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kStrategyClsidKey);
    if (e == ERROR_SUCCESS || e == ERROR_FILE_NOT_FOUND) return S_OK;
    return HRESULT_FROM_WIN32(e);
}
static void StrategyTxnReportCleanupFailure(void* context, const wchar_t* operation, HRESULT originalHr, HRESULT cleanupHr) {
    (void)context;
    StrategyLogRollbackFailure(operation, originalHr, cleanupHr);
}

static const StrategyRegistrationOps kStrategyOps = {
    StrategyTxnProbeClass,
    StrategyTxnWriteClassName,
    StrategyTxnWriteModulePath,
    StrategyTxnWriteThreadingModel,
    StrategyTxnRegisterProfile,
    StrategyTxnAddLanguage,
    StrategyTxnEnableLanguage,
    StrategyTxnRegisterCategory,
    StrategyTxnUnregisterCategory,
    StrategyTxnUnregisterProfiles,
    StrategyTxnDeleteClass,
    StrategyTxnReportCleanupFailure
};

static HRESULT StrategyRegisterAll(void) {
    wchar_t modulePath[MAX_PATH] = {0};
    HMODULE module = NULL;
    DWORD n;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(uintptr_t)&StrategyRegisterAll, &module)) return HRESULT_FROM_WIN32(GetLastError());
    n = GetModuleFileNameW(module, modulePath, ARRAYSIZE(modulePath));
    if (n == 0) return HRESULT_FROM_WIN32(GetLastError());
    return StrategyRunPreparedRegistration(modulePath, ARRAYSIZE(modulePath), n, &kStrategyOps, modulePath);
}

static HRESULT StrategyUnregisterAll(void) {
    return StrategyRunUnregisterTransaction(&kStrategyOps, NULL);
}

static HRESULT StrategyRunRegistration(BOOL unregister) {
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return init;
    HRESULT hr = unregister ? StrategyUnregisterAll() : StrategyRegisterAll();
    if (SUCCEEDED(init)) CoUninitialize();
    return hr;
}
*/
import "C"

import (
	"sync/atomic"
	"syscall"
	"unsafe"

	"github.com/zzl/go-com/com"
	"github.com/zzl/go-com/com/comimpl"
	"github.com/zzl/go-win32api/v2/win32"
)

// Strategy A uses a fresh COM identity, separate from the existing Go TIP.
var clsidGoTip = syscall.GUID{Data1: 0xa4c7d921, Data2: 0x6b35, Data3: 0x4e8a, Data4: [8]byte{0x91, 0xf2, 0x7d, 0x0b, 0x6c, 0x3e, 0x5a, 0x14}}
var iidTextInputProcessor = syscall.GUID{Data1: 0xaa80e7f7, Data2: 0x2021, Data3: 0x11d2, Data4: [8]byte{0x93, 0xe0, 0x00, 0x60, 0xb0, 0x67, 0xb8, 0x6e}}

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

func (t *tipImpl) QueryInterface(iid *syscall.GUID, out unsafe.Pointer) win32.HRESULT {
	if out == nil {
		return win32.E_POINTER
	}
	*(*unsafe.Pointer)(out) = nil
	if iid == nil {
		return win32.E_POINTER
	}
	// Delegate IID matching and interface-pointer assignment to go-com. This
	// keeps QueryInterface consistent with the object's IID()/AssignPpvObject
	// contract instead of manually returning the IUnknown subobject pointer.
	return t.IUnknownImpl.QueryInterface(iid, out)
}

func (t *tipImpl) Activate(_ *win32.IUnknown, _ uint32) win32.HRESULT { return win32.S_OK }
func (t *tipImpl) Deactivate() win32.HRESULT                          { return win32.S_OK }

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
	if *(*syscall.GUID)(clsid) != clsidGoTip {
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
func DllRegisterServer() C.int32_t { return C.int32_t(C.StrategyRunRegistration(0)) }

//export DllUnregisterServer
func DllUnregisterServer() C.int32_t { return C.int32_t(C.StrategyRunRegistration(1)) }

func main() {}
