#ifndef RUNEC_VIEWER_INPUT_FEEDBACK_H
#define RUNEC_VIEWER_INPUT_FEEDBACK_H

#include <stdint.h>

#define RUNEC_CLICK_FEEDBACK_FRAME_COUNT 4
#define RUNEC_CLICK_FEEDBACK_FRAME_SECONDS 0.1f

typedef enum {
    RUNEC_CLICK_FEEDBACK_YELLOW = 0,
    RUNEC_CLICK_FEEDBACK_RED = 1,
} RuneCClickFeedbackStyle;

typedef struct {
    int click_active;
    RuneCClickFeedbackStyle click_style;
    int click_screen_x;
    int click_screen_y;
    float click_elapsed_seconds;
    int pending_destination_active;
    int pending_destination_x;
    int pending_destination_y;
    int pending_destination_plane;
    uint64_t pending_submitted_tick;
} RuneCInputFeedback;

void runec_input_feedback_reset(RuneCInputFeedback *feedback);
void runec_input_feedback_start_click(
    RuneCInputFeedback *feedback, RuneCClickFeedbackStyle style,
    int screen_x, int screen_y);
void runec_input_feedback_start_destination(
    RuneCInputFeedback *feedback, int x, int y, int plane,
    uint64_t submitted_tick);
void runec_input_feedback_advance(RuneCInputFeedback *feedback,
                                  float elapsed_seconds);
void runec_input_feedback_reconcile_tick(RuneCInputFeedback *feedback,
                                         uint64_t authoritative_tick);
int runec_input_feedback_click_frame(const RuneCInputFeedback *feedback);

#endif
