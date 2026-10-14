//go:build windows

package manualcom

import (
	"encoding/hex"
	"errors"
	"testing"
)

func TestGUIDEncoding(t *testing.T) {
	iidBytes := IIDITfTextInputProcessor.Bytes()
	got := hex.EncodeToString(iidBytes[:])
	if got != "f7e780aa2120d21193e00060b067b86e" {
		t.Fatalf("ITfTextInputProcessor IID bytes = %s", got)
	}
	unknownBytes := IIDIUnknown.Bytes()
	if got := hex.EncodeToString(unknownBytes[:]); got != "0000000000000000c000000000000046" {
		t.Fatalf("IUnknown IID bytes = %s", got)
	}
	factoryBytes := IIDIClassFactory.Bytes()
	if got := hex.EncodeToString(factoryBytes[:]); got != "0100000000000000c000000000000046" {
		t.Fatalf("IClassFactory IID bytes = %s", got)
	}
	clsidBytes := CLSIDGoTip.Bytes()
	got = hex.EncodeToString(clsidBytes[:])
	if got != "621d8c5d739f4c4ba3d21e7b8a9c6401" {
		t.Fatalf("CLSID bytes = %s", got)
	}
}

func TestHRESULTValues(t *testing.T) {
	tests := []struct {
		name string
		got  HRESULT
		want uint32
	}{
		{"S_OK", SOK, 0x00000000},
		{"E_POINTER", EPointer, 0x80004003},
		{"E_NOINTERFACE", ENoInterface, 0x80004002},
		{"CLASS_E_NOAGGREGATION", EClassNoAggregation, 0x80040110},
		{"CLASS_E_CLASSNOTAVAILABLE", EClassNotAvailable, 0x80040111},
		{"E_OUTOFMEMORY", EOutOfMemory, 0x8007000E},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := uint32(tt.got); got != tt.want {
				t.Fatalf("HRESULT = 0x%08X, want 0x%08X", got, tt.want)
			}
		})
	}
}

func TestInterfaceVTableSlots(t *testing.T) {
	if IUnknownVTableSlots != 3 || IClassFactoryVTableSlots != 5 || ITfTextInputProcessorVTableSlots != 5 {
		t.Fatal("unexpected COM vtable slot count")
	}
}

func TestReferenceCountTransitions(t *testing.T) {
	r := NewRefCount()
	if got := r.AddRef(); got != 2 {
		t.Fatalf("AddRef = %d, want 2", got)
	}
	if got, err := r.Release(); err != nil || got != 1 {
		t.Fatalf("Release = %d, %v; want 1, nil", got, err)
	}
	if got, err := r.Release(); err != nil || got != 0 {
		t.Fatalf("final Release = %d, %v; want 0, nil", got, err)
	}
	if _, err := r.Release(); !errors.Is(err, ErrReferenceCountUnderflow) {
		t.Fatalf("underflow error = %v", err)
	}
}

func TestPointerSizeForCurrentTarget(t *testing.T) {
	if got := PointerSize(); got != 8 {
		t.Fatalf("pointer size = %d, current amd64 target expects 8", got)
	}
}
