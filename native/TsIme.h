#pragma once

#include <windows.h>
#include <msctf.h>

// Development-only identifiers for the TypeScript Windows IME TIP.
// {7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}
DEFINE_GUID(CLSID_TypeScriptWindowsIme,
    0x7b2e4f5a, 0x3a8e, 0x4d74, 0x9f, 0x0b, 0x6d, 0x5d, 0x6f, 0x0e, 0x6c, 0x41);

// {C0A3B5B1-3C52-4E8E-A8A7-1F2B3D4C5E60}
DEFINE_GUID(GUID_TypeScriptWindowsImeProfile,
    0xc0a3b5b1, 0x3c52, 0x4e8e, 0xa8, 0xa7, 0x1f, 0x2b, 0x3d, 0x4c, 0x5e, 0x60);

constexpr wchar_t kTypeScriptWindowsImeName[] = L"TypeScript Windows IME";
