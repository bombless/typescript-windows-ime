//go:build windows

package main

/*
#include <stdint.h>
*/
import "C"

import (
	"runtime"
	"syscall"
)

func callback(value uintptr) uintptr { return value + 0x1234 }

// Create one fixed callback thunk for the lifetime of the host process.
// syscall.NewCallback documents that this thunk memory is never released.
var callbackThunk = syscall.NewCallback(callback)

//export DllCanUnloadNow
func DllCanUnloadNow() C.int32_t {
	// S_FALSE: deliberately pin this Go runtime-bearing DLL for process life.
	// This is a lifetime experiment, not a complete COM server.
	return 1
}

//export ManualComGetCallback
func ManualComGetCallback() C.uintptr_t { return C.uintptr_t(callbackThunk) }

//export ManualComForceGC
func ManualComForceGC() { runtime.GC() }

func main() {}
