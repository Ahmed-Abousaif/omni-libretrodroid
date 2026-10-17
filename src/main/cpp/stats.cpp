#include "stats.h"

#include <android/log.h>
#include <chrono>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <unistd.h>
#include <malloc.h>
#include <sys/system_properties.h>

namespace libretrodroid {

namespace {

using Clock = std::chrono::steady_clock;

#ifdef NDEBUG
std::atomic<bool> g_profilingEnabled{false};
#else
std::atomic<bool> g_profilingEnabled{true};
#endif

bool checkSystemProperty() {
    char prop[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.omnidroid.profile", prop) > 0) {
        return (prop[0] == '1' || prop[0] == 't' || prop[0] == 'T' || prop[0] == 'y' || prop[0] == 'Y');
    }
    return false;
}

size_t queryProcessRssBytes() {
    FILE* file = fopen("/proc/self/statm", "r");
    if (!file) return 0;
    long pages = 0;
    if (fscanf(file, "%*s%ld", &pages) == 1) {
        fclose(file);
        long pageSize = sysconf(_SC_PAGESIZE);
        return static_cast<size_t>(pages * (pageSize > 0 ? pageSize : 4096));
    }
    fclose(file);
    return 0;
}

size_t queryHeapAllocatedBytes() {
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 33
    struct mallinfo2 mi = mallinfo2();
    return static_cast<size_t>(mi.uordblks);
#else
    struct mallinfo mi = mallinfo();
    return static_cast<size_t>(mi.uordblks);
#endif
}

struct State {
    std::mutex mutex;
    Clock::time_point windowStart = Clock::now();
    Clock::time_point lastLog = Clock::now();

    // Audio
    size_t inputFrames = 0;
    int32_t underrunFrames = 0;
    int underrunEvents = 0;
    double fillPercent = 0.0;
    int32_t xruns = 0;
    double piAdjustment = 0.0;

    // Frame timings
    double retroRunMsSum = 0.0;
    double cpuRenderMsSum = 0.0;
    double gpuRenderMsSum = 0.0;
    int gpuSamples = 0;
    double presentIntervalMsSum = 0.0;
    int steps = 0;
    uint32_t droppedFrames = 0;

    // Pipeline compiles
    uint32_t pipelineCompiles = 0;
    double pipelineCompileMsSum = 0.0;

    // Memory
    size_t lastHeapAllocatedBytes = 0;
    int64_t heapAllocDeltaSum = 0;

    // Latest snapshot for UI / JNI
    FrameStatsSnapshot latestSnapshot {};
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

    double avgRun = s.steps > 0 ? s.retroRunMsSum / s.steps : 0.0;
    double avgRender = s.steps > 0 ? s.cpuRenderMsSum / s.steps : 0.0;
    double avgGpu = s.gpuSamples > 0 ? s.gpuRenderMsSum / s.gpuSamples : -1.0;
    double avgPresent = s.steps > 0 ? s.presentIntervalMsSum / s.steps : 0.0;
    double fps = avgPresent > 0.0 ? (1000.0 / avgPresent) : 0.0;
    size_t rssBytes = queryProcessRssBytes();
    double rssMb = static_cast<double>(rssBytes) / (1024.0 * 1024.0);
    double allocKbPerFrame = s.steps > 0 ? (static_cast<double>(s.heapAllocDeltaSum) / s.steps / 1024.0) : 0.0;

    // Update snapshot
    s.latestSnapshot.avgCpuRunMs = avgRun;
    s.latestSnapshot.avgCpuRenderMs = avgRender;
    s.latestSnapshot.avgGpuRenderMs = avgGpu;
    s.latestSnapshot.avgPresentIntervalMs = avgPresent;
    s.latestSnapshot.fps = fps;
    s.latestSnapshot.droppedFrames = s.droppedFrames;
    s.latestSnapshot.pipelineCompiles = s.pipelineCompiles;
    s.latestSnapshot.pipelineCompileMs = s.pipelineCompileMsSum;
    s.latestSnapshot.memoryRssBytes = rssBytes;
    s.latestSnapshot.audioXruns = s.xruns;
    s.latestSnapshot.audioFillPercent = s.fillPercent;
    s.latestSnapshot.allocBytesPerFrame = s.steps > 0 ? static_cast<int32_t>(s.heapAllocDeltaSum / s.steps) : 0;

    if (avgGpu >= 0.0) {
        __android_log_print(
            ANDROID_LOG_INFO,
            "OmnidroidStats",
            "FPS: %5.1f | CPU-Run: %5.2f ms | CPU-Rend: %5.2f ms | GPU: %5.2f ms | Present: %5.2f ms | Drop: %u | PipeComp: %u (%.1f ms) | RSS: %.1f MB | Alloc: %.1f KB/f",
            fps,
            avgRun,
            avgRender,
            avgGpu,
            avgPresent,
            s.droppedFrames,
            s.pipelineCompiles,
            s.pipelineCompileMsSum,
            rssMb,
            allocKbPerFrame
        );
    } else {
        __android_log_print(
            ANDROID_LOG_INFO,
            "OmnidroidStats",
            "FPS: %5.1f | CPU-Run: %5.2f ms | CPU-Rend: %5.2f ms | Present: %5.2f ms | Drop: %u | PipeComp: %u (%.1f ms) | RSS: %.1f MB | Alloc: %.1f KB/f",
            fps,
            avgRun,
            avgRender,
            avgPresent,
            s.droppedFrames,
            s.pipelineCompiles,
            s.pipelineCompileMsSum,
            rssMb,
            allocKbPerFrame
        );
    }

    s.lastLog = now;
    s.windowStart = now;
    s.inputFrames = 0;
    s.underrunFrames = 0;
    s.underrunEvents = 0;
    s.steps = 0;
    s.retroRunMsSum = 0.0;
    s.cpuRenderMsSum = 0.0;
    s.gpuRenderMsSum = 0.0;
    s.gpuSamples = 0;
    s.presentIntervalMsSum = 0.0;
    s.droppedFrames = 0;
    s.pipelineCompiles = 0;
    s.pipelineCompileMsSum = 0.0;
    s.heapAllocDeltaSum = 0;
}

} // namespace

bool LRDStats::isProfilingEnabled() {
    static bool checkedProp = false;
    if (!checkedProp) {
        checkedProp = true;
        if (checkSystemProperty()) {
            g_profilingEnabled.store(true, std::memory_order_relaxed);
        }
    }
    return g_profilingEnabled.load(std::memory_order_relaxed);
}

void LRDStats::setProfilingEnabled(bool enabled) {
    g_profilingEnabled.store(enabled, std::memory_order_relaxed);
}

void LRDStats::noteAudioWrite(size_t frames) {
    if (!isProfilingEnabled()) return;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.inputFrames += frames;
    logIfDue(s);
}

void LRDStats::noteUnderrun(int32_t missingFrames) {
    if (!isProfilingEnabled()) return;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.underrunEvents += 1;
    s.underrunFrames += missingFrames;
}

void LRDStats::noteAudioState(double fillPercent, int32_t xruns, double piAdjustment) {
    if (!isProfilingEnabled()) return;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.fillPercent = fillPercent;
    s.xruns = xruns;
    s.piAdjustment = piAdjustment;
}

void LRDStats::noteStep(double retroRunMs, double presentIntervalMs) {
    noteFrame(retroRunMs, 0.0, -1.0, presentIntervalMs, 16.67);
}

void LRDStats::noteFrame(
    double retroRunMs,
    double renderMs,
    double gpuTimeMs,
    double presentIntervalMs,
    double targetIntervalMs
) {
    if (!isProfilingEnabled()) return;

    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    s.retroRunMsSum += retroRunMs;
    s.cpuRenderMsSum += renderMs;
    if (gpuTimeMs >= 0.0) {
        s.gpuRenderMsSum += gpuTimeMs;
        s.gpuSamples += 1;
    }
    s.presentIntervalMsSum += presentIntervalMs;
    s.steps += 1;

    // Detect late/dropped frames (>1.4x target interval)
    if (targetIntervalMs > 0.0 && presentIntervalMs > (targetIntervalMs * 1.4)) {
        s.droppedFrames += 1;
    }

    // Per-frame allocation tracking
    size_t currentHeap = queryHeapAllocatedBytes();
    if (s.lastHeapAllocatedBytes > 0 && currentHeap > s.lastHeapAllocatedBytes) {
        s.heapAllocDeltaSum += static_cast<int64_t>(currentHeap - s.lastHeapAllocatedBytes);
    }
    s.lastHeapAllocatedBytes = currentHeap;

    logIfDue(s);
}

void LRDStats::notePipelineCompile(double durationMs) {
    if (!isProfilingEnabled()) return;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.pipelineCompiles += 1;
    s.pipelineCompileMsSum += durationMs;
}

FrameStatsSnapshot LRDStats::getSnapshot() {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.latestSnapshot;
}

} // namespace libretrodroid

