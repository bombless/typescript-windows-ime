package main

import (
	"strings"
	"testing"
)

// These cover the reply fields the edit session depends on. Node sends objects
// with a text field for candidates, and the Host only accepts a plain string
// array, so the text values have to be lifted out and re-encoded.

func TestReplyStringExtractsTextFields(t *testing.T) {
	line := `{"id":7,"session":99,"consume":true,"composition":"ni",` +
		`"commit":"你","candidates":[{"text":"你","index":0}],"selectedCandidate":0}`
	if got := replyString(line, "composition"); got != "ni" {
		t.Errorf("composition: got %q want %q", got, "ni")
	}
	if got := replyString(line, "commit"); got != "你" {
		t.Errorf("commit: got %q want %q", got, "你")
	}
	if got := replyString(line, "missing"); got != "" {
		t.Errorf("a missing field must read as empty, got %q", got)
	}
	// A field name that appears inside a value must not be mistaken for one.
	quoted := `{"id":8,"session":1,"consume":true,"composition":"say \"ni\" now"}`
	if got := replyString(quoted, "composition"); got != `say "ni" now` {
		t.Errorf("escaped quotes must survive, got %q", got)
	}
}

func TestReplyHasDetectsCommit(t *testing.T) {
	withCommit := `{"id":7,"session":99,"consume":true,"composition":"","commit":"你"}`
	withoutCommit := `{"id":7,"session":99,"consume":true,"composition":"n"}`
	if !replyHas(withCommit, "commit") {
		t.Error("a commit reply must be recognised as one")
	}
	if replyHas(withoutCommit, "commit") {
		t.Error("a composition-only reply must not be read as a commit")
	}
}

func TestReplyUintReadsSelection(t *testing.T) {
	if got := replyUint(`{"id":7,"selectedCandidate":2}`, "selectedCandidate"); got != 2 {
		t.Errorf("selectedCandidate: got %d want 2", got)
	}
	if got := replyUint(`{"id":7,"consume":true}`, "selectedCandidate"); got != 0 {
		t.Errorf("a missing selection must read as 0, got %d", got)
	}
}

func TestReplyCandidatesReadsObjectFormat(t *testing.T) {
	line := `{"id":7,"session":99,"consume":true,"composition":"ni",` +
		`"candidates":[{"text":"你","index":0,"annotation":"ni"},{"text":"拟","index":1,"annotation":"ni"}],` +
		`"selectedCandidate":0}`
	got := replyCandidates(line)
	want := []string{"你", "拟"}
	if len(got) != len(want) {
		t.Fatalf("replyCandidates() = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("candidate %d: got %q want %q", i, got[i], want[i])
		}
	}
	// The probe list the TIP sends at connect time is a plain string array;
	// the Host parses that shape itself, so nothing here reads it.
	if got := replyCandidates(`{"candidates":["strategy-generated-bindings"]}`); got != nil {
		t.Errorf("a string-array list must not be read as objects: %v", got)
	}
}

func TestJsonEscapeQuotesForTheHost(t *testing.T) {
	cases := []struct {
		in   string
		want string
	}{
		{"ni", "ni"},
		{`say "hi"`, `say \"hi\"`},
		{`back\slash`, `back\\slash`},
		{"line\nbreak", `line\nbreak`},
		{"tab\there", `tab\there`},
	}
	for _, c := range cases {
		if got := jsonEscape(c.in); got != c.want {
			t.Errorf("jsonEscape(%q) = %q, want %q", c.in, got, c.want)
		}
	}
	// The escaped form has to be legal when wrapped in quotes, because the Host
	// parses the candidate list as JSON text.
	for _, c := range cases {
		quoted := `"` + jsonEscape(c.in) + `"`
		if !strings.HasPrefix(quoted, `"`) || !strings.HasSuffix(quoted, `"`) {
			t.Errorf("jsonEscape(%q) produced unquotable text %s", c.in, quoted)
		}
	}
}

// These cover what the Host is asked to draw, and what focus returning to the
// application is allowed to put back on screen.

func TestCandidatesPayloadEncodesSelectionAndCaret(t *testing.T) {
	reply := `{"id":7,"consume":true,"composition":"ni",` +
		`"candidates":[{"text":"你","index":0},{"text":"拟","index":1}],"selectedCandidate":1}`
	payload := candidatesPayload(reply, [4]int32{10, 20, 30, 40})
	for _, want := range []string{
		`"candidates":["你","拟"]`,
		`"selection":1`,
		`"caret":{"left":10,"top":20,"right":30,"bottom":40}`,
	} {
		if !strings.Contains(payload, want) {
			t.Errorf("candidatesPayload() = %s, want it to contain %s", payload, want)
		}
	}
}

func TestCandidatesPayloadIsEmptyWhenTheListMustHide(t *testing.T) {
	for _, reply := range []string{
		// A chosen candidate commits, so the list has to go.
		`{"id":7,"consume":true,"composition":"","commit":"你","candidates":[{"text":"你"}]}`,
		// No candidates at all.
		`{"id":7,"consume":true,"composition":"ni"}`,
		// Candidates but no composition to compose.
		`{"id":7,"consume":true,"composition":"","candidates":[{"text":"你"}]}`,
	} {
		if payload := candidatesPayload(reply, [4]int32{10, 20, 30, 40}); payload != "" {
			t.Errorf("candidatesPayload(%s) = %q, want an empty hide request", reply, payload)
		}
	}
}

func TestRestoreCandidatesIgnoresAServiceWithNoComposition(t *testing.T) {
	service := &tipImpl{}
	service.shownCandidates.Store(`,"candidates":["你","拟"],"selection":0`)
	// No composition and nothing remembered: a remembered list alone must not
	// reach the Host, because no keystroke could consume a pick from it.
	service.restoreCandidates()
	if service.shownCandidates.Load().(string) != `,"candidates":["你","拟"],"selection":0` {
		t.Error("restoreCandidates must not touch the remembered payload")
	}
}

func TestRestoreCandidatesIgnoresNothingRemembered(t *testing.T) {
	service := &tipImpl{}
	service.composition.Store(1)
	service.compositionContext.Store(1)
	service.shownCandidates.Store("")
	service.restoreCandidates()
}
