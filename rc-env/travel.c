// Walking distance from every tile of the gym to each landmark, built once per
// process from the first env's map (static collision). TRAVEL follows it a
// short hop at a time, so every route the engine searches is short and
// reachable, and observations get true walking distances.
#include "env_internal.h"

#include "../rc-core/pathfinding.h"

#include <pthread.h>
#include <stdlib.h>

#define UNREACHABLE UINT16_MAX

static uint16_t *fields[RC_GYM_MAX_LANDMARKS];
static int field_x, field_y, field_w, field_h;
static pthread_once_t fields_once = PTHREAD_ONCE_INIT;
static const RcGymLayout *build_layout;
static const RcWorldMap *build_map;

static const int dirs[8][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1}, {1, -1}, {-1, -1}, {-1, 1}};

static int cell(int x, int y) {
    x -= field_x;
    y -= field_y;
    return x >= 0 && y >= 0 && x < field_w && y < field_h ? y * field_w + x : -1;
}

static void build_field(const RcWorldMap *map, const RcGymLandmark *lm, uint16_t *dist,
                        int *queue) {
    for (int i = 0; i < field_w * field_h; i++) dist[i] = UNREACHABLE;
    int head = 0, tail = 0;
    // Seed at the landmark, or the walkable tiles next to it when it is blocked.
    for (int r = 0; r <= 3 && !tail; r++)
        for (int dx = -r; dx <= r; dx++)
            for (int dy = -r; dy <= r; dy++) {
                int c = cell(lm->x + dx, lm->y + dy);
                if (c < 0 || dist[c] != UNREACHABLE
                        || rc_tile_blocked(map, lm->x + dx, lm->y + dy, lm->plane))
                    continue;
                dist[c] = 0;
                queue[tail++] = c;
            }
    while (head < tail) {
        int c = queue[head++];
        int x = field_x + c % field_w, y = field_y + c / field_w;
        for (int d = 0; d < 8; d++) {
            int nx = x + dirs[d][0], ny = y + dirs[d][1], n = cell(nx, ny);
            // Walking downhill later goes from n back to (x, y), so test that step.
            if (n < 0 || dist[n] != UNREACHABLE
                    || !rc_can_move(map, nx, ny, -dirs[d][0], -dirs[d][1], lm->plane))
                continue;
            dist[n] = (uint16_t)(dist[c] + 1);
            queue[tail++] = n;
        }
    }
}

static void build_fields(void) {
    const RcGymLayout *layout = build_layout;
    field_x = layout->min_rx * 64;
    field_y = layout->min_ry * 64;
    field_w = (layout->max_rx - layout->min_rx + 1) * 64;
    field_h = (layout->max_ry - layout->min_ry + 1) * 64;
    int *queue = malloc((size_t)field_w * field_h * sizeof(*queue));
    for (int i = 0; queue && i < layout->landmark_count; i++) {
        fields[i] = malloc((size_t)field_w * field_h * sizeof(uint16_t));
        if (fields[i]) build_field(build_map, &layout->landmarks[i], fields[i], queue);
    }
    free(queue);
}

// The process's layout and loaded map (one mode per process, like its game data).
void rc_env_build_travel(const RcGymLayout *layout, const RcWorldMap *map) {
    build_layout = layout;
    build_map = map;
    pthread_once(&fields_once, build_fields);
}

int rc_env_travel_distance(int landmark, int x, int y, int plane) {
    if (landmark < 0 || landmark >= RC_GYM_MAX_LANDMARKS || !fields[landmark] || plane != 0)
        return -1;
    int c = cell(x, y);
    return c >= 0 && fields[landmark][c] != UNREACHABLE ? fields[landmark][c] : -1;
}

int rc_env_travel_waypoint(const RcWorldMap *map, int landmark, int x, int y, int plane,
                           int max_steps, int *wx, int *wy) {
    int d = rc_env_travel_distance(landmark, x, y, plane);
    if (d < 0) return 0;
    for (int step = 0; step < max_steps && d > 0; step++) {
        int best = -1, best_d = d;
        for (int k = 0; k < 8; k++) {
            int nd = rc_env_travel_distance(landmark, x + dirs[k][0], y + dirs[k][1], plane);
            if (nd >= 0 && nd < best_d && rc_can_move(map, x, y, dirs[k][0], dirs[k][1], plane)) {
                best = k;
                best_d = nd;
            }
        }
        if (best < 0) break;
        x += dirs[best][0];
        y += dirs[best][1];
        d = best_d;
    }
    *wx = x;
    *wy = y;
    return 1;
}
