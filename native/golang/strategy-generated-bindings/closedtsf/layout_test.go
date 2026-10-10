package closedtsf

import (
	"reflect"
	"syscall"
	"testing"
	"unsafe"
)

func TestAmd64ScalarAndAliasWidths(t *testing.T) {
	if unsafe.Sizeof(uintptr(0)) != 8 {
		t.Fatalf("amd64 pointer width = %d", unsafe.Sizeof(uintptr(0)))
	}
	checks := []struct {
		name string
		size uintptr
		want uintptr
	}{
		{"BOOL", unsafe.Sizeof(BOOL(0)), 4},
		{"HRESULT", unsafe.Sizeof(HRESULT(0)), 4},
		{"TfClientId/DWORD", unsafe.Sizeof(uint32(0)), 4},
		{"TF_PRESERVEDKEY", unsafe.Sizeof(TF_PRESERVEDKEY{}), 8},
		{"HWND", unsafe.Sizeof(HWND(0)), 8},
		{"WPARAM", unsafe.Sizeof(WPARAM(0)), 8},
		{"LPARAM", unsafe.Sizeof(LPARAM(0)), 8},
		{"PWSTR", unsafe.Sizeof(PWSTR(nil)), 8},
		{"BSTR", unsafe.Sizeof(BSTR(nil)), 8},
		{"ITfThreadMgr pointer", unsafe.Sizeof((*ITfThreadMgr)(nil)), 8},
	}
	for _, check := range checks {
		if check.size != check.want {
			t.Errorf("%s size = %d; want %d", check.name, check.size, check.want)
		}
	}
}

func TestIUnknownAndInterfaceVtableOrder(t *testing.T) {
	unknown := reflect.TypeOf(IUnknownVtbl{})
	wantUnknown := []string{"QueryInterface", "AddRef", "Release"}
	if unknown.NumField() != len(wantUnknown) {
		t.Fatalf("IUnknownVtbl fields = %d", unknown.NumField())
	}
	for i, name := range wantUnknown {
		field := unknown.Field(i)
		if field.Name != name || field.Type.Kind() != reflect.Uintptr {
			t.Fatalf("IUnknown slot %d = %s %s", i, field.Name, field.Type)
		}
	}
	cases := []struct {
		name    string
		value   any
		methods []string
	}{
		{"ITfThreadMgrVtbl", ITfThreadMgrVtbl{}, []string{"Activate", "Deactivate", "CreateDocumentMgr", "EnumDocumentMgrs", "GetFocus", "SetFocus", "AssociateFocus", "IsThreadFocus", "GetFunctionProvider", "EnumFunctionProviders", "GetGlobalCompartment"}},
		{"ITfKeystrokeMgrVtbl", ITfKeystrokeMgrVtbl{}, []string{"AdviseKeyEventSink", "UnadviseKeyEventSink", "GetForeground", "TestKeyDown", "TestKeyUp", "KeyDown", "KeyUp", "GetPreservedKey", "IsPreservedKey", "PreserveKey", "UnpreserveKey", "SetPreservedKeyDescription", "GetPreservedKeyDescription", "SimulatePreservedKey"}},
		{"ITfKeyEventSinkVtbl", ITfKeyEventSinkVtbl{}, []string{"OnSetFocus", "OnTestKeyDown", "OnTestKeyUp", "OnKeyDown", "OnKeyUp", "OnPreservedKey"}},
		{"ITfEditSessionVtbl", ITfEditSessionVtbl{}, []string{"DoEditSession"}},
		{"ITfTextInputProcessorVtbl", ITfTextInputProcessorVtbl{}, []string{"Activate", "Deactivate"}},
	}
	pointer := unsafe.Sizeof(uintptr(0))
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			typ := reflect.TypeOf(tc.value)
			if typ.NumField() != len(tc.methods)+1 {
				t.Fatalf("fields = %d; want %d", typ.NumField(), len(tc.methods)+1)
			}
			base := typ.Field(0)
			if !base.Anonymous || base.Name != "IUnknownVtbl" || base.Offset != 0 {
				t.Fatalf("first field = %+v", base)
			}
			for i, name := range tc.methods {
				field := typ.Field(i + 1)
				if field.Name != name || field.Type.Kind() != reflect.Uintptr {
					t.Fatalf("method %d = %s %s", i, field.Name, field.Type)
				}
				if field.Offset != uintptr(3+i)*pointer {
					t.Fatalf("%s offset = %d; want %d", name, field.Offset, uintptr(3+i)*pointer)
				}
			}
		})
	}
}

func TestClosedInterfaceIdentities(t *testing.T) {
	iids := []struct {
		name  string
		got   syscall.GUID
		data1 uint32
		data2 uint16
		data3 uint16
	}{
		{"ITfThreadMgr", IID_ITfThreadMgr, 0xAA80E801, 0x2021, 0x11D2},
		{"ITfKeystrokeMgr", IID_ITfKeystrokeMgr, 0xAA80E7F0, 0x2021, 0x11D2},
		{"ITfKeyEventSink", IID_ITfKeyEventSink, 0xAA80E7F5, 0x2021, 0x11D2},
		{"ITfEditSession", IID_ITfEditSession, 0xAA80E803, 0x2021, 0x11D2},
		{"ITfTextInputProcessor", IID_ITfTextInputProcessor, 0xAA80E7F7, 0x2021, 0x11D2},
	}
	data4 := [8]byte{0x93, 0xE0, 0x00, 0x60, 0xB0, 0x67, 0xB8, 0x6E}
	for _, iid := range iids {
		if iid.got.Data1 != iid.data1 || iid.got.Data2 != iid.data2 || iid.got.Data3 != iid.data3 || iid.got.Data4 != data4 {
			t.Errorf("%s IID = %+v", iid.name, iid.got)
		}
	}
	mgr := reflect.TypeOf(ITfThreadMgr{})
	if mgr.NumField() != 1 || !mgr.Field(0).Anonymous || mgr.Field(0).Name != "IUnknown" {
		t.Fatalf("ITfThreadMgr = %s with %d fields", mgr, mgr.NumField())
	}
	if mgr == reflect.TypeOf(IUnknown{}) {
		t.Fatal("ITfThreadMgr collapsed to IUnknown")
	}
	var _ ITfThreadMgrInterface
	var _ ITfKeystrokeMgrInterface
	var _ ITfKeyEventSinkInterface
	var _ ITfEditSessionInterface
	var _ ITfTextInputProcessorInterface
}
