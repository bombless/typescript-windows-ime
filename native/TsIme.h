#pragma once

#include <windows.h>
#include <msctf.h>

// Keep the MSI Release TIP and the manually installed Dev TIP independent.
// Each flavor must have its own CLSID and language-profile GUID.
#ifdef TSIME_RELEASE
// Release CLSID: {A6C63D91-DF29-4D11-8F9A-21DCE63B4E10}
DEFINE_GUID(CLSID_TypeScriptWindowsIme,
    0xa6c63d91, 0xdf29, 0x4d11, 0x8f, 0x9a, 0x21, 0xdc, 0xe6, 0x3b, 0x4e, 0x10);
// Release profile: {F2E94A73-8D2B-4F6C-AE31-5D7C903B12A8}
DEFINE_GUID(GUID_TypeScriptWindowsImeProfile,
    0xf2e94a73, 0x8d2b, 0x4f6c, 0xae, 0x31, 0x5d, 0x7c, 0x90, 0x3b, 0x12, 0xa8);
constexpr wchar_t kTypeScriptWindowsImeName[] = L"TypeScript Windows IME Release";
#else
// Dev CLSID: {7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}
DEFINE_GUID(CLSID_TypeScriptWindowsIme,
    0x7b2e4f5a, 0x3a8e, 0x4d74, 0x9f, 0x0b, 0x6d, 0x5d, 0x6f, 0x0e, 0x6c, 0x41);
// Dev profile: {C0A3B5B1-3C52-4E8E-A8A7-1F2B3D4C5E60}
DEFINE_GUID(GUID_TypeScriptWindowsImeProfile,
    0xc0a3b5b1, 0x3c52, 0x4e8e, 0xa8, 0xa7, 0x1f, 0x2b, 0x3d, 0x4c, 0x5e, 0x60);
constexpr wchar_t kTypeScriptWindowsImeName[] = L"TypeScript Windows IME Dev";
#endif
