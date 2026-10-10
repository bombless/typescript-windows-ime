package tsfdecl

import (
	"reflect"
	"syscall"
	"testing"
	"unsafe"

	"github.com/zzl/go-win32api/v2/win32"
)

func TestGeneratedProcessorDeclarationShape(t *testing.T) {
	if reflect.TypeOf(ITfThreadMgr{}).Name() == reflect.TypeOf(IUnknown{}).Name() {
		t.Fatal("ITfThreadMgr must remain a distinct opaque type, not an IUnknown alias")
	}
	if IID_ITfTextInputProcessor.Data1 != 0xAA80E7F7 {
		t.Fatalf("unexpected ITfTextInputProcessor IID: %#v", IID_ITfTextInputProcessor)
	}
	var vtbl ITfTextInputProcessorVtbl
	if unsafe.Sizeof(vtbl) != unsafe.Sizeof(win32.IUnknownVtbl{})+2*unsafe.Sizeof(uintptr(0)) {
		t.Fatalf("unexpected vtable size: %d", unsafe.Sizeof(vtbl))
	}
	// COM methods append after the three IUnknown slots. Check field offsets
	// as well as total size on each target architecture.
	if unsafe.Offsetof(vtbl.Activate) != 3*unsafe.Sizeof(uintptr(0)) {
		t.Fatalf("Activate slot offset = %d; want %d", unsafe.Offsetof(vtbl.Activate), 3*unsafe.Sizeof(uintptr(0)))
	}
	if unsafe.Offsetof(vtbl.Deactivate) != 4*unsafe.Sizeof(uintptr(0)) {
		t.Fatalf("Deactivate slot offset = %d; want %d", unsafe.Offsetof(vtbl.Deactivate), 4*unsafe.Sizeof(uintptr(0)))
	}
	if unsafe.Sizeof(HRESULT(0)) != 4 {
		t.Fatalf("HRESULT size = %d; want 4 bytes", unsafe.Sizeof(HRESULT(0)))
	}
	if unsafe.Sizeof(uint32(0)) != 4 {
		t.Fatal("TfClientId/DWORD representation must be 4 bytes")
	}
	var _ ITfTextInputProcessorInterface = (*compileOnlyProcessor)(nil)
}

type compileOnlyProcessor struct{}

func (*compileOnlyProcessor) QueryInterface(*syscall.GUID, unsafe.Pointer) HRESULT {
	return win32.E_NOTIMPL
}
func (*compileOnlyProcessor) AddRef() uint32                         { return 1 }
func (*compileOnlyProcessor) Release() uint32                        { return 1 }
func (*compileOnlyProcessor) Activate(*ITfThreadMgr, uint32) HRESULT { return win32.E_NOTIMPL }
func (*compileOnlyProcessor) Deactivate() HRESULT                    { return win32.E_NOTIMPL }
