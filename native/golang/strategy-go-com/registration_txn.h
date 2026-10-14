#pragma once

// Shared registration transaction for production and the native fake harness.
// The header does not call Win32 or TSF. Production supplies one callback per
// actual COM write and TSF call; tests supply fakes. Only artifacts this
// invocation positively created are cleaned up, in reverse creation order.
#include <windows.h>
#include <stddef.h>

// Isolated Strategy A identity. Production callbacks and the harness both
// read these constants so the language list cannot drift from the coordinator.
static const CLSID kStrategyAClsid = {0xa4c7d921, 0x6b35, 0x4e8a, {0x91, 0xf2, 0x7d, 0x0b, 0x6c, 0x3e, 0x5a, 0x14}};
static const GUID kStrategyAProfile = {0xe8b1c4d7, 0x2a63, 0x49f0, {0xb5, 0xd8, 0x1c, 0x7e, 0x3a, 0x9f, 0x62, 0x40}};
static const wchar_t kStrategyAName[] = L"strategy-go-com";

enum { kStrategyLanguageCount = 3 };
static const LANGID kStrategyLanguageIds[kStrategyLanguageCount] = {0x0409, 0x0804, 0x0411};

// GetModuleFileNameW reports truncation by returning the buffer capacity.
// Reject that, and any empty path, before a registration callback runs.
static inline HRESULT StrategyValidateModulePath(const wchar_t* path,
                                                  size_t capacity,
                                                  size_t length) {
    if (!path || capacity == 0 || length == 0) return E_INVALIDARG;
    if (length >= capacity) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    if (path[0] == L'\0') return E_INVALIDARG;
    return S_OK;
}

typedef struct StrategyRegistrationOps {
    HRESULT (*probeClass)(void* context);
    HRESULT (*writeClassName)(void* context);
    HRESULT (*writeModulePath)(void* context);
    HRESULT (*writeThreadingModel)(void* context);
    HRESULT (*registerProfile)(void* context);
    HRESULT (*addLanguageProfile)(void* context, LANGID lang);
    HRESULT (*enableLanguageProfile)(void* context, LANGID lang);
    HRESULT (*registerCategory)(void* context);
    HRESULT (*unregisterCategory)(void* context);
    HRESULT (*unregisterProfiles)(void* context);
    HRESULT (*deleteClass)(void* context);
    void (*reportCleanupFailure)(void* context, const wchar_t* operation,
                                 HRESULT originalHr, HRESULT cleanupHr);
} StrategyRegistrationOps;

static inline int StrategyRegistrationOpsReady(const StrategyRegistrationOps* ops) {
    return ops && ops->probeClass && ops->writeClassName && ops->writeModulePath &&
        ops->writeThreadingModel && ops->registerProfile && ops->addLanguageProfile &&
        ops->enableLanguageProfile && ops->registerCategory && ops->unregisterCategory &&
        ops->unregisterProfiles && ops->deleteClass && ops->reportCleanupFailure;
}

// createdClass is set once the class-name write is attempted: that helper
// creates the CLSID key before the default value write, so a failed value
// write can still leave a key behind. createdProfile is set only after
// ITfInputProcessorProfiles::Register succeeds. createdCategory is set only
// after ITfCategoryMgr::RegisterCategory succeeds. Language profiles are part
// of the profile-class artifact and are removed by Unregister(CLSID).
static inline HRESULT StrategyCleanupCreated(const StrategyRegistrationOps* ops,
                                             void* context,
                                             int createdClass,
                                             int createdProfile,
                                             int createdCategory,
                                             HRESULT originalHr) {
    if (createdCategory) {
        HRESULT cleanupHr = ops->unregisterCategory(context);
        if (FAILED(cleanupHr)) {
            ops->reportCleanupFailure(context,
                L"Unregister Strategy A keyboard category", originalHr, cleanupHr);
        }
    }
    if (createdProfile) {
        HRESULT cleanupHr = ops->unregisterProfiles(context);
        if (FAILED(cleanupHr)) {
            ops->reportCleanupFailure(context,
                L"Unregister Strategy A profiles", originalHr, cleanupHr);
        }
    }
    if (createdClass) {
        HRESULT cleanupHr = ops->deleteClass(context);
        if (FAILED(cleanupHr)) {
            ops->reportCleanupFailure(context,
                L"Delete Strategy A COM CLSID subtree", originalHr, cleanupHr);
        }
    }
    return originalHr;
}

static inline HRESULT StrategyRunRegisterTransaction(const StrategyRegistrationOps* ops,
                                                     void* context) {
    HRESULT hr;
    unsigned int i;
    int createdClass = 0;
    int createdProfile = 0;
    int createdCategory = 0;

    if (!StrategyRegistrationOpsReady(ops)) return E_POINTER;

    hr = ops->probeClass(context);
    if (FAILED(hr)) return hr;

    hr = ops->writeClassName(context);
    createdClass = 1;
    if (FAILED(hr)) {
        return StrategyCleanupCreated(ops, context, createdClass, createdProfile,
                                      createdCategory, hr);
    }

    hr = ops->writeModulePath(context);
    if (FAILED(hr)) {
        return StrategyCleanupCreated(ops, context, createdClass, createdProfile,
                                      createdCategory, hr);
    }

    hr = ops->writeThreadingModel(context);
    if (FAILED(hr)) {
        return StrategyCleanupCreated(ops, context, createdClass, createdProfile,
                                      createdCategory, hr);
    }

    hr = ops->registerProfile(context);
    if (FAILED(hr)) {
        return StrategyCleanupCreated(ops, context, createdClass, createdProfile,
                                      createdCategory, hr);
    }
    createdProfile = 1;

    for (i = 0; i < kStrategyLanguageCount; ++i) {
        LANGID lang = kStrategyLanguageIds[i];
        hr = ops->addLanguageProfile(context, lang);
        if (FAILED(hr)) {
            return StrategyCleanupCreated(ops, context, createdClass, createdProfile,
                                          createdCategory, hr);
        }
        hr = ops->enableLanguageProfile(context, lang);
        if (FAILED(hr)) {
            return StrategyCleanupCreated(ops, context, createdClass, createdProfile,
                                          createdCategory, hr);
        }
    }

    hr = ops->registerCategory(context);
    if (FAILED(hr)) {
        return StrategyCleanupCreated(ops, context, createdClass, createdProfile,
                                      createdCategory, hr);
    }
    createdCategory = 1;
    (void)createdCategory;
    return S_OK;
}

// Validates the module path, then runs the registration transaction.
// A rejected path performs no operation callback.
static inline HRESULT StrategyRunPreparedRegistration(const wchar_t* path,
                                                      size_t capacity,
                                                      size_t length,
                                                      const StrategyRegistrationOps* ops,
                                                      void* context) {
    HRESULT hr = StrategyValidateModulePath(path, capacity, length);
    if (FAILED(hr)) return hr;
    return StrategyRunRegisterTransaction(ops, context);
}

// Unregister always attempts category, profile, and COM class, including
// artifacts that are already absent. Each callback must succeed when its own
// Strategy A artifact is absent. The first real failure is returned after the
// remaining callbacks run, so a later retry can finish the rest.
static inline HRESULT StrategyRunUnregisterTransaction(const StrategyRegistrationOps* ops,
                                                       void* context) {
    HRESULT first = S_OK;
    HRESULT hr;
    if (!ops || !ops->unregisterCategory || !ops->unregisterProfiles || !ops->deleteClass) {
        return E_POINTER;
    }
    hr = ops->unregisterCategory(context);
    if (FAILED(hr)) first = hr;
    hr = ops->unregisterProfiles(context);
    if (FAILED(hr) && SUCCEEDED(first)) first = hr;
    hr = ops->deleteClass(context);
    if (FAILED(hr) && SUCCEEDED(first)) first = hr;
    return first;
}
