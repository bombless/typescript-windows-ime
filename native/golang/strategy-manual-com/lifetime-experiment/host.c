#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

typedef int32_t (*can_unload_fn)(void);
typedef uintptr_t (*get_callback_fn)(void);
typedef void (*force_gc_fn)(void);
typedef uintptr_t (__stdcall *callback_fn)(uintptr_t);

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: LifetimeHost.exe <dll-path>\n");
        return 2;
    }
    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 3;
    }
    can_unload_fn can_unload = (can_unload_fn)GetProcAddress(module, "DllCanUnloadNow");
    get_callback_fn get_callback = (get_callback_fn)GetProcAddress(module, "ManualComGetCallback");
    force_gc_fn force_gc = (force_gc_fn)GetProcAddress(module, "ManualComForceGC");
    if (!can_unload || !get_callback || !force_gc) {
        fprintf(stderr, "required DLL export missing\n");
        return 4;
    }
    if (can_unload() != 1) {
        fprintf(stderr, "DllCanUnloadNow must return S_FALSE (1)\n");
        return 5;
    }
    uintptr_t address = get_callback();
    callback_fn callback = (callback_fn)address;
    if (!callback) {
        fprintf(stderr, "callback thunk address is null\n");
        return 6;
    }
    for (uintptr_t i = 0; i < 10000; ++i) {
        if (callback(i) != i + 0x1234) {
            fprintf(stderr, "callback mismatch at %" PRIuPTR "\n", i);
            return 7;
        }
    }
    force_gc();
    if (callback(7) != 7 + 0x1234 || can_unload() != 1) {
        fprintf(stderr, "post-GC callback or unload-policy check failed\n");
        return 8;
    }
    printf("PASS: loaded Go DLL; DllCanUnloadNow=S_FALSE; callback=10001 calls; post-GC call passed; module intentionally left loaded\n");
    /* Intentionally do not call FreeLibrary: runtime/thunk unload is unproven. */
    return 0;
}