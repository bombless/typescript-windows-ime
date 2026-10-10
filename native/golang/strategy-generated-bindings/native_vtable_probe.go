package tsfdecl

/*
#include <stdint.h>

// Calls the actual five-slot COM vtable from native C code. On Windows amd64
// the platform ABI has a single calling convention; this probe does not claim
// 386 runtime coverage.
static int32_t native_call_processor_vtable(void *obj, const void *iid,
    uint32_t client_id, int32_t *activate_hr, int32_t *deactivate_hr,
    uint32_t *ref_after_add, uint32_t *ref_after_release) {
    void **vtbl = *(void ***)obj;
    int32_t (*query_interface)(void *, const void *, void **) =
        (int32_t (*)(void *, const void *, void **))vtbl[0];
    uint32_t (*add_ref)(void *) = (uint32_t (*)(void *))vtbl[1];
    uint32_t (*release)(void *) = (uint32_t (*)(void *))vtbl[2];
    int32_t (*activate)(void *, void *, uintptr_t) =
        (int32_t (*)(void *, void *, uintptr_t))vtbl[3];
    int32_t (*deactivate)(void *) = (int32_t (*)(void *))vtbl[4];
    void *queried = 0;
    int32_t hr = query_interface(obj, iid, &queried);
    if (hr != 0 || queried != obj) return hr ? hr : (int32_t)0x80004005u;
    *ref_after_add = add_ref(obj);
    *ref_after_release = release(obj);
    *activate_hr = activate(obj, 0, client_id);
    *deactivate_hr = deactivate(obj);
    release(queried);
    return 0;
}
*/
import "C"

import "unsafe"

type nativeVtableProbeResult struct {
	queryInterface  HRESULT
	activate        HRESULT
	deactivate      HRESULT
	refAfterAdd     uint32
	refAfterRelease uint32
}

func invokeNativeVtable(obj unsafe.Pointer) nativeVtableProbeResult {
	var result nativeVtableProbeResult
	var activateHR, deactivateHR C.int32_t
	var refAfterAdd, refAfterRelease C.uint32_t
	result.queryInterface = HRESULT(C.native_call_processor_vtable(
		obj,
		unsafe.Pointer(&IID_ITfTextInputProcessor),
		42,
		&activateHR,
		&deactivateHR,
		&refAfterAdd,
		&refAfterRelease,
	))
	result.activate = HRESULT(activateHR)
	result.deactivate = HRESULT(deactivateHR)
	result.refAfterAdd = uint32(refAfterAdd)
	result.refAfterRelease = uint32(refAfterRelease)
	return result
}
