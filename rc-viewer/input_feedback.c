#include "input_feedback.h"

#include <string.h>

void runec_input_feedback_reset(RuneCInputFeedback *feedback) {
    if (feedback)
        memset(feedback, 0, sizeof(*feedback));
}

void runec_input_feedback_start_click(
    RuneCInputFeedback *feedback, RuneCClickFeedbackStyle style,
    int screen_x, int screen_y) {
    if (!feedback)
        return;
    feedback->click_active = 1;
    feedback->click_style = style;
    feedback->click_screen_x = screen_x;
    feedback->click_screen_y = screen_y;
    feedback->click_elapsed_seconds = 0.0f;
}

void runec_input_feedback_start_destination(
    RuneCInputFeedback *feedback, int x, int y, int plane,
    uint64_t submitted_tick) {
    if (!feedback)
        return;
    feedback->pending_destination_active = 1;
    feedback->pending_destination_x = x;
    feedback->pending_destination_y = y;
    feedback->pending_destination_plane = plane;
    feedback->pending_submitted_tick = submitted_tick;
}

void runec_input_feedback_advance(RuneCInputFeedback *feedback,
                                  float elapsed_seconds) {
    if (!feedback || !feedback->click_active || elapsed_seconds <= 0.0f)
        return;
    feedback->click_elapsed_seconds += elapsed_seconds;
    if (feedback->click_elapsed_seconds >=
            RUNEC_CLICK_FEEDBACK_FRAME_COUNT
                * RUNEC_CLICK_FEEDBACK_FRAME_SECONDS) {
        feedback->click_active = 0;
    }
}

void runec_input_feedback_reconcile_tick(RuneCInputFeedback *feedback,
                                         uint64_t authoritative_tick) {
    if (!feedback || !feedback->pending_destination_active)
        return;
    if (authoritative_tick > feedback->pending_submitted_tick)
        feedback->pending_destination_active = 0;
}

int runec_input_feedback_click_frame(const RuneCInputFeedback *feedback) {
    if (!feedback || !feedback->click_active)
        return -1;
    int frame = (int)(feedback->click_elapsed_seconds
                    / RUNEC_CLICK_FEEDBACK_FRAME_SECONDS);
    return frame >= 0 && frame < RUNEC_CLICK_FEEDBACK_FRAME_COUNT ? frame : -1;
}
