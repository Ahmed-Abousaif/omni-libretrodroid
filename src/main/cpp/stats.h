#ifndef LIBRETRODROID_STATS_H
#define LIBRETRODROID_STATS_H

#include <cstddef>
#include <cstdint>

namespace libretrodroid {

// Debug-only once-per-second counters. Release builds compile these to no-ops.
class LRDStats {
public:
    static void noteAudioWrite(size_t frames);
    static void noteUnderrun(int32_t missingFrames);
    static void noteAudioState(double fillPercent, int32_t xruns, double piAdjustment);
    static void noteStep(double retroRunMs, double presentIntervalMs);
};

} // namespace libretrodroid

#endif
