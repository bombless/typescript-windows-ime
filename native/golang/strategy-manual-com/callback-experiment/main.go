//go:build windows

package main

/*
#include <stdint.h>
uintptr_t invoke_callback(uintptr_t callback, uintptr_t value);
typedef struct IUnknownLike IUnknownLike;
IUnknownLike *create_unknown_like(uintptr_t query_interface, uintptr_t add_ref, uintptr_t release);
uintptr_t call_query_interface(IUnknownLike *obj, const unsigned char iid[16], uintptr_t *out);
uintptr_t call_add_ref(IUnknownLike *obj);
uintptr_t call_release(IUnknownLike *obj);
long object_refcount(IUnknownLike *obj);
long object_add_ref(IUnknownLike *obj);
long object_release_ref(IUnknownLike *obj);
void destroy_unknown_like(IUnknownLike *obj);
void copy_iid_iunknown(unsigned char out[16]);
int iid_pointer_is_iunknown(uintptr_t iid);
void write_pointer(uintptr_t address, uintptr_t value);
long object_add_ref_from_uintptr(uintptr_t obj);
long object_release_ref_from_uintptr(uintptr_t obj);
int stress_callback_concurrency(uintptr_t callback, int thread_count, int iterations);
*/
import "C"

import (
	"fmt"
	"runtime"
	"sync/atomic"
	"syscall"
	"unsafe"
)

var calls atomic.Uint64
var vtableCalls atomic.Uint64
var reentrantCallback uintptr

func callback(value uintptr) uintptr {
	calls.Add(1)
	return value + 0x1234
}

func reentrantCallbackFn(value uintptr) uintptr {
	if value == 0 {
		return 0
	}
	inner := C.invoke_callback(C.uintptr_t(reentrantCallback), C.uintptr_t(value-1))
	return uintptr(inner) + 1
}

func queryInterface(this, iidPtr, outPtr uintptr) uintptr {
	vtableCalls.Add(1)
	if outPtr == 0 {
		return uintptr(0x80004003) // E_POINTER
	}
	C.write_pointer(C.uintptr_t(outPtr), 0)
	if iidPtr == 0 {
		return uintptr(0x80004003) // E_POINTER
	}
	if C.iid_pointer_is_iunknown(C.uintptr_t(iidPtr)) == 0 {
		return uintptr(0x80004002) // E_NOINTERFACE
	}
	C.object_add_ref_from_uintptr(C.uintptr_t(this))
	C.write_pointer(C.uintptr_t(outPtr), C.uintptr_t(this))
	return 0
}

func addRef(this uintptr) uintptr {
	vtableCalls.Add(1)
	return uintptr(C.object_add_ref_from_uintptr(C.uintptr_t(this)))
}

func release(this uintptr) uintptr {
	vtableCalls.Add(1)
	return uintptr(C.object_release_ref_from_uintptr(C.uintptr_t(this)))
}

func main() {
	cb := syscall.NewCallback(callback)
	for i := uintptr(0); i < 10000; i++ {
		got := C.invoke_callback(C.uintptr_t(cb), C.uintptr_t(i))
		want := i + 0x1234
		if uintptr(got) != want {
			panic(fmt.Sprintf("callback result mismatch at %d: got %#x want %#x", i, uintptr(got), want))
		}
	}
	runtime.GC()
	got := C.invoke_callback(C.uintptr_t(cb), C.uintptr_t(7))
	if uintptr(got) != 7+0x1234 || calls.Load() != 10001 {
		panic(fmt.Sprintf("post-GC callback mismatch: result=%#x calls=%d", uintptr(got), calls.Load()))
	}
	if failures := int(C.stress_callback_concurrency(C.uintptr_t(cb), 8, 10000)); failures != 0 {
		panic(fmt.Sprintf("concurrent callback stress failed: %d incorrect results", failures))
	}
	if calls.Load() != 90001 {
		panic(fmt.Sprintf("concurrent callback count mismatch: got=%d want=90001", calls.Load()))
	}
	reentrantCallback = syscall.NewCallback(reentrantCallbackFn)
	if got := uintptr(C.invoke_callback(C.uintptr_t(reentrantCallback), 64)); got != 64 {
		panic(fmt.Sprintf("same-thread reentrant callback mismatch: got=%d want=64", got))
	}

	obj := C.create_unknown_like(C.uintptr_t(syscall.NewCallback(queryInterface)), C.uintptr_t(syscall.NewCallback(addRef)), C.uintptr_t(syscall.NewCallback(release)))
	if obj == nil {
		panic("native IUnknown-like allocation failed")
	}
	defer C.destroy_unknown_like(obj)

	var iid [16]C.uchar
	C.copy_iid_iunknown(&iid[0])
	var out C.uintptr_t
	if hr := uintptr(C.call_query_interface(obj, &iid[0], &out)); hr != 0 || uintptr(out) != uintptr(unsafe.Pointer(obj)) || C.object_refcount(obj) != 2 {
		panic(fmt.Sprintf("IUnknown QueryInterface failed: hr=%#x out=%#x refs=%d", hr, uintptr(out), int(C.object_refcount(obj))))
	}
	if got := uintptr(C.call_add_ref(obj)); got != 3 || C.object_refcount(obj) != 3 {
		panic(fmt.Sprintf("vtable AddRef failed: return=%d refs=%d", got, int(C.object_refcount(obj))))
	}
	if got := uintptr(C.call_release(obj)); got != 2 || C.object_refcount(obj) != 2 {
		panic(fmt.Sprintf("vtable Release failed: return=%d refs=%d", got, int(C.object_refcount(obj))))
	}

	var unsupported [16]C.uchar
	unsupported[0] = 0xFE
	out = 0xBAD
	if hr := uintptr(C.call_query_interface(obj, &unsupported[0], &out)); hr != uintptr(0x80004002) || out != 0 {
		panic(fmt.Sprintf("unsupported QueryInterface must clear output: hr=%#x out=%#x", hr, uintptr(out)))
	}
	if hr := uintptr(C.call_query_interface(obj, &iid[0], nil)); hr != uintptr(0x80004003) {
		panic(fmt.Sprintf("null QueryInterface output should return E_POINTER, got %#x", hr))
	}
	if got := uintptr(C.call_release(obj)); got != 1 || C.object_refcount(obj) != 1 {
		panic(fmt.Sprintf("final harness release failed: return=%d refs=%d", got, int(C.object_refcount(obj))))
	}
	runtime.GC()
	if hr := uintptr(C.call_query_interface(obj, &iid[0], &out)); hr != 0 || C.object_refcount(obj) != 2 {
		panic(fmt.Sprintf("post-GC vtable dispatch failed: hr=%#x refs=%d", hr, int(C.object_refcount(obj))))
	}
	fmt.Printf("PASS: scalar callback=90001 calls including 8x10000 concurrent calls; COM-shaped IUnknown vtable checks passed; vtable-callbacks=%d; refs=%d; pointer-size=%d\n", vtableCalls.Load(), int(C.object_refcount(obj)), unsafe.Sizeof(uintptr(0)))
}
