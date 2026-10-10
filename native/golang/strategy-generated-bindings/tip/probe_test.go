package main

import (
	"strings"
	"testing"
)

func TestProbeLines(t *testing.T) {
	hello, show := probeLines(42)
	if strings.Contains(hello, "\n") || strings.Contains(show, "\n") {
		t.Fatal("probe lines must stay on one line")
	}
	for _, part := range []string{`"id":1`, `"session":42`, `"type":"hello"`, `"protocol":3`} {
		if !strings.Contains(hello, part) {
			t.Fatalf("hello missing %s: %s", part, hello)
		}
	}
	for _, part := range []string{
		`"id":2`,
		`"session":42`,
		`"type":"showCandidates"`,
		`"candidates":["strategy-generated-bindings"]`,
		`"selection":0`,
		`"left":200`,
		`"top":200`,
		`"right":400`,
		`"bottom":220`,
		`"dpi":96`,
	} {
		if !strings.Contains(show, part) {
			t.Fatalf("show missing %s: %s", part, show)
		}
	}
}
