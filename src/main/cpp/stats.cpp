#include "stats.h"

#ifndef NDEBUG

#include <android/log.h>
#include <chrono>
#include <mutex>

namespace libretrodroid {

namespace {

using Clock = std::chrono::steady_clock;

struct State {
    std::mutex mutex;
    Clock::time_point windowStart = Clock::now();
    Clock::time_point lastLog = Clock::now();
    size_t inputFrames = 0;
    int32_t underrunFrames = 0;
    int underrunEvents = 0;
    double fillPercent = 0;
    int32_t xruns = 0;
    double piAdjustment = 0;
    double retroRunMs = 0;
    double presentIntervalMs = 0;
    int steps = 0;
};

State& state() {
    static State s;
    return s;
}

void logIfDue(State& s) {
    auto now = Clock::now();
    auto elapsedMs = std::chrono::duration<double, std::milli>(now - s.lastLog).count();
    if (elapsedMs < 1000.0) {
        return;
    }
    double windowSec = std::chrono::duration<double>(now - s.windowStart).count();
    if (windowSec <= 0.0) {
        windowSec = 1.0;
    }
    double inputFps = static_cast<double>(s.inputFrames) / windowSec;
    double avgRun = s.steps > 0 ? s.retroRunMs / s.steps : 0.0;
    double avgPresent = s.steps > 0 ? s.presentIntervalMs / s.steps : 0.0;
    __android_log_print(
        ANDROID_LOG_INFO,
        "LRDStats",
        "fifo=%.1f%% underruns=%d (%d frames) xruns=%d pi=%.4f in=%.0f fps retroRun=%.2f ms presentInterval=%.2f ms",
        s.fillPercent,
        s.underrunEvents,
        s.underrunFrames,
        s.xruns,
        s.piAdjustment,
        inputFps,
        avgRun,
        avgPresent
    );
    s.lastLog = now;
    s.windowStart = now;
    s.inputFrames = 0;
    s.underrunFrames = 0;
    s.underrunEvents = 0;
    s.steps = 0;
    s.retroRunMs = 0;
    s.presentIntervalMs = 0;
}

} // namespace

void LRDStats::noteAudioWrite(size_t frames) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.inputFrames += frames;
    logIfDue(s);
}

void LRDStats::noteUnderrun(int32_t missingFrames) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.underrunEvents += 1;
    s.underrunFrames += missingFrames;
}

void LRDStats::noteAudioState(double fillPercent, int32_t xruns, double piAdjustment) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.fillPercent = fillPercent;
    s.xruns = xruns;
    s.piAdjustment = piAdjustment;
}

void LRDStats::noteStep(double retroRunMs, double presentIntervalMs) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.retroRunMs += retroRunMs;
    s.presentIntervalMs += presentIntervalMs;
    s.steps += 1;
    logIfDue(s);
}

} // namespace libretrodroid

#else

namespace libretrodroid {
void LRDStats::noteAudioWrite(size_t) {}
void LRDStats::noteUnderrun(int32_t) {}
void LRDStats::noteAudioState(double, int32_t, double) {}
void LRDStats::noteStep(double, double) {}
}

#endif
