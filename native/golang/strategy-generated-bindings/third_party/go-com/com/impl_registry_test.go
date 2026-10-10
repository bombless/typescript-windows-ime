package com

import (
	"syscall"
	"testing"
	"unsafe"

	"github.com/zzl/go-win32api/v2/win32"
)

type registryTestImpl struct{ tag int }

func (*registryTestImpl) QueryInterface(*syscall.GUID, unsafe.Pointer) win32.HRESULT {
	return win32.E_NOINTERFACE
}
func (*registryTestImpl) AddRef() uint32  { return 1 }
func (*registryTestImpl) Release() uint32 { return 0 }

func withEmptyRegistry(t *testing.T, fn func()) {
	t.Helper()
	registryMu.Lock()
	oldImpls, oldFree := impls, freeImplSlots
	impls, freeImplSlots = nil, nil
	registryMu.Unlock()
	t.Cleanup(func() {
		registryMu.Lock()
		impls, freeImplSlots = oldImpls, oldFree
		registryMu.Unlock()
	})
	fn()
}

func TestAddImplReusesFreedSlotsInLIFOOrder(t *testing.T) {
	withEmptyRegistry(t, func() {
		a, b, c := &registryTestImpl{tag: 1}, &registryTestImpl{tag: 2}, &registryTestImpl{tag: 3}
		if got := AddImpl(a); got != 0 {
			t.Fatalf("first slot = %d; want 0", got)
		}
		if got := AddImpl(b); got != 1 {
			t.Fatalf("second slot = %d; want 1", got)
		}

		registryMu.Lock()
		impls[0], impls[1] = nil, nil
		freeImplSlots = append(freeImplSlots, 0, 1)
		registryMu.Unlock()

		if got := AddImpl(c); got != 1 {
			t.Fatalf("first reused slot = %d; want most-recently-freed slot 1", got)
		}
		if got := AddImpl(a); got != 0 {
			t.Fatalf("second reused slot = %d; want slot 0", got)
		}
		registryMu.RLock()
		defer registryMu.RUnlock()
		if impls[0] != a || impls[1] != c {
			t.Fatalf("registry contents after reuse = %#v; want slot0=a, slot1=c", impls)
		}
		if len(freeImplSlots) != 0 {
			t.Fatalf("free slot stack length = %d; want 0", len(freeImplSlots))
		}
	})
}

func TestAddImplRejectsOccupiedFreeSlot(t *testing.T) {
	withEmptyRegistry(t, func() {
		registryMu.Lock()
		impls = []win32.IUnknownInterface{&registryTestImpl{tag: 1}}
		freeImplSlots = []int32{0}
		registryMu.Unlock()

		defer func() {
			if recover() == nil {
				t.Fatal("AddImpl did not panic for an occupied free slot")
			}
		}()
		AddImpl(&registryTestImpl{tag: 2})
	})
}
