package main

/*
#include <stdint.h>
#include <windows.h>
#include <msctf.h>

// Thin vtable thunks for the TSF calls this package makes. Each helper forwards
// exactly one COM call, so the argument layout stays in C where the SDK
// declarations are checked by the compiler, and no vtable offsets are spelled
// out in Go.
//
// COM objects arrive as uintptr_t: Go hands over the raw pointer value it was
// given by TSF, and only the cast to the interface type happens here. Passing
// the value through C.uintptr_t keeps Go from converting a uintptr into an
// unsafe.Pointer at every call site.

static HRESULT tipQueryInterface(uintptr_t object, const GUID* iid, uintptr_t* out) {
    IUnknown* unknown = (IUnknown*)(void*)object;
    return unknown->lpVtbl->QueryInterface(unknown, iid, (void**)out);
}

static HRESULT tipContextRequestEditSession(uintptr_t context, uint32_t client, uintptr_t session,
                                            uint32_t flags, int32_t* phrSession) {
    ITfContext* ctx = (ITfContext*)(void*)context;
    return ctx->lpVtbl->RequestEditSession(ctx, client, (ITfEditSession*)(void*)session,
        flags, (HRESULT*)phrSession);
}

// Returns the selection's range. The caller owns the reference: TSF hands out
// one and the session is its only holder.
static HRESULT tipContextGetSelectionRange(uintptr_t context, uint32_t ec, uintptr_t* range) {
    TF_SELECTION selection;
    ULONG fetched = 0;
    ITfContext* ctx = (ITfContext*)(void*)context;
    HRESULT hr = ctx->lpVtbl->GetSelection(ctx, ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (FAILED(hr)) return hr;
    if (fetched != 1 || !selection.range) return E_FAIL;
    *range = (uintptr_t)(void*)selection.range;
    return S_OK;
}

// Collapses the document selection onto a range. Without this the caret stays
// where the composition began, in front of the text the IME just wrote.
static HRESULT tipContextSetSelection(uintptr_t context, uint32_t ec, uintptr_t range) {
    TF_SELECTION selection;
    selection.range = (ITfRange*)(void*)range;
    selection.style.ase = TF_AE_NONE;
    selection.style.fInterimChar = FALSE;
    ITfContext* ctx = (ITfContext*)(void*)context;
    return ctx->lpVtbl->SetSelection(ctx, ec, 1, &selection);
}

static HRESULT tipContextGetActiveView(uintptr_t context, uintptr_t* view) {
    ITfContext* ctx = (ITfContext*)(void*)context;
    return ctx->lpVtbl->GetActiveView(ctx, (ITfContextView**)(void**)view);
}

static HRESULT tipContextCompositionStart(uintptr_t contextComposition, uint32_t ec, uintptr_t range,
                                          uintptr_t sink, uintptr_t* composition) {
    ITfContextComposition* cc = (ITfContextComposition*)(void*)contextComposition;
    return cc->lpVtbl->StartComposition(cc, ec, (ITfRange*)(void*)range,
        (ITfCompositionSink*)(void*)sink, (ITfComposition**)(void**)composition);
}

static HRESULT tipCompositionGetRange(uintptr_t composition, uintptr_t* range) {
    ITfComposition* comp = (ITfComposition*)(void*)composition;
    return comp->lpVtbl->GetRange(comp, (ITfRange**)(void**)range);
}

static HRESULT tipCompositionEnd(uintptr_t composition, uint32_t ec) {
    ITfComposition* comp = (ITfComposition*)(void*)composition;
    return comp->lpVtbl->EndComposition(comp, ec);
}

static HRESULT tipRangeSetText(uintptr_t range, uint32_t ec, const wchar_t* text, int32_t count) {
    ITfRange* r = (ITfRange*)(void*)range;
    return r->lpVtbl->SetText(r, ec, 0, text, count);
}

static HRESULT tipRangeClone(uintptr_t range, uintptr_t* clone) {
    ITfRange* r = (ITfRange*)(void*)range;
    return r->lpVtbl->Clone(r, (ITfRange**)(void**)clone);
}

static HRESULT tipRangeCollapse(uintptr_t range, uint32_t ec, int32_t anchor) {
    ITfRange* r = (ITfRange*)(void*)range;
    return r->lpVtbl->Collapse(r, ec, (TfAnchor)anchor);
}

// Moves the end anchor by a character count. pHalt is NULL: the plain shift is
// all the composition needs, and pcch still reports how far the anchor really
// went, which is how a document shorter than the text we wrote shows up.
static HRESULT tipRangeShiftEnd(uintptr_t range, uint32_t ec, int32_t count, int32_t* shifted) {
    ITfRange* r = (ITfRange*)(void*)range;
    LONG cch = 0;
    HRESULT hr = r->lpVtbl->ShiftEnd(r, ec, count, &cch, NULL);
    if (shifted) *shifted = (int32_t)cch;
    return hr;
}

// Reads where the range sits in the document. ITfRange itself has no extent
// accessor, only the ACP flavour does, and unlike most of the interface it
// needs no edit cookie. A context that is not ACP based simply has no extent
// to report: the caller logs that and carries on.
static HRESULT tipRangeExtent(uintptr_t range, int32_t* anchor, int32_t* length) {
    ITfRange* r = (ITfRange*)(void*)range;
    ITfRangeACP* acp = NULL;
    HRESULT hr = r->lpVtbl->QueryInterface(r, &IID_ITfRangeACP, (void**)&acp);
    if (FAILED(hr)) return hr;
    LONG a = 0;
    LONG c = 0;
    hr = acp->lpVtbl->GetExtent(acp, &a, &c);
    acp->lpVtbl->Release(acp);
    if (anchor) *anchor = (int32_t)a;
    if (length) *length = (int32_t)c;
    return hr;
}

// Reads back what the range covers. The buffer is one unit larger than the
// text the caller expects, so a range covering more or less than that cannot
// hide behind the capacity: pcch comes back as units+1 or less.
static HRESULT tipRangeText(uintptr_t range, uint32_t ec, wchar_t* buffer, int32_t capacity, int32_t* count) {
    ITfRange* r = (ITfRange*)(void*)range;
    ULONG written = 0;
    HRESULT hr = r->lpVtbl->GetText(r, ec, 0, buffer, (ULONG)capacity, &written);
    if (count) *count = (int32_t)written;
    return hr;
}

// Gravity needs no edit cookie, so it can be read whenever the range exists.
static HRESULT tipRangeGravity(uintptr_t range, int32_t* gStart, int32_t* gEnd) {
    ITfRange* r = (ITfRange*)(void*)range;
    TfGravity start = TF_GRAVITY_BACKWARD;
    TfGravity end = TF_GRAVITY_BACKWARD;
    HRESULT hr = r->lpVtbl->GetGravity(r, &start, &end);
    if (gStart) *gStart = (int32_t)start;
    if (gEnd) *gEnd = (int32_t)end;
    return hr;
}

static HRESULT tipViewGetTextExt(uintptr_t view, uint32_t ec, uintptr_t range, void* rect, int32_t* clipped) {
    ITfContextView* v = (ITfContextView*)(void*)view;
    return v->lpVtbl->GetTextExt(v, ec, (ITfRange*)(void*)range, (RECT*)rect, (BOOL*)clipped);
}

static ULONG tipRelease(uintptr_t object) {
    IUnknown* unknown = (IUnknown*)(void*)object;
    return unknown->lpVtbl->Release(unknown);
}
*/
import "C"

import (
	"fmt"
	"strconv"
	"strings"
	"syscall"
	"unsafe"

	"github.com/zzl/go-com/com"
	"github.com/zzl/go-win32api/v2/win32"
	"strategy-generated-bindings/closedtsf"
)

// TSF constants that the edit session needs. TF_ES_SYNC | TF_ES_READWRITE asks
// for a synchronous read-write session, which TSF runs by calling back into
// DoEditSession on this thread before RequestEditSession returns.
const (
	tfEsSyncReadWrite = 0x1 | 0x6
	tfAnchorStart     = 0
	tfAnchorEnd       = 1
)

// IIDs for the interfaces this file calls or implements. They come from
// um/msctf.idl and are only spelled out here because the generated closedtsf
// package stops at the interfaces it was extracted for.
var (
	iidContextComposition = syscall.GUID{Data1: 0xD40C8AAE, Data2: 0xAC92, Data3: 0x4FC7, Data4: [8]byte{0x9A, 0x11, 0x0E, 0xE0, 0xE2, 0x3A, 0xA3, 0x9B}}
	iidCompositionSink    = syscall.GUID{Data1: 0xA781718C, Data2: 0x579A, Data3: 0x4B15, Data4: [8]byte{0xA2, 0x80, 0x32, 0xB8, 0x57, 0x7A, 0xCC, 0x5E}}
)

type editSessionVtbl struct {
	closedtsf.IUnknownVtbl
	DoEditSession uintptr
}

type compositionSinkVtbl struct {
	closedtsf.IUnknownVtbl
	OnCompositionTerminated uintptr
}

// editSessionImpl carries one edit request. A fresh COM object is created per
// request, so the data lives with the object that DoEditSession receives and
// never has to be shared between callbacks.
type editSessionImpl struct {
	com.IUnknownImpl
	service *tipImpl
	context uintptr // *ITfContext, borrowed for the duration of the session.
	caretRange uintptr // non-zero for a caret-only edit session
	text    string
	commit  bool
	caret   [4]int32
	failed  bool
}

type compositionSinkImpl struct {
	com.IUnknownImpl
	service *tipImpl
}

type editSessionComObj struct{ com.IUnknownComObj }

func (e *editSessionComObj) IID() *syscall.GUID { return &closedtsf.IID_ITfEditSession }

func (e *editSessionComObj) GetVtbl() *win32.IUnknownVtbl {
	return (*win32.IUnknownVtbl)(unsafe.Pointer(e.BuildEditSessionVtbl(true)))
}

type compositionSinkComObj struct{ com.IUnknownComObj }

func (c *compositionSinkComObj) IID() *syscall.GUID { return &iidCompositionSink }

func (c *compositionSinkComObj) GetVtbl() *win32.IUnknownVtbl {
	return (*win32.IUnknownVtbl)(unsafe.Pointer(c.BuildCompositionSinkVtbl(true)))
}

var editSessionVTable *editSessionVtbl
var compositionSinkVTable *compositionSinkVtbl

func (e *editSessionComObj) BuildEditSessionVtbl(lock bool) *editSessionVtbl {
	if lock {
		com.MuVtbl.Lock()
		defer com.MuVtbl.Unlock()
	}
	if editSessionVTable == nil {
		v := (*editSessionVtbl)(com.Malloc(unsafe.Sizeof(editSessionVtbl{})))
		base := e.IUnknownComObj.BuildVtbl(false)
		v.QueryInterface = base.QueryInterface
		v.AddRef = base.AddRef
		v.Release = base.Release
		v.DoEditSession = syscall.NewCallback(editSessionDoEditSession)
		editSessionVTable = v
	}
	return editSessionVTable
}

func (c *compositionSinkComObj) BuildCompositionSinkVtbl(lock bool) *compositionSinkVtbl {
	if lock {
		com.MuVtbl.Lock()
		defer com.MuVtbl.Unlock()
	}
	if compositionSinkVTable == nil {
		v := (*compositionSinkVtbl)(com.Malloc(unsafe.Sizeof(compositionSinkVtbl{})))
		base := c.IUnknownComObj.BuildVtbl(false)
		v.QueryInterface = base.QueryInterface
		v.AddRef = base.AddRef
		v.Release = base.Release
		v.OnCompositionTerminated = syscall.NewCallback(compositionSinkOnTerminated)
		compositionSinkVTable = v
	}
	return compositionSinkVTable
}

func comRelease(object uintptr) {
	if object != 0 {
		C.tipRelease(C.uintptr_t(object))
	}
}

// applyReply writes the Host's answer into the document: the composition while
// the user is still picking a candidate, the committed text when they choose
// one. It mirrors TextService::ApplyResponse in native/TsIme.cpp.
func (t *tipImpl) applyReply(context uintptr, reply string) [4]int32 {
	var caret [4]int32
	if context == 0 {
		strategyLog("StrategyTip applyReply skipped: no ITfContext")
		return caret
	}
	composition := replyString(reply, "composition")
	commit := replyString(reply, "commit")
	hasCommit := replyHas(reply, "commit")
	text := composition
	if hasCommit {
		text = commit
	}
	if text == "" && !hasCommit {
		// Nothing to draw and nothing to commit: end what is on screen.
		strategyLog("StrategyTip applyReply ending empty composition context=%#x previousWritten=%q range=%#x", context, t.writtenComposition(), uintptr(t.composition.Load()))
		t.endComposition(context, text)
		return caret
	}
	caret = t.runEditSession(context, text, hasCommit)
	return caret
}

// runEditSession asks the context for a synchronous read-write session and runs
// the composition work inside DoEditSession. RequestEditSession returns only
// after DoEditSession has finished, so the caret it reads is available here.
func (t *tipImpl) runEditSession(context uintptr, text string, commit bool) [4]int32 {
	session := com.NewComObj[editSessionComObj](&editSessionImpl{
		service: t, context: context, text: text, commit: commit,
	})
	var sessionResult int32
	hr := C.tipContextRequestEditSession(C.uintptr_t(context), C.uint32_t(t.clientID),
		C.uintptr_t(uintptr(unsafe.Pointer(session))), C.uint32_t(tfEsSyncReadWrite),
		(*C.int32_t)(unsafe.Pointer(&sessionResult)))
	impl := session.Impl().(*editSessionImpl)
	caret := impl.caret
	failed := impl.failed
	session.Release()
	if hr < 0 || sessionResult < 0 || failed {
		// A rejected session must not stay broken: drop the composition so the
		// next keystroke starts a fresh one, exactly like the C++ service.
		strategyLog("StrategyTip edit session rejected hr=0x%08x session=0x%08x failed=%v",
			uint32(hr), uint32(sessionResult), failed)
		t.dropComposition()
		return caret
	}
	strategyLog("StrategyTip edit session ok commit=%v text=%q caret=%d,%d,%d,%d",
		commit, text, caret[0], caret[1], caret[2], caret[3])
	return caret
}

// runCaretEditSession applies a parked selection using a fresh TSF write cookie.
// ITfContext::SetSelection cannot be called with cookie 0 outside an edit session.
func (t *tipImpl) runCaretEditSession(context, caretRange uintptr) {
	if context == 0 || caretRange == 0 {
		return
	}
	session := com.NewComObj[editSessionComObj](&editSessionImpl{
		service: t, context: context, caretRange: caretRange,
	})
	var sessionResult int32
	hr := C.tipContextRequestEditSession(C.uintptr_t(context), C.uint32_t(t.clientID),
		C.uintptr_t(uintptr(unsafe.Pointer(session))), C.uint32_t(tfEsSyncReadWrite),
		(*C.int32_t)(unsafe.Pointer(&sessionResult)))
	impl := session.Impl().(*editSessionImpl)
	failed := impl.failed
	session.Release()
	if hr < 0 || sessionResult < 0 || failed {
		strategyLog("StrategyTip caret edit session rejected hr=0x%08x session=0x%08x failed=%v",
			uint32(hr), uint32(sessionResult), failed)
	}
}

// editSessionDoEditSession is the ITfEditSession callback. TSF calls it with a
// write cookie on the thread that asked for the session, which is the thread
// that was inside OnKeyDown.
func editSessionDoEditSession(this uintptr, ec uintptr) uintptr {
	return guardedCall("ITfEditSession.DoEditSession", func() uintptr {
		session := (*editSessionComObj)(unsafe.Pointer(this))
		impl := session.Impl().(*editSessionImpl)
		if !impl.apply(uintptr(ec)) {
			impl.failed = true
			return hresultToUintptr(int32(win32.E_FAIL))
		}
		return hresultToUintptr(int32(win32.S_OK))
	})
}

// utf16Units is how many UTF-16 code units text occupies in the document,
// which is the count ITfRange::SetText takes. It is not len(text): a Chinese
// character is one unit while several bytes in the Go string, and -1 means the
// text cannot be encoded at all, so no range may be sized from it.
func utf16Units(text string) int {
	wide, err := syscall.UTF16FromString(text)
	if err != nil {
		return -1
	}
	return len(wide) - 1 // UTF16FromString appends the terminator SetText must not receive
}

// writtenComposition is the text this service last put into the document
// through the composition range. It is what the next write has to replace and
// what a cancel has to erase.
func (t *tipImpl) writtenComposition() string {
	text, _ := t.compositionWritten.Load().(string)
	return text
}

func (t *tipImpl) storeWrittenComposition(text string) {
	t.compositionWritten.Store(text)
}

// coverCompositionRange places the range over exactly units UTF-16 code units
// starting at its own start anchor.
//
// A composition range does not grow with the text SetText writes into it: the
// anchors stay where the composition began, so the range keeps covering
// nothing while the document grows underneath. SetText on a collapsed range is
// an insertion, so every keystroke then inserts at the composition start
// instead of replacing the last one. That is what turned "ni" plus a space
// into "你nin": "n" was replaced by "ni", "ni" was inserted before it as
// "n" + "ni" = "nin", and the commit was inserted before that as "你" +
// "nin". Placing the range over the text first makes each write a replacement
// of exactly what is on screen, and leaves the range covering what was just
// written so the caret and the next keystroke agree with it.
func coverCompositionRange(ec uintptr, r uintptr, units int, when string) bool {
	if C.tipRangeCollapse(C.uintptr_t(r), C.uint32_t(ec), C.int32_t(tfAnchorStart)) < 0 {
		strategyLog("StrategyTip %s: collapsing the composition range failed", when)
		return false
	}
	if units <= 0 {
		return true
	}
	var shifted int32
	if C.tipRangeShiftEnd(C.uintptr_t(r), C.uint32_t(ec), C.int32_t(units),
		(*C.int32_t)(unsafe.Pointer(&shifted))) < 0 {
		strategyLog("StrategyTip %s: extending the composition range by %d failed", when, units)
		return false
	}
	if int(shifted) != units {
		// Short of the request means the document holds less text after the
		// composition start than the last write left there. Writing anyway
		// would overwrite text this service does not own.
		strategyLog("StrategyTip %s: composition range covers %d of %d units", when, shifted, units)
		return false
	}
	return true
}

// placeSelectionAtRangeEnd prepares the selection change that puts the caret
// behind committed text, without running it. SetText and EndComposition leave
// the selection where the composition began, so the caret shows up in front of
// the text the user just committed instead of after it. TSF never moves the
// selection for us: the only range the composition tracks is its own.
//
// The SetSelection itself is deferred because it notifies the application, and
// a Notepad-class host dispatches that synchronously back into a key callback.
// That callback would block on the non-reentrant keyMu this edit session runs
// under and freeze the host, which is exactly the hazard OnCompositionTerminated
// avoids by only recording a flag. So this clones the range, collapses the clone
// at the end of the text and parks it on the service. The key sink calls
// placePendingCaret after keyMu is released.
func (s *editSessionImpl) placeSelectionAtRangeEnd(ec uintptr, r uintptr) bool {
	var end uintptr
	if C.tipRangeClone(C.uintptr_t(r), (*C.uintptr_t)(unsafe.Pointer(&end))) < 0 || end == 0 {
		strategyLog("StrategyTip placing the caret: cloning the composition range failed")
		return false
	}
	if C.tipRangeCollapse(C.uintptr_t(end), C.uint32_t(ec), C.int32_t(tfAnchorEnd)) < 0 {
		strategyLog("StrategyTip placing the caret: collapsing the clone failed")
		comRelease(end)
		return false
	}
	s.service.dropPendingCaret()
	s.service.pendingCaretRange.Store(end)
	return true
}

// dropPendingCaret releases a parked selection change that never ran.
func (t *tipImpl) dropPendingCaret() {
	if parked := t.pendingCaretRange.Swap(0); parked != 0 {
		comRelease(parked)
	}
}

// placePendingCaret runs a parked selection change against context, the
// borrowed ITfContext of the OnKeyDown call that parked it. The key sink calls
// it after releasing keyMu, so no re-entrant key callback can deadlock against
// that lock. A failure only loses the caret: the text is already committed, and
// a fresh edit session is not worth asking for.
func (t *tipImpl) placePendingCaret(context uintptr) {
	parked := t.pendingCaretRange.Swap(0)
	if parked == 0 {
		return
	}
	defer comRelease(parked)
	t.runCaretEditSession(context, parked)
}

// checkCompositionText reads the range back and reports when it does not hold
// exactly want. The buffer is one unit larger than the expected text, so a
// range that covers too much or too little cannot look like a match.
func checkCompositionText(ec uintptr, r uintptr, want string, when string) {
	units := utf16Units(want)
	if units < 0 {
		return
	}
	buffer := make([]uint16, units+1)
	var count int32
	if C.tipRangeText(C.uintptr_t(r), C.uint32_t(ec),
		(*C.wchar_t)(unsafe.Pointer(&buffer[0])), C.int32_t(len(buffer)),
		(*C.int32_t)(unsafe.Pointer(&count))) < 0 {
		return
	}
	if got := syscall.UTF16ToString(buffer[:count]); got != want {
		strategyLog("StrategyTip %s: composition range holds %q, expected %q", when, got, want)
	}
}

// logCompositionRange records where the range sits and which way its anchors
// face, so a write that lands somewhere unexpected is visible in the log.
func logCompositionRange(r uintptr, units int, when string) {
	var gravityStart, gravityEnd int32
	C.tipRangeGravity(C.uintptr_t(r),
		(*C.int32_t)(unsafe.Pointer(&gravityStart)), (*C.int32_t)(unsafe.Pointer(&gravityEnd)))
	var anchor, length int32
	hr := C.tipRangeExtent(C.uintptr_t(r),
		(*C.int32_t)(unsafe.Pointer(&anchor)), (*C.int32_t)(unsafe.Pointer(&length)))
	if hr < 0 {
		// Not every context exposes an ACP based range; the gravity is still
		// worth having.
		strategyLog("StrategyTip %s composition units=%d gravity=%d,%d extent unavailable hr=0x%08x",
			when, units, gravityStart, gravityEnd, uint32(hr))
		return
	}
	strategyLog("StrategyTip %s composition units=%d extent=%d,%d gravity=%d,%d",
		when, units, anchor, length, gravityStart, gravityEnd)
}

// apply performs the composition work with a write cookie in hand.
func (s *editSessionImpl) apply(ec uintptr) bool {
	context := s.context
	strategyLog("StrategyTip editSession apply begin context=%#x ec=%#x text=%q commit=%v previousWritten=%q range=%#x", context, ec, s.text, s.commit, s.service.writtenComposition(), uintptr(s.service.composition.Load()))
	if context == 0 {
		return false
	}
	if s.caretRange != 0 {
		hr := C.tipContextSetSelection(C.uintptr_t(context), C.uint32_t(ec), C.uintptr_t(s.caretRange))
		if hr < 0 {
			strategyLog("StrategyTip placing the caret: SetSelection hr=0x%08x", uint32(hr))
			return false
		}
		return true
	}
	var contextComposition uintptr
	hr := C.tipQueryInterface(C.uintptr_t(context),
		(*C.GUID)(unsafe.Pointer(&iidContextComposition)), (*C.uintptr_t)(unsafe.Pointer(&contextComposition)))
	if hr < 0 || contextComposition == 0 {
		strategyLog("StrategyTip QueryInterface(ITfContextComposition) hr=0x%08x", uint32(hr))
		return false
	}
	defer comRelease(contextComposition)

	if s.text == "" {
		// No composition and no commit: take the composition off the screen.
		strategyLog("StrategyTip editSession clearing composition context=%#x previousWritten=%q range=%#x", context, s.service.writtenComposition(), uintptr(s.service.composition.Load()))
		s.service.endCompositionIn(ec)
		strategyLog("StrategyTip editSession cleared composition context=%#x rangeNow=%#x writtenNow=%q", context, uintptr(s.service.composition.Load()), s.service.writtenComposition())
		return true
	}

	composition := uintptr(s.service.composition.Load())
	if composition != 0 && !compositionIsAlive(composition) {
		// An application may end the composition behind our back. A dead
		// ITfComposition must not poison every later edit session.
		strategyLog("StrategyTip stale composition discarded composition=%#x", composition)
		comRelease(composition)
		composition = 0
		s.service.composition.Store(0)
	}

	if composition == 0 {
		var selection uintptr
		if C.tipContextGetSelectionRange(C.uintptr_t(context), C.uint32_t(ec),
			(*C.uintptr_t)(unsafe.Pointer(&selection))) < 0 || selection == 0 {
			strategyLog("StrategyTip GetSelection failed or returned no range")
			return false
		}
		sink := com.NewComObj[compositionSinkComObj](&compositionSinkImpl{service: s.service})
		var created uintptr
		hr = C.tipContextCompositionStart(C.uintptr_t(contextComposition), C.uint32_t(ec),
			C.uintptr_t(selection), C.uintptr_t(uintptr(unsafe.Pointer(sink))),
			(*C.uintptr_t)(unsafe.Pointer(&created)))
		comRelease(selection)
		sink.Release()
		if hr < 0 || created == 0 {
			strategyLog("StrategyTip StartComposition hr=0x%08x", uint32(hr))
			return false
		}
		s.service.composition.Store(created)
		s.service.storeWrittenComposition("")
		composition = created
		strategyLog("StrategyTip StartComposition ok composition=%#x", composition)
	}

	var comRange uintptr
	hr = C.tipCompositionGetRange(C.uintptr_t(composition),
		(*C.uintptr_t)(unsafe.Pointer(&comRange)))
	if hr < 0 || comRange == 0 {
		strategyLog("StrategyTip composition GetRange hr=0x%08x", uint32(hr))
		return false
	}
	// Put the range over the text the previous write left behind before it is
	// replaced. The very first write of a composition has nothing to replace,
	// and collapsing there would throw away the selection the user asked to
	// overwrite, so that case writes into the range untouched.
	if previous := s.service.writtenComposition(); previous != "" {
		if units := utf16Units(previous); units >= 0 {
			logCompositionRange(comRange, units, "before SetText")
			if coverCompositionRange(ec, comRange, units, "before SetText") {
				checkCompositionText(ec, comRange, previous, "before SetText")
			}
		}
	}
	wide, err := syscall.UTF16FromString(s.text)
	if err != nil {
		comRelease(comRange)
		strategyLog("StrategyTip composition text is not valid UTF-8: %v", err)
		return false
	}
	units := len(wide) - 1 // the terminator that UTF16FromString appends is not text
	hr = C.tipRangeSetText(C.uintptr_t(comRange), C.uint32_t(ec),
		(*C.wchar_t)(unsafe.Pointer(&wide[0])), C.int32_t(units))
	if hr < 0 {
		strategyLog("StrategyTip range SetText hr=0x%08x", uint32(hr))
		comRelease(comRange)
		return false
	}
	// Leave the range covering what was just written: the caret, the candidate
	// window and the next keystroke all read the composition from this range.
	coverCompositionRange(ec, comRange, units, "after SetText")
	checkCompositionText(ec, comRange, s.text, "after SetText")
	logCompositionRange(comRange, units, "after SetText")
	s.service.storeWrittenComposition(s.text)
	s.readCaret(ec, context, comRange)

	if s.commit {
		// Prepare the caret move now, but do not run SetSelection here: keyMu
		// is still held and the application could re-enter the TIP. The key
		// sink finishes the move once the lock is gone.
		s.placeSelectionAtRangeEnd(ec, comRange)
		if C.tipCompositionEnd(C.uintptr_t(composition), C.uint32_t(ec)) < 0 {
			strategyLog("StrategyTip EndComposition failed")
		}
		comRelease(composition)
		s.service.composition.Store(0)
		s.service.storeWrittenComposition("")
	}
	comRelease(comRange)
	return true
}

// readCaret measures the insertion point after the composition text changed.
// Applications that have not laid the text out yet fail on the collapsed range,
// so the whole composition rectangle is the fallback anchor, exactly like the
// C++ CaretProbeSession.
func (s *editSessionImpl) readCaret(ec uintptr, context uintptr, comRange uintptr) {
	var view uintptr
	if C.tipContextGetActiveView(C.uintptr_t(context),
		(*C.uintptr_t)(unsafe.Pointer(&view))) < 0 || view == 0 {
		return
	}
	defer comRelease(view)

	var end uintptr
	if C.tipRangeClone(C.uintptr_t(comRange),
		(*C.uintptr_t)(unsafe.Pointer(&end))) >= 0 && end != 0 {
		C.tipRangeCollapse(C.uintptr_t(end), C.uint32_t(ec), C.int32_t(tfAnchorEnd))
		var rect [4]int32
		var clipped int32
		if C.tipViewGetTextExt(C.uintptr_t(view), C.uint32_t(ec), C.uintptr_t(end),
			unsafe.Pointer(&rect[0]), (*C.int32_t)(unsafe.Pointer(&clipped))) >= 0 {
			s.caret = rect
		}
		comRelease(end)
	}
	if s.caret[2] <= s.caret[0] && s.caret[3] <= s.caret[1] {
		var rect [4]int32
		var clipped int32
		if C.tipViewGetTextExt(C.uintptr_t(view), C.uint32_t(ec), C.uintptr_t(comRange),
			unsafe.Pointer(&rect[0]), (*C.int32_t)(unsafe.Pointer(&clipped))) >= 0 {
			s.caret = rect
		}
	}
}

func compositionIsAlive(composition uintptr) bool {
	var probe uintptr
	hr := C.tipCompositionGetRange(C.uintptr_t(composition),
		(*C.uintptr_t)(unsafe.Pointer(&probe)))
	if hr < 0 || probe == 0 {
		return false
	}
	comRelease(probe)
	return true
}

// endCompositionIn runs inside an edit session and removes the composition.
// Ending a composition leaves its text in the document as ordinary text, so
// the letters have to be cleared first: a composition that ends on a cancel,
// on a key the engine did not take, or on a reply with no text must not leave
// the pinyin behind. A commit does the opposite and replaces the text itself.
func (t *tipImpl) endCompositionIn(ec uintptr) {
	composition := uintptr(t.composition.Load())
	if composition == 0 {
		return
	}
	var comRange uintptr
	if C.tipCompositionGetRange(C.uintptr_t(composition),
		(*C.uintptr_t)(unsafe.Pointer(&comRange))) >= 0 && comRange != 0 {
		// Place the range over the text that has to go first. Clearing a
		// collapsed range deletes nothing, which is how the pinyin survived a
		// cancel that was meant to erase it.
		if previous := t.writtenComposition(); previous != "" {
			if units := utf16Units(previous); units >= 0 {
				logCompositionRange(comRange, units, "before clearing")
				if coverCompositionRange(ec, comRange, units, "before clearing") {
					checkCompositionText(ec, comRange, previous, "before clearing")
				}
			}
		}
		empty := []uint16{0}
		if C.tipRangeSetText(C.uintptr_t(comRange), C.uint32_t(ec),
			(*C.wchar_t)(unsafe.Pointer(&empty[0])), 0) < 0 {
			strategyLog("StrategyTip clearing the composition text failed")
		}
		comRelease(comRange)
	}
	if C.tipCompositionEnd(C.uintptr_t(composition), C.uint32_t(ec)) < 0 {
		strategyLog("StrategyTip EndComposition on empty text failed")
	}
	comRelease(composition)
	t.composition.Store(0)
	t.storeWrittenComposition("")
}

// endComposition takes the composition down through its own edit session. The
// context must be the one the caller was handed: an ITfContext is only valid
// for the callback that owns it, so the stored pointer is never used here.
func (t *tipImpl) endComposition(context uintptr, text string) {
	if t.composition.Load() == 0 {
		return
	}
	if context == 0 {
		// Without a context there is no edit session, so the reference is
		// dropped and the next keystroke starts a new composition.
		t.dropComposition()
		return
	}
	t.runEditSession(context, "", false)
}

func (t *tipImpl) dropComposition() {
	composition := uintptr(t.composition.Swap(0))
	if composition != 0 {
		comRelease(composition)
	}
	// Whatever was written belongs to a composition that no longer exists, so
	// the next one starts with nothing to replace.
	t.storeWrittenComposition("")
}

// compositionSinkOnTerminated hears about a composition the application ended
// itself. It only records the fact: taking a lock here could deadlock against a
// key callback that is waiting for the application to accept its edit session.
func compositionSinkOnTerminated(this uintptr, _ uintptr, composition uintptr) uintptr {
	return guardedCall("ITfCompositionSink.OnCompositionTerminated", func() uintptr {
		sink := (*compositionSinkComObj)(unsafe.Pointer(this))
		impl := sink.Impl().(*compositionSinkImpl)
		if impl.service != nil {
			impl.service.compositionTerminated.Store(true)
			strategyLog("TSF OnCompositionTerminated composition=%#x", composition)
		}
		return hresultToUintptr(int32(win32.S_OK))
	})
}

var (
	procGetForegroundWindow = user32NewProc("GetForegroundWindow")
	procGetDpiForWindow     = user32NewProc("GetDpiForWindow")
)

func user32NewProc(name string) *syscall.LazyProc {
	return syscall.NewLazyDLL("user32.dll").NewProc(name)
}

func foregroundDpi() int {
	window, _, _ := procGetForegroundWindow.Call()
	if window == 0 {
		return 96
	}
	dpi, _, _ := procGetDpiForWindow.Call(window, 0)
	if dpi == 0 {
		return 96
	}
	return int(dpi)
}

// applyComposition writes one key reply into the document and reports where the
// caret ended up. It runs whatever the reply says: composition text while the
// candidate list is open, committed text when a candidate is chosen, and nothing
// when the composition ends.
func (t *tipImpl) applyComposition(context uintptr, reply string) [4]int32 {
	previous := t.writtenComposition()
	composition := replyString(reply, "composition")
	commit := replyString(reply, "commit")
	strategyLog("StrategyTip applyComposition begin context=%#x composition=%q commit=%q hasCommit=%v previousWritten=%q range=%#x", context, composition, commit, replyHas(reply, "commit"), previous, uintptr(t.composition.Load()))
	t.compositionContext.Store(context)
	if composition != "" {
		t.compositionTerminated.Store(false)
	}
	if t.compositionTerminated.Swap(false) {
		// The application ended the composition since the last key: the pointer
		// is stale and the edit session will start a new one.
		t.dropComposition()
	}
	return t.applyReply(context, reply)
}

// forgetComposition drops a composition whose key was not consumed, so the next
// keystroke starts from a clean slate.
func (t *tipImpl) forgetComposition(context uintptr) {
	if t.composition.Load() != 0 {
		t.endComposition(context, "")
	}
}

// candidatesPayload encodes the showCandidates body for one reply and caret.
// An empty string means the list has to be hidden instead: no candidates, a
// commit, or an empty composition.
func candidatesPayload(reply string, caret [4]int32) string {
	candidates := replyCandidates(reply)
	composition := replyString(reply, "composition")
	hasCommit := replyHas(reply, "commit")
	if len(candidates) == 0 || hasCommit || composition == "" {
		return ""
	}
	var list strings.Builder
	for i, candidate := range candidates {
		if i > 0 {
			list.WriteByte(',')
		}
		list.WriteByte('"')
		list.WriteString(jsonEscape(candidate))
		list.WriteByte('"')
	}
	return fmt.Sprintf(`,"candidates":[%s],"selection":%d,"caret":{"left":%d,"top":%d,"right":%d,"bottom":%d},"dpi":%d`,
		list.String(), replyUint(reply, "selectedCandidate"),
		caret[0], caret[1], caret[2], caret[3], foregroundDpi())
}

// notifyCandidates tells the Host to draw or hide the candidate list. The Host
// consumes these itself and never answers them. Whatever it draws is
// remembered, so focus returning to this application can restore the same list.
func (t *tipImpl) notifyCandidates(reply string, caret [4]int32) {
	payload := candidatesPayload(reply, caret)
	if payload == "" {
		t.shownCandidates.Store("")
		sendNotify("hideCandidates", "")
		return
	}
	t.shownCandidates.Store(payload)
	sendNotify("showCandidates", payload)
}

// restoreCandidates re-sends the remembered list. The Host draws it on a
// topmost window of its own, so it does not follow the caret: switching away
// hides it and only this can put it back. A composition that is no longer on
// screen owns no list, so nothing is sent for one.
func (t *tipImpl) restoreCandidates() {
	if t.composition.Load() == 0 || t.compositionContext.Load() == 0 {
		return
	}
	payload, _ := t.shownCandidates.Load().(string)
	if payload == "" {
		return
	}
	strategyLog("StrategyTip restoring candidates after focus")
	sendNotify("showCandidates", payload)
}

// sendNotify writes a notification that carries no reply. Only the id and the
// session are needed; the Host routes it by session and renders it itself.
func sendNotify(kind, payload string) {
	pipeMu.Lock()
	defer pipeMu.Unlock()
	if (pipeHandle == syscall.InvalidHandle || !pipeReady.Load()) && !connectLocked() {
		return
	}
	id := pipeRequestID.Add(1)
	line := fmt.Sprintf(`{"id":%d,"session":%d,"type":%q%s}`,
		id, strategySessionID(), kind, payload)
	if err := writeLine(pipeHandle, line); err != nil {
		strategyLog("StrategyTip notify failed id=%d type=%s err=%v", id, kind, err)
		return
	}
	strategyLog("StrategyTip sent notify id=%d type=%s", id, kind)
}

func jsonEscape(text string) string {
	var escaped strings.Builder
	for _, r := range text {
		switch r {
		case '"':
			escaped.WriteString("\\\"")
		case '\\':
			escaped.WriteString("\\\\")
		case '\n':
			escaped.WriteString("\\n")
		case '\r':
			escaped.WriteString("\\r")
		case '\t':
			escaped.WriteString("\\t")
		default:
			escaped.WriteRune(r)
		}
	}
	return escaped.String()
}

// --- Minimal JSON field readers. The Host's answers are produced by Node's
// JSON.stringify, which never adds spaces, and this DLL is the only producer of
// the requests, so the fields are located by text like the Host does.

func replyHas(line, field string) bool {
	return strings.Contains(line, "\""+field+"\":")
}

func replyString(line, field string) string {
	needle := "\"" + field + "\":\""
	index := strings.Index(line, needle)
	if index < 0 {
		return ""
	}
	rest := line[index+len(needle):]
	// The closing quote is the first quote that is not part of an escape, or a
	// value could not contain a quoted string.
	for i := 0; i < len(rest); i++ {
		switch rest[i] {
		case '\\':
			i++
		case '"':
			return unquoteJSONString(rest[:i])
		}
	}
	return ""
}

// unquoteJSONString decodes the escape sequences a JSON string value may carry.
// Node escapes only what it must, and composition text is pinyin, so the common
// case is a verbatim copy.
func unquoteJSONString(text string) string {
	if !strings.ContainsRune(text, '\\') {
		return text
	}
	if unquoted, err := strconv.Unquote("\"" + text + "\""); err == nil {
		return unquoted
	}
	return text
}

func replyUint(line, field string) int {
	needle := "\"" + field + "\":"
	index := strings.Index(line, needle)
	if index < 0 {
		return 0
	}
	rest := line[index+len(needle):]
	value := 0
	for i := 0; i < len(rest) && rest[i] >= '0' && rest[i] <= '9'; i++ {
		value = value*10 + int(rest[i]-'0')
	}
	return value
}

// replyCandidates reads the candidate texts. Node sends objects with a text
// field, and the Host accepts a plain string array, so this extracts the text
// values and the caller re-encodes them.
func replyCandidates(line string) []string {
	start := strings.Index(line, "\"candidates\":[")
	if start < 0 {
		return nil
	}
	rest := line[start+len("\"candidates\":["):]
	if end := strings.IndexByte(rest, ']'); end >= 0 {
		rest = rest[:end]
	}
	var candidates []string
	for {
		index := strings.Index(rest, "\"text\":\"")
		if index < 0 {
			return candidates
		}
		rest = rest[index+len("\"text\":\""):]
		end := strings.IndexByte(rest, '"')
		if end < 0 {
			return candidates
		}
		candidates = append(candidates, unquoteJSONString(rest[:end]))
		rest = rest[end:]
	}
}
