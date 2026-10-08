#define INITGUID
#include "TsIme.h"

#include <windows.h>
#include <msctf.h>
#include <cstdio>
#include <string>

namespace {
std::wstring GuidString(REFGUID guid) {
    wchar_t buffer[64]{};
    StringFromGUID2(guid, buffer, ARRAYSIZE(buffer));
    return buffer;
}

void PrintHr(const char* label, HRESULT hr) {
    std::printf("%-42s 0x%08lX %s\n", label, static_cast<unsigned long>(hr),
        SUCCEEDED(hr) ? "OK" : "FAIL");
}

void PrintRegistryValue(HKEY root, const wchar_t* path, REGSAM view, const char* label) {
    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | view, &key);
    if (openResult != ERROR_SUCCESS) {
        std::printf("REG %-12s MISSING (win32=%ld)\n", label, openResult);
        return;
    }
    wchar_t value[1024]{};
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    const LONG result = RegQueryValueExW(key, nullptr, nullptr, &type,
        reinterpret_cast<BYTE*>(value), &bytes);
    if (result == ERROR_SUCCESS && type == REG_SZ) {
        std::wprintf(L"REG %-12S = %ls\n", label, value);
    } else {
        std::printf("REG %-12s MISSING (win32=%ld)\n", label, result);
    }
    RegCloseKey(key);
}

void PrintRegistryDword(HKEY root, const wchar_t* path, REGSAM view,
                        const wchar_t* valueName, const char* label) {
    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | view, &key);
    if (openResult != ERROR_SUCCESS) {
        std::printf("REG %-12s MISSING (win32=%ld)\n", label, openResult);
        return;
    }
    DWORD value = 0, type = 0, bytes = sizeof(value);
    const LONG result = RegQueryValueExW(key, valueName, nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &bytes);
    if (result == ERROR_SUCCESS && type == REG_DWORD) {
        std::printf("REG %-12s = %lu (0x%08lX)\n", label,
            static_cast<unsigned long>(value), static_cast<unsigned long>(value));
    } else {
        std::printf("REG %-12s MISSING (win32=%ld)\n", label, result);
    }
    RegCloseKey(key);
}

void PrintRegistry(HKEY root, const wchar_t* rootName, REGSAM view, const char* viewName) {
    const wchar_t* clsid = L"{7B2E4F5A-3A8E-4D74-9F0B-6D5D6F0E6C41}";
    const wchar_t* profile = L"{C0A3B5B1-3C52-4E8E-A8A7-1F2B3D4C5E60}";
    const wchar_t* languages[] = {L"00000409", L"00000804", L"00000411"};
    std::wprintf(L"\n[%ls %S]\n", rootName, viewName);
    const std::wstring com = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + clsid;
    PrintRegistryValue(root, (com + L"\\InprocServer32").c_str(), view, "COM InprocServer32");
    for (const wchar_t* lang : languages) {
        const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\CTF\\TIP\\") +
            clsid + L"\\LanguageProfile\\0x" + lang + L"\\" + profile;
        std::string label = "Enable ";
        label += (lang == languages[0] ? "0409" : lang == languages[1] ? "0804" : "0411");
        PrintRegistryDword(root, key.c_str(), view, L"Enable", label.c_str());
    }
}

void EnumerateLanguageProfiles(ITfInputProcessorProfiles* profiles, LANGID langid) {
    IEnumTfLanguageProfiles* enumerator = nullptr;
    const HRESULT hr = profiles->EnumLanguageProfiles(langid, &enumerator);
    char label[64]{};
    std::snprintf(label, sizeof(label), "EnumLanguageProfiles 0x%04X", langid);
    PrintHr(label, hr);
    if (FAILED(hr)) return;
    ULONG fetched = 0;
    TF_LANGUAGEPROFILE profile{};
    while (enumerator->Next(1, &profile, &fetched) == S_OK && fetched == 1) {
        std::printf("  PROFILE lang=0x%04X active=%d\n",
            profile.langid, profile.fActive ? 1 : 0);
        std::wprintf(L"    clsid=%ls\n    catid=%ls\n    profile=%ls\n",
            GuidString(profile.clsid).c_str(), GuidString(profile.catid).c_str(),
            GuidString(profile.guidProfile).c_str());
        if (profile.clsid == CLSID_TypeScriptWindowsIme)
            std::printf("    ** THIS IS TYPESCRIPT WINDOWS IME **\n");
    }
    enumerator->Release();
}

void CheckComActivation() {
    std::printf("\n[COM ACTIVATION]\n");
    ITfTextInputProcessor* tip = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_TypeScriptWindowsIme, nullptr,
        CLSCTX_INPROC_SERVER, IID_ITfTextInputProcessor, reinterpret_cast<void**>(&tip));
    PrintHr("CoCreateInstance(TypeScriptWindowsIme)", hr);
    if (SUCCEEDED(hr)) tip->Release();
}

void CheckActiveAndDefaultProfiles(ITfInputProcessorProfiles* profiles) {
    std::printf("\n[ACTIVE / DEFAULT PROFILE]\n");
    CLSID defaultClsid{};
    GUID defaultProfile{};
    HRESULT hr = profiles->GetDefaultLanguageProfile(
        0x0804, GUID_TFCAT_TIP_KEYBOARD, &defaultClsid, &defaultProfile);
    PrintHr("GetDefaultLanguageProfile(0x0804)", hr);
    if (SUCCEEDED(hr)) {
        std::wprintf(L"  default clsid=%ls profile=%ls\n",
            GuidString(defaultClsid).c_str(), GuidString(defaultProfile).c_str());
    }

    ITfInputProcessorProfileMgr* mgr = nullptr;
    hr = profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,
        reinterpret_cast<void**>(&mgr));
    PrintHr("QI(ITfInputProcessorProfileMgr)", hr);
    if (SUCCEEDED(hr)) {
        TF_INPUTPROCESSORPROFILE active{};
        hr = mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active);
        PrintHr("GetActiveProfile(TIP_KEYBOARD)", hr);
        if (SUCCEEDED(hr)) {
            std::wprintf(L"  active lang=0x%04X clsid=%ls profile=%ls\n",
                active.langid, GuidString(active.clsid).c_str(),
                GuidString(active.guidProfile).c_str());
        }
        mgr->Release();
    }
}
}

int wmain(int argc, wchar_t** argv) {
    const bool activateTest = argc > 1 && wcscmp(argv[1], L"--activate") == 0;
    const bool restoreTest = argc > 1 && wcscmp(argv[1], L"--restore") == 0;
    std::printf("=== TypeScript Windows IME / TSF Diagnostic ===\n");
    std::wprintf(L"CLSID        %ls\n", GuidString(CLSID_TypeScriptWindowsIme).c_str());
    std::wprintf(L"Profile GUID %ls\n", GuidString(GUID_TypeScriptWindowsImeProfile).c_str());
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PrintHr("CoInitializeEx", init);
    const bool uninit = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return 1;

    PrintRegistry(HKEY_LOCAL_MACHINE, L"HKLM", KEY_WOW64_64KEY, "64-bit");
    PrintRegistry(HKEY_CURRENT_USER, L"HKCU", KEY_WOW64_64KEY, "64-bit");

    std::printf("\n[TSF PROFILES]\n");
    ITfInputProcessorProfiles* profiles = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
        CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles, reinterpret_cast<void**>(&profiles));
    PrintHr("CoCreateInstance(TF_InputProcessorProfiles)", hr);
    if (SUCCEEDED(hr)) {
        EnumerateLanguageProfiles(profiles, 0x0409);
        EnumerateLanguageProfiles(profiles, 0x0804);
        EnumerateLanguageProfiles(profiles, 0x0411);
        CheckActiveAndDefaultProfiles(profiles);
        if (activateTest) {
            std::printf("\n[EXPLICIT ACTIVATION TEST]\n");
            hr = profiles->ActivateLanguageProfile(
                CLSID_TypeScriptWindowsIme, 0x0804, GUID_TypeScriptWindowsImeProfile);
            PrintHr("ActivateLanguageProfile(TypeScript, 0x0804)", hr);
            CheckActiveAndDefaultProfiles(profiles);
        }
        if (restoreTest) {
            std::printf("\n[RESTORE ORIGINAL PROFILE]\n");
            CLSID pinyinClsid{};
            GUID pinyinProfile{};
            pinyinClsid = GUID{0x81d4e9c9, 0x1d3b, 0x41bc, {0x9e, 0x6c, 0x4b, 0x40, 0xbf, 0x79, 0xe3, 0x5e}};
            pinyinProfile = GUID{0xfa550b04, 0x5ad7, 0x411f, {0xa5, 0xac, 0xca, 0x03, 0x8e, 0xc5, 0x15, 0xd7}};
            hr = profiles->ActivateLanguageProfile(pinyinClsid, 0x0804, pinyinProfile);
            PrintHr("ActivateLanguageProfile(Microsoft Pinyin, 0x0804)", hr);
            CheckActiveAndDefaultProfiles(profiles);
        }
        profiles->Release();
    }
    CheckComActivation();
    if (uninit) CoUninitialize();
    std::printf("\n=== Diagnostic complete ===\n");
    return 0;
}