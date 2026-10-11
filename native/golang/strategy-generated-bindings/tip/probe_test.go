package main

import (
	"strings"
	"testing"
)

func TestHelloLine(t *testing.T) {
	hello := helloLine(42)
	if strings.Contains(hello, "\n") {
		t.Fatal("hello must stay on one line")
	}
	for _, part := range []string{`"id":1`, `"session":42`, `"type":"hello"`, `"protocol":3`} {
		if !strings.Contains(hello, part) {
			t.Fatalf("hello missing %s: %s", part, hello)
		}
	}
}

// Activation must not announce a candidate list: the Host draws every
// showCandidates it receives in a window of its own, so one sent on activation
// put a floating box over the application the moment the input method was
// selected.
func TestActivationSendsNoCandidateNotification(t *testing.T) {
	if strings.Contains(helloLine(42), "Candidates") {
		t.Fatalf("activation must not announce candidates: %s", helloLine(42))
	}
}
