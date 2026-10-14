#include "registration_txn.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum Step {
    StepProbe = 0,
    StepName,
    StepModule,
    StepThreading,
    StepRegister,
    StepAdd0409,
    StepEnable0409,
    StepAdd0804,
    StepEnable0804,
    StepAdd0411,
    StepEnable0411,
    StepCategory,
    StepCount
};

struct Report {
    std::wstring operation;
    HRESULT original;
    HRESULT cleanup;
};

struct Fake {
    std::vector<std::string> calls;
    std::vector<LANGID> langs;
    std::vector<Report> reports;
    int failAt = -1;
    bool preexisting = false;
    int step = 0;
    HRESULT failCode = E_ACCESSDENIED;
    HRESULT cleanupCategory = S_OK;
    HRESULT cleanupProfile = S_OK;
    HRESULT cleanupClass = S_OK;
    bool strategyCom = false;
    bool strategyProfile = false;
    bool strategyCategory = false;
    bool unrelatedCom = true;
    bool orphanTsf = true;
};

const char* kForward[StepCount] = {
    "probe", "name", "module", "threading", "register",
    "add", "enable", "add", "enable", "add", "enable", "category"
};

HRESULT FailureCode(int step) {
    return MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, static_cast<USHORT>(0x800 + step));
}

HRESULT Forward(Fake* fake, const char* name) {
    const int index = fake->step++;
    fake->calls.push_back(name);
    if (index == fake->failAt) return fake->failCode;
    return S_OK;
}

HRESULT Probe(void* context) {
    Fake* fake = static_cast<Fake*>(context);
    const HRESULT hr = Forward(fake, "probe");
    if (fake->preexisting) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    return hr;
}

HRESULT WriteName(void* context) {
    Fake* fake = static_cast<Fake*>(context);
    const HRESULT hr = Forward(fake, "name");
    fake->strategyCom = true;
    return hr;
}

HRESULT WriteModule(void* context) { return Forward(static_cast<Fake*>(context), "module"); }
HRESULT WriteThreading(void* context) { return Forward(static_cast<Fake*>(context), "threading"); }

HRESULT RegisterProfile(void* context) {
    Fake* fake = static_cast<Fake*>(context);
    const HRESULT hr = Forward(fake, "register");
    if (SUCCEEDED(hr)) fake->strategyProfile = true;
    return hr;
}

HRESULT AddLanguage(void* context, LANGID lang) {
    Fake* fake = static_cast<Fake*>(context);
    const HRESULT hr = Forward(fake, "add");
    fake->langs.push_back(lang);
    return hr;
}

HRESULT EnableLanguage(void* context, LANGID lang) {
    Fake* fake = static_cast<Fake*>(context);
    const HRESULT hr = Forward(fake, "enable");
    fake->langs.push_back(lang);
    return hr;
}

HRESULT RegisterCategory(void* context) {
    Fake* fake = static_cast<Fake*>(context);
    const HRESULT hr = Forward(fake, "category");
    if (SUCCEEDED(hr)) fake->strategyCategory = true;
    return hr;
}

HRESULT UnregisterCategory(void* context) {
    Fake* fake = static_cast<Fake*>(context);
    fake->calls.push_back("unregisterCategory");
    if (!fake->strategyCategory) return S_OK;
    if (FAILED(fake->cleanupCategory)) return fake->cleanupCategory;
    fake->strategyCategory = false;
    return S_OK;
}

HRESULT UnregisterProfiles(void* context) {
    Fake* fake = static_cast<Fake*>(context);
    fake->calls.push_back("unregisterProfiles");
    if (!fake->strategyProfile) return S_OK;
    if (FAILED(fake->cleanupProfile)) return fake->cleanupProfile;
    fake->strategyProfile = false;
    return S_OK;
}

HRESULT DeleteClass(void* context) {
    Fake* fake = static_cast<Fake*>(context);
    fake->calls.push_back("deleteClass");
    if (!fake->strategyCom) return S_OK;
    if (FAILED(fake->cleanupClass)) return fake->cleanupClass;
    fake->strategyCom = false;
    return S_OK;
}

void ReportCleanup(void* context, const wchar_t* operation, HRESULT original, HRESULT cleanup) {
    Fake* fake = static_cast<Fake*>(context);
    fake->calls.push_back("report");
    Report report;
    report.operation = operation ? operation : L"";
    report.original = original;
    report.cleanup = cleanup;
    fake->reports.push_back(report);
}

const StrategyRegistrationOps kOps = {
    Probe, WriteName, WriteModule, WriteThreading, RegisterProfile,
    AddLanguage, EnableLanguage, RegisterCategory, UnregisterCategory,
    UnregisterProfiles, DeleteClass, ReportCleanup
};

const wchar_t kValidPath[] = L"C:\\strategy\\TypeScriptWindowsImeGoCom.dll";

bool Check(bool value, const char* message) {
    if (!value) std::printf("FAIL: %s\n", message);
    return value;
}

void DumpCalls(const Fake& fake) {
    std::printf("  calls:");
    for (size_t i = 0; i < fake.calls.size(); ++i) std::printf(" %s", fake.calls[i].c_str());
    std::printf("\n");
}

bool SameCalls(const Fake& fake, const std::vector<std::string>& expected, const char* label) {
    if (fake.calls.size() != expected.size()) {
        std::printf("FAIL: %s call count %zu != %zu\n", label, fake.calls.size(), expected.size());
        DumpCalls(fake);
        return false;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (fake.calls[i] != expected[i]) {
            std::printf("FAIL: %s call %zu is %s, expected %s\n",
                        label, i, fake.calls[i].c_str(), expected[i].c_str());
            DumpCalls(fake);
            return false;
        }
    }
    return true;
}

bool PreservesNeighbors(const Fake& fake, const char* label) {
    return Check(fake.unrelatedCom, label) && Check(fake.orphanTsf, label);
}

std::vector<std::string> ExpectedRegistration(int failAt, bool withReports) {
    std::vector<std::string> expected;
    const int last = failAt < 0 ? StepCount - 1 : failAt;
    for (int i = 0; i <= last && i < StepCount; ++i) expected.push_back(kForward[i]);
    if (failAt < 0) return expected;
    const bool createdProfile = failAt > StepRegister;
    const bool createdClass = failAt >= StepName;
    if (createdProfile) {
        expected.push_back("unregisterProfiles");
        if (withReports) expected.push_back("report");
    }
    if (createdClass) {
        expected.push_back("deleteClass");
        if (withReports) expected.push_back("report");
    }
    return expected;
}

std::vector<LANGID> ExpectedLangs(int failAt) {
    std::vector<LANGID> langs;
    const int last = failAt < 0 ? StepEnable0411 : failAt;
    for (int step = StepAdd0409; step <= last; ++step) {
        if (step >= StepAdd0409 && step <= StepEnable0409) langs.push_back(0x0409);
        if (step >= StepAdd0804 && step <= StepEnable0804) langs.push_back(0x0804);
        if (step >= StepAdd0411 && step <= StepEnable0411) langs.push_back(0x0411);
    }
    return langs;
}

bool LangsMatch(const Fake& fake, const std::vector<LANGID>& expected) {
    if (fake.langs.size() != expected.size()) {
        std::printf("FAIL: language count %zu != %zu\n", fake.langs.size(), expected.size());
        return false;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (fake.langs[i] != expected[i]) {
            std::printf("FAIL: language %zu is 0x%04X\n", i, fake.langs[i]);
            return false;
        }
    }
    return true;
}

HRESULT RunRegistration(Fake* fake) {
    return StrategyRunPreparedRegistration(
        kValidPath, 260, wcslen(kValidPath), &kOps, fake);
}

bool IdentityMatches() {
    bool ok = true;
    ok &= Check(kStrategyAClsid.Data1 == 0xA4C7D921 && kStrategyAClsid.Data2 == 0x6B35 &&
                    kStrategyAClsid.Data3 == 0x4E8A && kStrategyAClsid.Data4[0] == 0x91 &&
                    kStrategyAClsid.Data4[1] == 0xF2 && kStrategyAClsid.Data4[2] == 0x7D &&
                    kStrategyAClsid.Data4[3] == 0x0B && kStrategyAClsid.Data4[4] == 0x6C &&
                    kStrategyAClsid.Data4[5] == 0x3E && kStrategyAClsid.Data4[6] == 0x5A &&
                    kStrategyAClsid.Data4[7] == 0x14,
                "Strategy A CLSID");
    ok &= Check(kStrategyAProfile.Data1 == 0xE8B1C4D7 && kStrategyAProfile.Data2 == 0x2A63 &&
                    kStrategyAProfile.Data3 == 0x49F0 && kStrategyAProfile.Data4[0] == 0xB5 &&
                    kStrategyAProfile.Data4[1] == 0xD8 && kStrategyAProfile.Data4[2] == 0x1C &&
                    kStrategyAProfile.Data4[3] == 0x7E && kStrategyAProfile.Data4[4] == 0x3A &&
                    kStrategyAProfile.Data4[5] == 0x9F && kStrategyAProfile.Data4[6] == 0x62 &&
                    kStrategyAProfile.Data4[7] == 0x40,
                "Strategy A profile GUID");
    ok &= Check(wcscmp(kStrategyAName, L"strategy-go-com") == 0, "display name");
    ok &= Check(kStrategyLanguageCount == 3 && kStrategyLanguageIds[0] == 0x0409 &&
                    kStrategyLanguageIds[1] == 0x0804 && kStrategyLanguageIds[2] == 0x0411,
                "language ids");
    return ok;
}

bool FailureInjection() {
    bool ok = true;
    for (int failAt = 0; failAt < StepCount; ++failAt) {
        Fake fake;
        fake.failAt = failAt;
        fake.failCode = FailureCode(failAt);
        const HRESULT hr = RunRegistration(&fake);
        ok &= Check(hr == fake.failCode, "original operation HRESULT preserved");
        ok &= SameCalls(fake, ExpectedRegistration(failAt, false), "failure injection");
        ok &= LangsMatch(fake, ExpectedLangs(failAt));
        ok &= Check(!fake.strategyCom && !fake.strategyProfile && !fake.strategyCategory,
                    "rollback removes only artifacts created by this attempt");
        ok &= PreservesNeighbors(fake, "failure injection preserves unrelated and orphan metadata");
        ok &= Check(fake.reports.empty(), "successful cleanup does not emit diagnostics");
        if (failAt <= StepRegister) {
            ok &= Check(std::find(fake.calls.begin(), fake.calls.end(), "unregisterProfiles") == fake.calls.end(),
                        "failed Register does not unregister profiles");
        }
        if (failAt <= StepCategory) {
            ok &= Check(std::find(fake.calls.begin(), fake.calls.end(), "unregisterCategory") == fake.calls.end(),
                        "failed RegisterCategory does not unregister that category");
        }
    }
    return ok;
}

bool CleanupFailures() {
    bool ok = true;
    Fake both;
    both.failAt = StepCategory;
    both.failCode = FailureCode(StepCategory);
    both.cleanupProfile = E_FAIL;
    both.cleanupClass = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    HRESULT hr = RunRegistration(&both);
    ok &= Check(hr == both.failCode, "cleanup failures must not replace the original HRESULT");
    ok &= SameCalls(both, ExpectedRegistration(StepCategory, true), "both cleanups run and report");
    ok &= Check(both.reports.size() == 2, "one diagnostic per failed cleanup");
    if (both.reports.size() == 2) {
        ok &= Check(both.reports[0].operation.find(L"profiles") != std::wstring::npos, "profile cleanup name");
        ok &= Check(both.reports[1].operation.find(L"CLSID") != std::wstring::npos, "class cleanup name");
        ok &= Check(both.reports[0].original == both.failCode && both.reports[0].cleanup == E_FAIL,
                    "profile diagnostic includes both HRESULTs");
        ok &= Check(both.reports[1].original == both.failCode && both.reports[1].cleanup == both.cleanupClass,
                    "class diagnostic includes both HRESULTs");
    }
    ok &= Check(both.strategyProfile && both.strategyCom && !both.strategyCategory,
                "failed cleanups leave their own artifacts");
    ok &= PreservesNeighbors(both, "failed cleanup preserves orphan metadata");

    Fake profileOnly;
    profileOnly.failAt = StepAdd0804;
    profileOnly.failCode = FailureCode(StepAdd0804);
    profileOnly.cleanupProfile = E_FAIL;
    hr = RunRegistration(&profileOnly);
    ok &= Check(hr == profileOnly.failCode, "partial cleanup keeps the original HRESULT");
    ok &= Check(profileOnly.reports.size() == 1 && profileOnly.reports[0].original == hr &&
                    profileOnly.reports[0].cleanup == E_FAIL,
                "only the failed cleanup is diagnosed");
    ok &= Check(profileOnly.strategyProfile && !profileOnly.strategyCom && !profileOnly.strategyCategory,
                "successful class cleanup still removes the class");
    ok &= PreservesNeighbors(profileOnly, "partial cleanup preserves orphan metadata");
    return ok;
}

bool PreexistingClass() {
    Fake fake;
    fake.preexisting = true;
    fake.strategyCom = true;
    const HRESULT hr = RunRegistration(&fake);
    bool ok = Check(hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), "pre-existing CLSID is refused");
    ok &= SameCalls(fake, {"probe"}, "pre-existing CLSID performs no writes");
    ok &= Check(fake.strategyCom && fake.reports.empty(), "pre-existing CLSID is not rolled back");
    ok &= PreservesNeighbors(fake, "pre-existing CLSID preserves unrelated metadata");
    return ok;
}

bool SuccessPath() {
    Fake fake;
    const HRESULT hr = RunRegistration(&fake);
    bool ok = Check(hr == S_OK, "successful registration");
    ok &= SameCalls(fake, ExpectedRegistration(-1, false), "success runs every step once");
    ok &= LangsMatch(fake, ExpectedLangs(-1));
    ok &= Check(fake.strategyCom && fake.strategyProfile && fake.strategyCategory, "success records created artifacts");
    ok &= Check(fake.reports.empty(), "success does not clean up");
    ok &= PreservesNeighbors(fake, "success preserves unrelated metadata");
    return ok;
}

bool PathRejection() {
    bool ok = true;
    const wchar_t* path = kValidPath;
    const size_t length = wcslen(path);
    struct Case { const wchar_t* path; size_t capacity; size_t length; HRESULT hr; const char* name; };
    const Case cases[] = {
        {nullptr, 32, length, E_INVALIDARG, "null module path"},
        {path, 32, 0, E_INVALIDARG, "empty module path"},
        {L"", 32, 1, E_INVALIDARG, "blank module path"},
        {path, 0, length, E_INVALIDARG, "zero-capacity module path"},
        {path, 8, 8, HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), "truncated module path"},
    };
    for (const Case& item : cases) {
        Fake fake;
        const HRESULT hr = StrategyRunPreparedRegistration(item.path, item.capacity, item.length, &kOps, &fake);
        ok &= Check(hr == item.hr, item.name);
        ok &= Check(fake.calls.empty() && !fake.strategyCom && !fake.strategyProfile && !fake.strategyCategory,
                    "rejected path has no registration side effect");
        ok &= PreservesNeighbors(fake, "rejected path preserves metadata");
    }
    Fake missing;
    const HRESULT hr = StrategyRunPreparedRegistration(path, 260, length, nullptr, &missing);
    ok &= Check(hr == E_POINTER && missing.calls.empty(), "missing operations are refused after path validation");
    return ok;
}

bool UnregisterRetries() {
    bool ok = true;
    struct Presence { bool category; bool profile; bool com; };
    const Presence cases[] = {
        {true, true, true},
        {false, true, true},
        {true, false, true},
        {true, true, false},
        {false, false, true},
        {false, true, false},
        {true, false, false},
        {false, false, false},
    };
    for (const Presence& item : cases) {
        Fake fake;
        fake.strategyCategory = item.category;
        fake.strategyProfile = item.profile;
        fake.strategyCom = item.com;
        const HRESULT hr = StrategyRunUnregisterTransaction(&kOps, &fake);
        ok &= Check(hr == S_OK, "absent Strategy A metadata unregisters successfully");
        ok &= SameCalls(fake, {"unregisterCategory", "unregisterProfiles", "deleteClass"}, "unregister attempts every artifact");
        ok &= Check(!fake.strategyCategory && !fake.strategyProfile && !fake.strategyCom, "present Strategy A metadata is removed");
        ok &= PreservesNeighbors(fake, "unregister preserves unrelated and orphan metadata");
        Fake again;
        again.unrelatedCom = fake.unrelatedCom;
        again.orphanTsf = fake.orphanTsf;
        const HRESULT second = StrategyRunUnregisterTransaction(&kOps, &again);
        ok &= Check(second == S_OK, "a second unregister pass succeeds");
        ok &= SameCalls(again, {"unregisterCategory", "unregisterProfiles", "deleteClass"}, "second pass still attempts every artifact");
        ok &= PreservesNeighbors(again, "second pass preserves unrelated and orphan metadata");
    }

    Fake blocked;
    blocked.strategyCategory = true;
    blocked.strategyProfile = true;
    blocked.strategyCom = true;
    blocked.cleanupCategory = E_FAIL;
    const HRESULT hr = StrategyRunUnregisterTransaction(&kOps, &blocked);
    ok &= Check(hr == E_FAIL, "a real unregister failure is returned");
    ok &= Check(blocked.strategyCategory && !blocked.strategyProfile && !blocked.strategyCom,
                "later unregister steps still run after one failure");
    ok &= PreservesNeighbors(blocked, "failed unregister preserves orphan metadata");
    blocked.cleanupCategory = S_OK;
    blocked.calls.clear();
    const HRESULT retried = StrategyRunUnregisterTransaction(&kOps, &blocked);
    ok &= Check(retried == S_OK && !blocked.strategyCategory, "retry finishes the artifact that previously failed");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= IdentityMatches();
    ok &= FailureInjection();
    ok &= CleanupFailures();
    ok &= PreexistingClass();
    ok &= SuccessPath();
    ok &= PathRejection();
    ok &= UnregisterRetries();
    if (ok) {
        std::puts("PASS: registration transaction failure injection, rollback, HRESULT preservation, orphan preservation, path rejection, and retry-safe unregistration");
    }
    return ok ? 0 : 1;
}
