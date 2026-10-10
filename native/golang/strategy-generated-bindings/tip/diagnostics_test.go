package main

import (
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

// A panic inside a COM callback must never cross back into native code: this DLL
// is loaded into whatever application the user typed into, so an escaping panic
// takes that process down instead of returning a failure HRESULT to TSF.
func TestGuardedCallConvertsPanicToEFail(t *testing.T) {
	// The guard logs a panic and dumps every goroutine. Both go to the shared
	// TIP log, so the test is redirected to its own file first.
	t.Setenv(strategyLogPathEnvVar, filepath.Join(t.TempDir(), "tip.log"))
	result := guardedCall("unit.panic", func() uintptr {
		panic("simulated failure inside a callback")
	})
	if result != hresultToUintptr(strategyEFail) {
		t.Fatalf("guardedCall must return E_FAIL (0x%08x) after a panic, got 0x%08x",
			hresultToUintptr(strategyEFail), result)
	}
	logged, err := os.ReadFile(os.Getenv(strategyLogPathEnvVar))
	if err != nil {
		t.Fatalf("the panic must be logged so it can be found without a debugger: %v", err)
	}
	if !strings.Contains(string(logged), "PANIC unit.panic") {
		t.Fatalf("the log must name the callback that panicked, got:\n%s", logged)
	}
	// A full dump is what makes a deadlock in a DLL debuggable at all.
	if !strings.Contains(string(logged), "goroutine dump") {
		t.Fatalf("the log must contain a goroutine dump, got:\n%s", logged)
	}
}

func TestGuardedCallPassesThroughSuccess(t *testing.T) {
	const want = 0x00000002 // S_OK
	if got := guardedCall("unit.ok", func() uintptr { return want }); got != want {
		t.Fatalf("guardedCall must return the callback result, got 0x%08x", got)
	}
}

// hresultToUintptr has to survive the full HRESULT range. Converting a negative
// constant straight to uintptr is rejected at compile time, which is why the
// conversion lives in a function.
func TestHresultToUintptrRoundTrip(t *testing.T) {
	cases := []struct {
		name string
		in   int32
		want uintptr
	}{
		{"S_OK", 0, 0x00000000},
		{"S_FALSE", 1, 0x00000001},
		{"E_FAIL", strategyEFail, 0x80004005},
		{"E_POINTER", -2147467261, 0x80004003},
	}
	for _, c := range cases {
		if got := hresultToUintptr(c.in); got != c.want {
			t.Errorf("%s: got 0x%08x want 0x%08x", c.name, got, c.want)
		}
	}
}

// The reply decoder exists because the old log truncated at 200 bytes, which cut
// off the consume flag exactly where it explains why a key was not eaten.
func TestDescribeReplyExtractsDecisionFields(t *testing.T) {
	line := `{"id":3,"session":21433417729,"consume":true,"composition":"a",` +
		`"candidates":[{"text":"啊","index":0},{"text":"阿","index":1}],"selectedCandidate":0}`
	got := describeReply(line)
	for _, want := range []string{"id=3", "session=21433417729", "consume=true", `composition="a"`, "candidates=2", "selected=0"} {
		if !strings.Contains(got, want) {
			t.Errorf("describeReply missing %q in %q", want, got)
		}
	}
	if !strings.Contains(got, "bytes=") {
		t.Errorf("describeReply must report the byte count, got %q", got)
	}
}

func TestDescribeReplyKeepsFullPayloadAndErrors(t *testing.T) {
	errorLine := `{"id":1,"session":7,"consume":false,"error":{"code":"UNSUPPORTED_PROTOCOL","message":"Unsupported protocol version: 9"}}`
	got := describeReply(errorLine)
	if !strings.Contains(got, "error=") {
		t.Fatalf("describeReply must surface an error reply, got %q", got)
	}
	if !strings.Contains(got, "UNSUPPORTED_PROTOCOL") {
		t.Fatalf("describeReply must keep the error detail, got %q", got)
	}
	// A payload longer than the old 200 byte cap has to survive intact.
	if !strings.Contains(got, errorLine) {
		t.Fatalf("describeReply must not truncate the raw line, got %q", got)
	}
}

func TestDescribeReplyHandlesMalformedLine(t *testing.T) {
	if got := describeReply("not json at all"); !strings.Contains(got, "bytes=") {
		t.Fatalf("describeReply must survive a malformed line, got %q", got)
	}
}

func TestGuidTextNamesKnownInterfaces(t *testing.T) {
	cases := []struct {
		name string
		guid syscall.GUID
		want string
	}{
		{"clsid", syscall.GUID(clsidStrategy), "CLSID_StrategyTip"},
		{"processor iid", syscall.GUID(iidTextInputProcessor), "IID_ITfTextInputProcessor"},
		{"key sink iid", syscall.GUID(iidKeyEventSink), "IID_ITfKeyEventSink"},
	}
	for _, c := range cases {
		if got := guidText(c.guid); got != c.want {
			t.Errorf("%s: got %q want %q", c.name, got, c.want)
		}
	}
	unknown := guidText(syscall.GUID{Data1: 0x11223344})
	if !strings.HasPrefix(unknown, "{11223344-") {
		t.Fatalf("an unknown IID must still be printable, got %q", unknown)
	}
}
