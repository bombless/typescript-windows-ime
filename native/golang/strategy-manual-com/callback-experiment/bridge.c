#include <stdint.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>

typedef uintptr_t (__stdcall *callback_fn)(uintptr_t);
typedef uintptr_t (__stdcall *query_interface_fn)(uintptr_t, uintptr_t, uintptr_t);
typedef struct IUnknownLike IUnknownLike;
typedef struct IUnknownLikeVTable {
    query_interface_fn QueryInterface;
    callback_fn AddRef;
    callback_fn Release;
} IUnknownLikeVTable;
struct IUnknownLike {
    IUnknownLikeVTable *lpVtbl;
    volatile LONG refs;
};

static const unsigned char IID_IUNKNOWN_BYTES[16] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46
};

IUnknownLike *create_unknown_like(uintptr_t query_interface, uintptr_t add_ref, uintptr_t release) {
    IUnknownLike *obj = (IUnknownLike*)calloc(1, sizeof(IUnknownLike));
    IUnknownLikeVTable *vtbl = (IUnknownLikeVTable*)calloc(1, sizeof(IUnknownLikeVTable));
    if (!obj || !vtbl) { free(obj); free(vtbl); return NULL; }
    vtbl->QueryInterface = (query_interface_fn)query_interface;
    vtbl->AddRef = (callback_fn)add_ref;
    vtbl->Release = (callback_fn)release;
    obj->lpVtbl = vtbl;
    obj->refs = 1;
    return obj;
}

uintptr_t call_query_interface(IUnknownLike *obj, const unsigned char iid[16], uintptr_t *out) {
    return obj->lpVtbl->QueryInterface((uintptr_t)obj, (uintptr_t)iid, (uintptr_t)out);
}
uintptr_t call_add_ref(IUnknownLike *obj) { return obj->lpVtbl->AddRef((uintptr_t)obj); }
uintptr_t call_release(IUnknownLike *obj) { return obj->lpVtbl->Release((uintptr_t)obj); }
LONG object_refcount(IUnknownLike *obj) { return obj->refs; }
LONG object_add_ref(IUnknownLike *obj) { return InterlockedIncrement(&obj->refs); }
LONG object_release_ref(IUnknownLike *obj) { return InterlockedDecrement(&obj->refs); }
void destroy_unknown_like(IUnknownLike *obj) { if (obj) { free(obj->lpVtbl); free(obj); } }
void copy_iid_iunknown(unsigned char out[16]) { memcpy(out, IID_IUNKNOWN_BYTES, 16); }
int iid_pointer_is_iunknown(uintptr_t iid) { return iid && memcmp((const void*)iid, IID_IUNKNOWN_BYTES, 16) == 0; }
void write_pointer(uintptr_t address, uintptr_t value) { if (address) *(uintptr_t*)address = value; }
LONG object_add_ref_from_uintptr(uintptr_t obj) { return InterlockedIncrement(&((IUnknownLike*)obj)->refs); }
LONG object_release_ref_from_uintptr(uintptr_t obj) { return InterlockedDecrement(&((IUnknownLike*)obj)->refs); }

uintptr_t invoke_callback(uintptr_t callback, uintptr_t value) {
    callback_fn fn = (callback_fn)(uintptr_t)callback;
    return fn(value);
}

typedef struct CallbackStressArgs {
    callback_fn fn;
    int iterations;
    volatile LONG failures;
} CallbackStressArgs;

static DWORD WINAPI callback_stress_thread(LPVOID raw) {
    CallbackStressArgs *args = (CallbackStressArgs*)raw;
    for (int i = 0; i < args->iterations; ++i) {
        uintptr_t value = (uintptr_t)i;
        if (args->fn(value) != value + 0x1234) InterlockedIncrement(&args->failures);
    }
    return 0;
}

int stress_callback_concurrency(uintptr_t callback, int thread_count, int iterations) {
    if (thread_count < 1 || thread_count > 32 || iterations < 1) return -1;
    HANDLE threads[32] = {0};
    CallbackStressArgs args[32];
    callback_fn fn = (callback_fn)(uintptr_t)callback;
    int created = 0;
    for (int i = 0; i < thread_count; ++i) {
        args[i].fn = fn;
        args[i].iterations = iterations;
        args[i].failures = 0;
        threads[i] = CreateThread(NULL, 0, callback_stress_thread, &args[i], 0, NULL);
        if (!threads[i]) break;
        ++created;
    }
    if (created != thread_count) {
        for (int i = 0; i < created; ++i) WaitForSingleObject(threads[i], INFINITE);
        for (int i = 0; i < created; ++i) CloseHandle(threads[i]);
        return -2;
    }
    WaitForMultipleObjects((DWORD)thread_count, threads, TRUE, INFINITE);
    int failures = 0;
    for (int i = 0; i < thread_count; ++i) {
        failures += (int)args[i].failures;
        CloseHandle(threads[i]);
    }
    return failures;
}
