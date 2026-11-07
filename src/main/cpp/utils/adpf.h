#ifndef LIBRETRODROID_ADPF_H
#define LIBRETRODROID_ADPF_H

#include <cstdint>
#include <unistd.h>
#include <vector>
#include <mutex>

namespace libretrodroid {

class ADPFManager {
public:
    static ADPFManager& getInstance();

    void init();
    void startSession(pid_t threadId, int64_t targetDurationNanos);
    void registerThread(pid_t threadId);
    void unregisterThread(pid_t threadId);
    void updateTargetDuration(int64_t targetDurationNanos);
    void reportActualDuration(int64_t actualDurationNanos);
    void closeSession();

    // Thermal query
    // 0 = NONE, 1 = LIGHT, 2 = MODERATE, 3 = SEVERE, 4 = CRITICAL, 5 = EMERGENCY, 6 = SHUTDOWN
    int getCurrentThermalStatus();
    bool isThermalsSevereOrWorse();

private:
    ADPFManager() = default;
    ~ADPFManager();

    bool initialized = false;
    void* hintManager = nullptr;
    void* hintSession = nullptr;
    void* thermalManager = nullptr;
    int64_t currentTargetNanos = 0;
    int lastReportedThermalStatus = 0;
    std::vector<int32_t> registeredTids;
    std::recursive_mutex adpfMutex;
};

} // namespace libretrodroid

#endif // LIBRETRODROID_ADPF_H
