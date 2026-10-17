#include "adpf.h"
#include <dlfcn.h>
#include "../log.h"

namespace libretrodroid {

// Opaque types from NDK
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef struct AThermalManager AThermalManager;

// NDK function pointer types
typedef APerformanceHintManager* (*pfn_APerformanceHint_getManager)();
typedef APerformanceHintSession* (*pfn_APerformanceHint_createSession)(
    APerformanceHintManager*, const int32_t*, size_t, int64_t);
typedef int (*pfn_APerformanceHint_updateTargetWorkDuration)(
    APerformanceHintSession*, int64_t);
typedef int (*pfn_APerformanceHint_reportActualWorkDuration)(
    APerformanceHintSession*, int64_t);
typedef void (*pfn_APerformanceHint_closeSession)(APerformanceHintSession*);

typedef AThermalManager* (*pfn_AThermal_acquireManager)();
typedef void (*pfn_AThermal_releaseManager)(AThermalManager*);
typedef int (*pfn_AThermal_getCurrentThermalStatus)(AThermalManager*);

static pfn_APerformanceHint_getManager s_getManager = nullptr;
static pfn_APerformanceHint_createSession s_createSession = nullptr;
static pfn_APerformanceHint_updateTargetWorkDuration s_updateTarget = nullptr;
static pfn_APerformanceHint_reportActualWorkDuration s_reportActual = nullptr;
static pfn_APerformanceHint_closeSession s_closeSession = nullptr;

static pfn_AThermal_acquireManager s_acquireThermal = nullptr;
static pfn_AThermal_releaseManager s_releaseThermal = nullptr;
static pfn_AThermal_getCurrentThermalStatus s_getThermalStatus = nullptr;

ADPFManager& ADPFManager::getInstance() {
    static ADPFManager instance;
    return instance;
}

ADPFManager::~ADPFManager() {
    closeSession();
    if (thermalManager != nullptr && s_releaseThermal != nullptr) {
        s_releaseThermal(static_cast<AThermalManager*>(thermalManager));
        thermalManager = nullptr;
    }
}

void ADPFManager::init() {
    if (initialized) {
        return;
    }
    initialized = true;

    // Dynamically resolve ADPF Performance Hint symbols (API 31+)
    s_getManager = reinterpret_cast<pfn_APerformanceHint_getManager>(
        dlsym(RTLD_DEFAULT, "APerformanceHint_getManager")
    );
    s_createSession = reinterpret_cast<pfn_APerformanceHint_createSession>(
        dlsym(RTLD_DEFAULT, "APerformanceHint_createSession")
    );
    s_updateTarget = reinterpret_cast<pfn_APerformanceHint_updateTargetWorkDuration>(
        dlsym(RTLD_DEFAULT, "APerformanceHint_updateTargetWorkDuration")
    );
    s_reportActual = reinterpret_cast<pfn_APerformanceHint_reportActualWorkDuration>(
        dlsym(RTLD_DEFAULT, "APerformanceHint_reportActualWorkDuration")
    );
    s_closeSession = reinterpret_cast<pfn_APerformanceHint_closeSession>(
        dlsym(RTLD_DEFAULT, "APerformanceHint_closeSession")
    );

    if (s_getManager != nullptr && s_createSession != nullptr) {
        hintManager = s_getManager();
        LOGI("ADPF Performance Hint Manager initialized: %p", hintManager);
    } else {
        LOGI("ADPF Performance Hint APIs not available on this platform");
    }

    // Dynamically resolve Thermal Manager symbols (API 30+)
    s_acquireThermal = reinterpret_cast<pfn_AThermal_acquireManager>(
        dlsym(RTLD_DEFAULT, "AThermal_acquireManager")
    );
    s_releaseThermal = reinterpret_cast<pfn_AThermal_releaseManager>(
        dlsym(RTLD_DEFAULT, "AThermal_releaseManager")
    );
    s_getThermalStatus = reinterpret_cast<pfn_AThermal_getCurrentThermalStatus>(
        dlsym(RTLD_DEFAULT, "AThermal_getCurrentThermalStatus")
    );

    if (s_acquireThermal != nullptr && s_getThermalStatus != nullptr) {
        thermalManager = s_acquireThermal();
        LOGI("Android Thermal Manager initialized: %p", thermalManager);
    } else {
        LOGI("Android Thermal Manager APIs not available on this platform");
    }
}

void ADPFManager::startSession(pid_t threadId, int64_t targetDurationNanos) {
    init();
    closeSession();

    if (hintManager == nullptr || s_createSession == nullptr || targetDurationNanos <= 0) {
        return;
    }

    int32_t tid = static_cast<int32_t>(threadId);
    hintSession = s_createSession(
        static_cast<APerformanceHintManager*>(hintManager),
        &tid,
        1,
        targetDurationNanos
    );
    currentTargetNanos = targetDurationNanos;
    LOGI("ADPF Performance Hint Session started for thread %d (target: %.2f ms, session: %p)",
         tid, static_cast<double>(targetDurationNanos) / 1e6, hintSession);
}

void ADPFManager::updateTargetDuration(int64_t targetDurationNanos) {
    if (hintSession == nullptr || s_updateTarget == nullptr || targetDurationNanos <= 0) {
        return;
    }
    if (currentTargetNanos != targetDurationNanos) {
        s_updateTarget(static_cast<APerformanceHintSession*>(hintSession), targetDurationNanos);
        currentTargetNanos = targetDurationNanos;
    }
}

void ADPFManager::reportActualDuration(int64_t actualDurationNanos) {
    if (hintSession == nullptr || s_reportActual == nullptr || actualDurationNanos <= 0) {
        return;
    }
    s_reportActual(static_cast<APerformanceHintSession*>(hintSession), actualDurationNanos);
}

void ADPFManager::closeSession() {
    if (hintSession != nullptr) {
        if (s_closeSession != nullptr) {
            s_closeSession(static_cast<APerformanceHintSession*>(hintSession));
        }
        hintSession = nullptr;
    }
    currentTargetNanos = 0;
}

int ADPFManager::getCurrentThermalStatus() {
    if (thermalManager == nullptr || s_getThermalStatus == nullptr) {
        return 0; // ATHERMAL_STATUS_NONE
    }
    int status = s_getThermalStatus(static_cast<AThermalManager*>(thermalManager));
    if (status >= 3 /* ATHERMAL_STATUS_SEVERE */ && status != lastReportedThermalStatus) {
        LOGW("High Thermal Status Detected: %d (0=NONE, 1=LIGHT, 2=MODERATE, 3=SEVERE, 4=CRITICAL)", status);
        lastReportedThermalStatus = status;
    }
    return status;
}

bool ADPFManager::isThermalsSevereOrWorse() {
    return getCurrentThermalStatus() >= 3;
}

} // namespace libretrodroid
