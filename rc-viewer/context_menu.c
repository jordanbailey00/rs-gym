#include "context_menu.h"

static int clamp_int(int value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

RuneCContextMenuLayout runec_context_menu_layout(
    int anchor_x, int anchor_y, int screen_width, int screen_height,
    int content_width, int action_count) {
    RuneCContextMenuLayout layout = {0};
    if (action_count < 0) action_count = 0;
    layout.width = content_width + RUNEC_CONTEXT_MENU_PADDING_X * 2;
    if (layout.width < 1) layout.width = 1;
    layout.height = RUNEC_CONTEXT_MENU_HEADER_HEIGHT
                  + action_count * RUNEC_CONTEXT_MENU_ROW_HEIGHT
                  + RUNEC_CONTEXT_MENU_BOTTOM_PADDING;

    int max_x = screen_width > layout.width
              ? screen_width - layout.width : 0;
    int max_y = screen_height > layout.height
              ? screen_height - layout.height : 0;
    layout.x = clamp_int(anchor_x - layout.width / 2, 0, max_x);
    layout.y = clamp_int(anchor_y, 0, max_y);
    return layout;
}

int runec_context_menu_contains(const RuneCContextMenuLayout *layout,
                                int x, int y) {
    return layout && x >= layout->x && y >= layout->y
        && x < layout->x + layout->width
        && y < layout->y + layout->height;
}

int runec_context_menu_contains_margin(const RuneCContextMenuLayout *layout,
                                       int x, int y, int margin) {
    if (!layout)
        return 0;
    if (margin < 0)
        margin = 0;
    return x >= layout->x - margin
        && x <= layout->x + layout->width + margin
        && y >= layout->y - margin
        && y <= layout->y + layout->height + margin;
}

int runec_context_menu_action_at(const RuneCContextMenuLayout *layout,
                                 int action_count, int x, int y) {
    if (!runec_context_menu_contains(layout, x, y)) return -1;
    int row_y = y - layout->y - RUNEC_CONTEXT_MENU_HEADER_HEIGHT;
    if (row_y < 0) return -1;
    int action = row_y / RUNEC_CONTEXT_MENU_ROW_HEIGHT;
    return action >= 0 && action < action_count ? action : -1;
}
