//go:build windows

// Package manualcom records the deliberately small ABI surface for the first
// TSF activation experiment. GUIDs and interface ordering are sourced from
// the Windows SDK (um/unknwn.h and um/msctf.h); this package is not yet a DLL.
package manualcom

import (
	"encoding/binary"
	"errors"
	"sync/atomic"
	"unsafe"
)

type HRESULT int32

const (
	SOK                 HRESULT = 0
	EPointer            HRESULT = -2147467261 // 0x80004003
	ENoInterface        HRESULT = -2147467262 // 0x80004002
	EClassNoAggregation HRESULT = -2147221232 // 0x80040110
	EClassNotAvailable  HRESULT = -2147221231 // 0x80040111
	EOutOfMemory        HRESULT = -2147024882 // 0x8007000E
)

type GUID struct {
	Data1 uint32
	Data2 uint16
	Data3 uint16
	Data4 [8]byte
}

var (
	IIDIUnknown              = GUID{Data1: 0x00000000, Data2: 0x0000, Data3: 0x0000, Data4: [8]byte{0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}
	IIDIClassFactory         = GUID{Data1: 0x00000001, Data2: 0x0000, Data3: 0x0000, Data4: [8]byte{0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}
	IIDITfTextInputProcessor = GUID{Data1: 0xAA80E7F7, Data2: 0x2021, Data3: 0x11D2, Data4: [8]byte{0x93, 0xE0, 0x00, 0x60, 0xB0, 0x67, 0xB8, 0x6E}}
	CLSIDGoTip               = GUID{Data1: 0x5D8C1D62, Data2: 0x9F73, Data3: 0x4B4C, Data4: [8]byte{0xA3, 0xD2, 0x1E, 0x7B, 0x8A, 0x9C, 0x64, 0x01}}
)

// IUnknown has exactly three vtable entries; IClassFactory appends two.
// ITfTextInputProcessor appends Activate and Deactivate after IUnknown.
const (
	IUnknownVTableSlots              = 3
	IClassFactoryVTableSlots         = 5
	ITfTextInputProcessorVTableSlots = 5
)

var ErrReferenceCountUnderflow = errors.New("COM reference count underflow")

// RefCount models the atomic lifetime invariant independently of callback ABI.
// The eventual COM object must arrange destruction when Release returns zero.
type RefCount struct{ value atomic.Int32 }

func NewRefCount() *RefCount       { r := &RefCount{}; r.value.Store(1); return r }
func (r *RefCount) AddRef() uint32 { return uint32(r.value.Add(1)) }
func (r *RefCount) Release() (uint32, error) {
	n := r.value.Add(-1)
	if n < 0 {
		r.value.Add(1)
		return uint32(max(n+1, 0)), ErrReferenceCountUnderflow
	}
	return uint32(n), nil
}

func (g GUID) Bytes() [16]byte {
	var b [16]byte
	binary.LittleEndian.PutUint32(b[0:4], g.Data1)
	binary.LittleEndian.PutUint16(b[4:6], g.Data2)
	binary.LittleEndian.PutUint16(b[6:8], g.Data3)
	copy(b[8:], g.Data4[:])
	return b
}

// PointerSize is intentionally runtime-derived; the current machine target is
// amd64. Separate 386 builds and in-host TSF tests remain required before support.
func PointerSize() uintptr { return unsafe.Sizeof(uintptr(0)) }
