package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
	"unsafe"

	"github.com/zzl/go-com/com"
)

// These tests cover the fix for the protocol layer never being able to consume a
// key. Before it, replies were only logged and the key callbacks returned
// without eating anything, so no pinyin could ever be typed.

func TestKeyDecisionFollowsReplyConsume(t *testing.T) {
	consumed := `{"id":7,"session":99,"consume":true,"composition":"n",` +
		`"candidates":[{"text":"你","index":0}]}`
	cases := []struct {
		name    string
		reply   string
		replied bool
		want    bool
	}{
		{"consume true is eaten", consumed, true, true},
		{"consume false passes through", `{"id":7,"session":99,"consume":false}`, true, false},
		{"a reply that never arrived must not eat the key", "", false, false},
		{"a lost reply must not eat the key", consumed, false, false},
		{"a timeout line must not eat the key", `{"error":{"code":"INVALID_MESSAGE"}}`, true, false},
	}
	for _, c := range cases {
		if got := keyDecision(c.reply, c.replied); got != c.want {
			t.Errorf("%s: keyDecision(%q, %v) = %v, want %v", c.name, c.reply, c.replied, got, c.want)
		}
	}
}

func TestReplyIDReadsTheIdField(t *testing.T) {
	cases := []struct {
		name   string
		line   string
		want   uint64
		wantOK bool
	}{
		{"id first", `{"id":7,"session":9,"consume":true}`, 7, true},
		{"session first", `{"session":9,"id":12,"consume":true}`, 12, true},
		{"whitespace tolerated", `{"id": 15, "consume":true}`, 15, true},
		{"zero id is still a field", `{"id":0,"session":1,"consume":false}`, 0, true},
		{"id text inside a string is not a field", `{"session":1,"composition":"a\",\"id\":9","id":5}`, 5, true},
		{"no id field", `{"session":1,"consume":true}`, 0, false},
		{"not json", `garbage`, 0, false},
	}
	for _, c := range cases {
		got, ok := replyID(c.line)
		if ok != c.wantOK || (ok && got != c.want) {
			t.Errorf("%s: replyID(%q) = (%d, %v), want (%d, %v)", c.name, c.line, got, ok, c.want, c.wantOK)
		}
	}
}

func TestReplyConsumeReadsTheFlag(t *testing.T) {
	cases := []struct {
		name string
		line string
		want bool
	}{
		{"true", `{"id":1,"consume":true}`, true},
		{"false", `{"id":2,"consume":false}`, false},
		{"whitespace tolerated", `{"id":3,"consume": true}`, true},
		{"flag missing", `{"id":4,"session":9}`, false},
		{"flag text inside a composition is not a field", `{"id":5,"consume":false,"composition":"consume:true"}`, false},
		{"first field wins over candidate text", `{"id":6,"consume":true,"candidates":[{"text":"consume:false"}]}`, true},
	}
	for _, c := range cases {
		if got := replyConsume(c.line); got != c.want {
			t.Errorf("%s: replyConsume(%q) = %v, want %v", c.name, c.line, got, c.want)
		}
	}
}

func TestDeliverReplyWakesOnlyTheWaitingId(t *testing.T) {
	waiter := registerReplyWaiter(64)
	defer unregisterReplyWaiter(64)
	if deliverReply(`{"id":63,"consume":true}`) {
		t.Fatal("a reply for another id must not be delivered")
	}
	if !deliverReply(`{"session":9,"id":64,"consume":true}`) {
		t.Fatal("the waiting id must take the reply")
	}
	select {
	case line, open := <-waiter:
		if !open || !strings.Contains(line, `"id":64`) {
			t.Fatalf("waiter received %q open=%v", line, open)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("the waiting callback was never released")
	}
}

func TestFailReplyWaitersReleasesCallbacksWithoutAReply(t *testing.T) {
	waiter := registerReplyWaiter(65)
	failReplyWaiters()
	select {
	case _, open := <-waiter:
		if open {
			t.Fatal("a failed waiter must read as no reply")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("failReplyWaiters must release every waiting callback")
	}
	if deliverReply(`{"id":65,"consume":true}`) {
		t.Fatal("a failed waiter must no longer be registered")
	}
}

// withClosedPipe hides the real pipe state so the round-trip path can be tested
// without a Host. Package state is global, so the original values are restored
// when the test ends.
func withClosedPipe(t *testing.T) {
	t.Helper()
	pipeMu.Lock()
	savedHandle := pipeHandle
	savedReady := pipeReady.Swap(false)
	pipeHandle = syscall.InvalidHandle
	pipeMu.Unlock()
	t.Cleanup(func() {
		pipeMu.Lock()
		pipeHandle = savedHandle
		pipeReady.Store(savedReady)
		pipeMu.Unlock()
	})
}

func TestCallCoreWithoutAPipeReportsNoReply(t *testing.T) {
	t.Setenv(strategyLogPathEnvVar, filepath.Join(t.TempDir(), "tip.log"))
	withClosedPipe(t)
	if _, ok := callCore("keyDown", 'A', 0); ok {
		t.Fatal("callCore must report no reply when the pipe is not ready")
	}
	// F1 has no protocol key name, so it is answered locally. It still must not
	// be eaten, exactly like the C++ service returning success with consume=false.
	reply, ok := callCore("testKeyDown", 0x70, 0)
	if !ok {
		t.Fatal("an unmapped key has nothing to ask the Host, so it is answered locally")
	}
	if keyDecision(reply, ok) {
		t.Fatal("an unmapped key must never be eaten")
	}
}

func TestWinSpaceBypassOnlyClaimsSpaceWhileWinIsHeld(t *testing.T) {
	if winSpaceBypass('A') {
		t.Fatal("only VK_SPACE can be bypassed")
	}
	if got, want := winSpaceBypass(0x20), winHeld(); got != want {
		t.Fatalf("winSpaceBypass(VK_SPACE) = %v, want %v (a Windows key must be held)", got, want)
	}
}

// Every key event carries a modifier mask, so the GetKeyState lookup runs on
// every keystroke. A Wrong-DLL lookup panics there, which a guarded callback
// converts into a silent E_FAIL and no key is ever consumed.
func TestCurrentModifiersReadsKeyboardState(t *testing.T) {
	for attempt := 0; attempt < 3; attempt++ {
		modifiers := currentModifiers()
		if modifiers < 0 || modifiers > 7 {
			t.Fatalf("currentModifiers() = %d, want a 3-bit mask", modifiers)
		}
	}
}

// The key callbacks must answer S_OK even when the pipe is gone: a failure
// HRESULT makes TSF drop the text service, which would look like an input method
// that cannot be switched to. The callbacks receive their real COM this pointer,
// because they need the service state that hangs off it. The ITfContext is null
// here, so no edit session is attempted; the write into a real context can only
// be exercised against a live TSF host.
func TestKeySinkCallbacksSurviveWithoutAPipe(t *testing.T) {
	t.Setenv(strategyLogPathEnvVar, filepath.Join(t.TempDir(), "tip.log"))
	withClosedPipe(t)
	obj := com.NewComObj[tipComObj](&tipImpl{})
	defer obj.Release()
	sink := uintptr(unsafe.Pointer(&obj.keySinkComObj))
	callbacks := []struct {
		name string
		call func() uintptr
	}{
		{"OnTestKeyDown", func() uintptr { return keySinkOnTestKeyDown(sink, 0, 'A', 0, 0) }},
		{"OnKeyDown", func() uintptr { return keySinkOnKeyDown(sink, 0, 'A', 0, 0) }},
		{"OnTestKeyUp", func() uintptr { return keySinkOnTestKeyUp(sink, 0, 'A', 0, 0) }},
		{"OnKeyUp", func() uintptr { return keySinkOnKeyUp(sink, 0, 'A', 0, 0) }},
	}
	for _, c := range callbacks {
		if got := c.call(); got != 0 {
			t.Errorf("%s returned 0x%x, want S_OK", c.name, got)
		}
	}
	// A null pfEaten must not take the host application down either.
	eat("unit", 0, true)
}

var testKernel32 = syscall.NewLazyDLL("kernel32.dll")

const (
	pipeAccessDuplex = 0x00000003
	pipeTypeByte     = 0x00000000
	pipeReadModeByte = 0x00000000
	pipeWait         = 0x00000000
)

func createTestPipe(name string) (syscall.Handle, error) {
	namePtr, err := syscall.UTF16PtrFromString(name)
	if err != nil {
		return syscall.InvalidHandle, err
	}
	handle, _, callErr := testKernel32.NewProc("CreateNamedPipeW").Call(
		uintptr(unsafe.Pointer(namePtr)), pipeAccessDuplex,
		pipeTypeByte|pipeReadModeByte|pipeWait, 1, 512, 512, 0, 0)
	if syscall.Handle(handle) == syscall.InvalidHandle {
		return syscall.InvalidHandle, callErr
	}
	return syscall.Handle(handle), nil
}

// errorPipeConnected means the client connected before ConnectNamedPipe was
// called. That is a completed connection, not a failure.
const errorPipeConnected = syscall.Errno(535)

func connectTestPipe(handle syscall.Handle) error {
	result, _, err := testKernel32.NewProc("ConnectNamedPipe").Call(uintptr(handle), 0)
	if result == 0 && err != errorPipeConnected {
		return err
	}
	return nil
}

func disconnectTestPipe(handle syscall.Handle) {
	_, _, _ = testKernel32.NewProc("DisconnectNamedPipe").Call(uintptr(handle))
}

// readTestLine reads one newline-terminated line from a synchronous byte pipe.
func readTestLine(handle syscall.Handle) (string, error) {
	var line []byte
	buffer := make([]byte, 64)
	for len(line) < 4096 {
		var read uint32
		if err := syscall.ReadFile(handle, buffer, &read, nil); err != nil {
			return "", err
		}
		line = append(line, buffer[:read]...)
		if index := strings.IndexByte(string(line), '\n'); index >= 0 {
			return string(line[:index]), nil
		}
	}
	return "", fmt.Errorf("line longer than 4096 bytes")
}

func writeTestLine(handle syscall.Handle, line string) error {
	// done must be non-nil: syscall.WriteFile dereferences it when the race
	// detector is enabled.
	var written uint32
	return syscall.WriteFile(handle, []byte(line+"\n"), &written, nil)
}

// The full round trip: a TSF-shaped callback writes a key event, the Host side
// answers with the consume flag, and the value that reaches pfEaten has to
// follow that flag. This is the behaviour the protocol layer was missing.
func TestCallCoreRoundTripDeliversConsume(t *testing.T) {
	t.Setenv(strategyLogPathEnvVar, filepath.Join(t.TempDir(), "tip.log"))
	// Own pipe name: the shared one may belong to a running Host, and a test
	// must not disturb a real input method.
	name := `\\.\pipe\TypeScriptWindowsIME.Test.` + strconv.Itoa(os.Getpid())
	savedName := tsfPipeName
	tsfPipeName = name
	defer func() { tsfPipeName = savedName }()

	server, err := createTestPipe(name)
	if err != nil {
		t.Fatalf("CreateNamedPipeW(%s): %v", name, err)
	}
	defer func() {
		disconnectTestPipe(server)
		_ = syscall.CloseHandle(server)
	}()

	hostDone := make(chan struct{})
	go func() {
		defer close(hostDone)
		if connectTestPipe(server) != nil {
			return
		}
		for {
			line, err := readTestLine(server)
			if err != nil {
				return
			}
			id, ok := replyID(line)
			if !ok {
				continue
			}
			// The key event carries vk; 'A' is the key the engine consumes and
			// 'B' is the one it does not.
			consume := "false"
			if strings.Contains(line, `"vk":65`) {
				consume = "true"
			}
			_ = writeTestLine(server,
				fmt.Sprintf(`{"session":42,"id":%d,"consume":%s,"composition":"a"}`, id, consume))
		}
	}()

	generation := pipeGeneration.Add(1)
	handle, err := dialTsf(5 * time.Second)
	if err != nil {
		t.Fatalf("dialTsf(%s): %v", name, err)
	}
	if !storePipe(generation, handle) {
		_ = syscall.CloseHandle(handle)
		t.Fatal("storePipe rejected the connection")
	}
	pipeReady.Store(true)
	drained := make(chan struct{})
	go func() {
		defer close(drained)
		drainPipe(generation, handle)
	}()
	defer func() {
		pipeGeneration.Add(1)
		_ = syscall.CancelIoEx(handle, nil)
		releaseIfOwner(handle)
		// Wait for the reader to stop logging before t.Setenv puts the shared
		// log path back: the race detector sees the environment read from the
		// reader goroutine and the write from the cleanup as a data race.
		<-drained
	}()

	reply, ok := callCore("keyDown", 'A', 30<<16)
	if !ok {
		t.Fatal("callCore must report the reply it received")
	}
	if !keyDecision(reply, ok) {
		t.Fatalf("a consume:true reply must eat the key, got %s", reply)
	}

	reply, ok = callCore("testKeyDown", 'B', 30<<16)
	if !ok {
		t.Fatal("callCore must report the reply it received")
	}
	if keyDecision(reply, ok) {
		t.Fatalf("a consume:false reply must leave the key with the application, got %s", reply)
	}
}
