//go:build ignore

/* Load the text-service DLL and call its registration export.
   regsvr32 exits 0xC0000409 after this Go DLL returns, so the installer
   uses this host and leaves the module mapped until the process ends. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int32_t (*RegistrationFn)(void);

int main(int argc, char** argv) {
    wchar_t path[1024];
    const char* exportName;
    HMODULE module;
    RegistrationFn fn;
    int32_t hr;
    if (argc != 3) {
        fprintf(stderr, "usage: registerhost register|unregister <dll>\n");
        return 2;
    }
    if (strcmp(argv[1], "register") == 0) exportName = "DllRegisterServer";
    else if (strcmp(argv[1], "unregister") == 0) exportName = "DllUnregisterServer";
    else {
        fprintf(stderr, "unknown action: %s\n", argv[1]);
        return 2;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, path, 1024) == 0) {
        fprintf(stderr, "path conversion failed: %lu\n", GetLastError());
        return 2;
    }
    module = LoadLibraryW(path);
    if (!module) {
        fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    fn = (RegistrationFn)GetProcAddress(module, exportName);
    if (!fn) {
        fprintf(stderr, "GetProcAddress(%s) failed: %lu\n", exportName, GetLastError());
        return 1;
    }
    hr = fn();
    fprintf(stderr, "%s hr 0x%08lX\n", exportName, (unsigned long)(uint32_t)hr);
    return hr == 0 ? 0 : 1;
}
