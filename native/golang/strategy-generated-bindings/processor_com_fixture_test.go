package tsfdecl

import (
	"sync"
	"syscall"
	"testing"
	"time"
	"unsafe"

	"github.com/zzl/go-com/com"
	"github.com/zzl/go-win32api/v2/win32"
)

// This fixture deliberately proves only a local COM object/vtable bridge. It
// does not register a TIP or claim that TSF activation is ABI-safe.
type processorComObj struct {
	com.IUnknownComObj
}

var processorVtbl = ITfTextInputProcessorVtbl{
	IUnknownVtbl: win32.IUnknownVtbl{
		QueryInterface: syscall.NewCallback((*com.IUnknownComObj).QueryInterface),
		AddRef:         syscall.NewCallback((*com.IUnknownComObj).AddRef),
		Release:        syscall.NewCallback((*com.IUnknownComObj).Release),
	},
	Activate:   syscall.NewCallback(processorActivateCallback),
	Deactivate: syscall.NewCallback(processorDeactivateCallback),
}

func (*processorComObj) GetVtbl() *win32.IUnknownVtbl {
	return (*win32.IUnknownVtbl)(unsafe.Pointer(&processorVtbl))
}

func (*processorComObj) IID() *syscall.GUID { return &IID_ITfTextInputProcessor }

func (p *processorComObj) Finalize() {
	impl := p.Impl().(*processorImpl)
	impl.finalized = true
	if impl.probeLifecycleHooks {
		childImpl := &processorImpl{}
		child := com.NewComObj[processorComObj, *processorComObj](childImpl)
		if got := child.Release(); got != 0 {
			panic("child final Release did not reach zero")
		}
		impl.finalizeHookReentered = true
	}
}

type processorImpl struct {
	com.IUnknownImpl
	activated             bool
	finalized             bool
	activateResult        HRESULT
	deactivateResult      HRESULT
	probeLifecycleHooks   bool
	freeHookReentered     bool
	finalizeHookReentered bool
}

func (p *processorImpl) OnComObjFree() {
	if !p.probeLifecycleHooks {
		return
	}
	childImpl := &processorImpl{}
	child := com.NewComObj[processorComObj, *processorComObj](childImpl)
	if got := child.Release(); got != 0 {
		panic("child final Release did not reach zero")
	}
	p.freeHookReentered = true
}

func (p *processorImpl) Activate(_ *ITfThreadMgr, _ uint32) HRESULT {
	if p.activateResult != win32.S_OK {
		return p.activateResult
	}
	p.activated = true
	return win32.S_OK
}

func (p *processorImpl) Deactivate() HRESULT {
	if p.deactivateResult != win32.S_OK {
		return p.deactivateResult
	}
	p.activated = false
	return win32.S_OK
}

func processorActivateCallback(this unsafe.Pointer, threadMgr unsafe.Pointer, clientID uintptr) uintptr {
	impl := (*com.IUnknownComObj)(this).Impl().(*processorImpl)
	return uintptr(impl.Activate((*ITfThreadMgr)(threadMgr), uint32(clientID)))
}

func processorDeactivateCallback(this unsafe.Pointer) uintptr {
	impl := (*com.IUnknownComObj)(this).Impl().(*processorImpl)
	return uintptr(impl.Deactivate())
}

func TestProcessorComVtableDispatchIdentityAndLifetime(t *testing.T) {
	impl := &processorImpl{}
	obj := com.NewComObj[processorComObj, *processorComObj](impl)
	processor := (*ITfTextInputProcessor)(unsafe.Pointer(obj))

	// NewComObj starts with one owned reference. Every successful QueryInterface
	// must add exactly one reference and Release must return to the prior count.
	if got := obj.AddRef(); got != 2 {
		t.Fatalf("AddRef() = %d; want 2", got)
	}
	if got := obj.Release(); got != 1 {
		t.Fatalf("Release() = %d; want 1", got)
	}

	var queried unsafe.Pointer
	hr := processor.QueryInterface(&IID_ITfTextInputProcessor, unsafe.Pointer(&queried))
	if hr != win32.S_OK || queried != unsafe.Pointer(obj) {
		t.Fatalf("QueryInterface(TIP) = %#x, pointer %p; want S_OK and %p", hr, queried, obj)
	}
	if got := (*IUnknown)(queried).Release(); got != 1 {
		t.Fatalf("Release(TIP query) = %d; want 1", got)
	}

	var unknown unsafe.Pointer
	hr = processor.QueryInterface(&win32.IID_IUnknown, unsafe.Pointer(&unknown))
	if hr != win32.S_OK || unknown != unsafe.Pointer(obj) {
		t.Fatalf("QueryInterface(IUnknown) = %#x, pointer %p; want S_OK and %p", hr, unknown, obj)
	}
	if got := (*IUnknown)(unknown).Release(); got != 1 {
		t.Fatalf("Release(IUnknown query) = %d; want 1", got)
	}

	unsupportedIID := syscall.GUID{Data1: 0xdeadbeef, Data2: 0x1234, Data3: 0x5678, Data4: [8]byte{1, 2, 3, 4, 5, 6, 7, 8}}
	var unsupported unsafe.Pointer
	if hr = processor.QueryInterface(&unsupportedIID, unsafe.Pointer(&unsupported)); hr != win32.E_NOINTERFACE {
		t.Fatalf("QueryInterface(unsupported IID) = %#x; want E_NOINTERFACE", hr)
	}
	if unsupported != nil {
		t.Fatalf("QueryInterface(unsupported IID) returned non-nil pointer %p", unsupported)
	}

	if hr = processor.Activate(nil, 42); hr != win32.S_OK || !impl.activated {
		t.Fatalf("Activate dispatch failed: HRESULT=%#x activated=%v", hr, impl.activated)
	}
	if hr = processor.Deactivate(); hr != win32.S_OK || impl.activated {
		t.Fatalf("Deactivate dispatch failed: HRESULT=%#x activated=%v", hr, impl.activated)
	}

	// HRESULT is a signed 32-bit status code even though the callback bridge
	// returns uintptr. Exercise a failing HRESULT through the actual callback
	// thunk and generated wrapper to catch accidental truncation/sign handling.
	impl.activateResult = win32.E_FAIL
	if hr = processor.Activate(nil, 42); hr != win32.E_FAIL || impl.activated {
		t.Fatalf("Activate error round-trip = %#x activated=%v; want E_FAIL and unchanged state", hr, impl.activated)
	}
	impl.activateResult = win32.S_OK
	if hr = processor.Activate(nil, 42); hr != win32.S_OK || !impl.activated {
		t.Fatalf("Activate after error = %#x activated=%v; want S_OK and active state", hr, impl.activated)
	}
	impl.deactivateResult = win32.E_FAIL
	if hr = processor.Deactivate(); hr != win32.E_FAIL || !impl.activated {
		t.Fatalf("Deactivate error round-trip = %#x activated=%v; want E_FAIL and unchanged state", hr, impl.activated)
	}
	impl.deactivateResult = win32.S_OK
	if hr = processor.Deactivate(); hr != win32.S_OK || impl.activated {
		t.Fatalf("Deactivate after error = %#x activated=%v; want S_OK and inactive state", hr, impl.activated)
	}

	if got := obj.Release(); got != 0 {
		t.Fatalf("final Release() = %d; want 0", got)
	}
	if !impl.finalized {
		t.Fatal("COM implementation Finalize was not called on final Release")
	}
}

func TestConcurrentIndependentComObjectLifecycle(t *testing.T) {
	const workers = 8
	const iterations = 250
	var wg sync.WaitGroup
	errCh := make(chan string, workers)
	for worker := 0; worker < workers; worker++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < iterations; i++ {
				impl := &processorImpl{}
				obj := com.NewComObj[processorComObj, *processorComObj](impl)
				if got := obj.AddRef(); got != 2 {
					errCh <- "AddRef count mismatch"
					return
				}
				if got := obj.Release(); got != 1 {
					errCh <- "intermediate Release count mismatch"
					return
				}
				if got := obj.Release(); got != 0 {
					errCh <- "final Release count mismatch"
					return
				}
				if !impl.finalized {
					errCh <- "Finalize was not called"
					return
				}
			}
		}()
	}
	wg.Wait()
	close(errCh)
	for err := range errCh {
		t.Error(err)
	}
}

func TestLifecycleHooksRunOutsideRegistryLock(t *testing.T) {
	result := make(chan *processorImpl, 1)
	go func() {
		impl := &processorImpl{probeLifecycleHooks: true}
		obj := com.NewComObj[processorComObj, *processorComObj](impl)
		if got := obj.Release(); got != 0 {
			result <- nil
			return
		}
		result <- impl
	}()

	select {
	case impl := <-result:
		if impl == nil {
			t.Fatal("final Release did not reach zero")
		}
		if !impl.freeHookReentered || !impl.finalizeHookReentered {
			t.Fatalf("lifecycle hooks did not both re-enter registry: OnComObjFree=%v Finalize=%v", impl.freeHookReentered, impl.finalizeHookReentered)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("lifecycle hook deadlocked while re-entering COM object creation/release")
	}
}

func TestNativeCCallerInvokesProcessorVtable(t *testing.T) {
	impl := &processorImpl{}
	obj := com.NewComObj[processorComObj, *processorComObj](impl)
	result := invokeNativeVtable(unsafe.Pointer(obj))
	if result.queryInterface != win32.S_OK {
		t.Fatalf("native QueryInterface HRESULT = %#x; want S_OK", result.queryInterface)
	}
	if result.refAfterAdd != 3 || result.refAfterRelease != 2 {
		t.Fatalf("native AddRef/Release counts = %d/%d; want 3/2", result.refAfterAdd, result.refAfterRelease)
	}
	if result.activate != win32.S_OK || result.deactivate != win32.S_OK {
		t.Fatalf("native Activate/Deactivate HRESULTs = %#x/%#x; want S_OK/S_OK", result.activate, result.deactivate)
	}
	if impl.activated {
		t.Fatal("native Deactivate callback did not clear activation state")
	}
	if got := obj.Release(); got != 0 {
		t.Fatalf("final Release after native call = %d; want 0", got)
	}
	if !impl.finalized {
		t.Fatal("native-call object was not finalized")
	}
}
