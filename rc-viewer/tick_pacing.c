#include "tick_pacing.h"

#include <math.h>

int rc_viewer_tick_pacing_advance(RcViewerTickPacing *pacing,
                                  double frame_seconds,
                                  double tick_seconds) {
    if (!pacing || frame_seconds < 0.0 || tick_seconds <= 0.0) return 0;
    pacing->accumulator_seconds += frame_seconds;
    uint64_t due = (uint64_t)floor(
        pacing->accumulator_seconds / tick_seconds);
    if (due == 0) return 0;
    pacing->accumulator_seconds -= (double)due * tick_seconds;
    if (due > RC_VIEWER_MAX_CATCH_UP_TICKS) {
        pacing->dropped_ticks += due - RC_VIEWER_MAX_CATCH_UP_TICKS;
        due = RC_VIEWER_MAX_CATCH_UP_TICKS;
    }
    return (int)due;
}

double rc_viewer_tick_pacing_fraction(const RcViewerTickPacing *pacing,
                                      double tick_seconds) {
    if (!pacing || tick_seconds <= 0.0) return 0.0;
    double fraction = pacing->accumulator_seconds / tick_seconds;
    if (fraction < 0.0) return 0.0;
    return fraction > 1.0 ? 1.0 : fraction;
}

void rc_viewer_tick_pacing_reset(RcViewerTickPacing *pacing) {
    if (!pacing) return;
    pacing->accumulator_seconds = 0.0;
}
