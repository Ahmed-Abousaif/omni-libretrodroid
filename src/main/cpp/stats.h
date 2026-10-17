#ifndef LIBRETRODROID_STATS_H
#define LIBRETRODROID_STATS_H

#include <cstddef>
#include <cstdint>

namespace libretrodroid {

struct FrameStatsSnapshot {
    double avgCpuRunMs = 0.0;
    double avgCpuRenderMs = 0.0;
    double avgGpuRenderMs = 0.0;
    double avgPresentIntervalMs = 0.0;
    double fps = 0.0;
    uint32_t droppedFrames = 0;
    uint32_t pipelineCompiles = 0;
    double pipelineCompileMs = 0.0;
    size_t memoryRssBytes = 0;
    int32_t audioXruns = 0;
    double audioFillPercent = 0.0;
    int32_t allocBytesPerFrame = 0;
};

class LRDStats {
public:
    static void setProfilingEnabled(bool enabled);
    static bool isProfilingEnabled();

    static void noteAudioWrite(size_t frames);
    static void noteUnderrun(int32_t missingFrames);
    static void noteAudioState(double fillPercent, int32_t xruns, double piAdjustment);
    static void noteStep(double retroRunMs, double presentIntervalMs);

    // Modern frame pacing, GPU execution, and pipeline compilation telemetry
    static void noteFrame(double retroRunMs, double renderMs, double gpuTimeMs, double presentIntervalMs, double targetIntervalMs);
    static void notePipelineCompile(double durationMs);

    static FrameStatsSnapshot getSnapshot();
};

} // namespace libretrodroid

#endif

