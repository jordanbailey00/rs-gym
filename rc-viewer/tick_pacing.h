#ifndef RC_VIEWER_TICK_PACING_H
#define RC_VIEWER_TICK_PACING_H

#include <stdint.h>

#define RC_VIEWER_MAX_CATCH_UP_TICKS 5

typedef struct {
    double accumulator_seconds;
    uint64_t dropped_ticks;
} RcViewerTickPacing;

int rc_viewer_tick_pacing_advance(RcViewerTickPacing *pacing,
                                  double frame_seconds,
                                  double tick_seconds);
double rc_viewer_tick_pacing_fraction(const RcViewerTickPacing *pacing,
                                      double tick_seconds);
void rc_viewer_tick_pacing_reset(RcViewerTickPacing *pacing);

#endif
