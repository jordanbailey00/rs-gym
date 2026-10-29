// Headless checks on the built gym (run tools/build_mode.py island first): the world loads,
// the player starts on the island, the sea blocks movement, NPCs spawn and the bank opens.
#include "../rc-core/api.h"
#include "../rc-core/config.h"
#include "../rc-core/pathfinding.h"
#include "../rc-content/content.h"
#include "gym_test_config.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

enum { BANK_BOOTH = 10355, SKIP = 77 };
#define START_X GYM_START_X
#define START_Y GYM_START_Y

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void tick(RcWorld *w, int n) {
    for (int i = 0; i < n; i++) rc_world_tick(w);
}

static int bank_option(const RcWorld *w, int x, int y) {
    for (int op = 0; op < RC_OBJECT_ACTIONS; op++) {
        if (rc_world_object_option_supported(w, BANK_BOOTH, x, y, 0, 0, op)
                && strcmp(rc_world_object_option_label(w, BANK_BOOTH, x, y, 0, 0, op), "Bank") == 0)
            return op;
    }
    return -1;
}

int main(void) {
    if (!gym_mode_built("island")) {
        fprintf(stderr, "gym data not built; run tools/build_mode.py island\n");
        return SKIP;
    }
    RcWorldConfig cfg = gym_test_config_mode("island", 1);
    RcWorld *w = rc_world_create_config(&cfg);
    if (!w) {
        fprintf(stderr, "FAIL: world creation\n");
        return 1;
    }
    CHECK(rc_world_install_content(w, rc_content_register_all), "content registration");
    CHECK(rc_world_relocate_player(w, START_X, START_Y, 0), "relocate to the gym start");
    const RcPlayer *p = rc_get_player(w);
    CHECK(p->x == START_X && p->y == START_Y, "player at %d,%d", p->x, p->y);

    int npc_count = 0, active = 0;
    const RcNpc *npcs = rc_get_npcs(w, &npc_count);
    for (int i = 0; i < npc_count; i++) active += npcs[i].active;
    CHECK(active > 0, "no NPCs spawned");

    // The ocean ring (world x 64..127) is blocked. Clicking just past the west coast walks
    // to the shore and stops on land, like an OSRS click on water. Routes search a 128x128
    // window, so the click starts within 64 tiles of the coast.
    CHECK(rc_tile_blocked(&w->map, 100, START_Y, 0), "ocean ring tile is walkable");
    int coast_x = 128;
    while (coast_x < START_X && rc_tile_blocked(&w->map, coast_x, START_Y, 0)) coast_x++;
    CHECK(coast_x > 128 && coast_x < START_X, "no west coast found on row %d", START_Y);
    int from_x = coast_x + 40;
    while (from_x > coast_x && rc_tile_blocked(&w->map, from_x, START_Y, 0)) from_x--;
    CHECK(rc_world_relocate_player(w, from_x, START_Y, 0), "relocate near the coast");
    CHECK(rc_player_walk_to(w, coast_x - 3, START_Y), "walk command rejected");
    tick(w, 120);
    CHECK(p->x >= coast_x && p->x <= coast_x + 2 && !rc_tile_blocked(&w->map, p->x, p->y, 0),
          "west walk ended at %d,%d (coast %d)", p->x, p->y, coast_x);

    // Draynor bank booths, stamped at area (86, 98): source booth (3091, 3242) lands on (219, 232).
    CHECK(rc_world_relocate_player(w, START_X, START_Y, 0), "relocate back to start");
    int op = bank_option(w, 219, 232);
    CHECK(op >= 0, "bank booth at 219,232 offers no Bank option");
    if (op >= 0) {
        rc_player_interact_object_at(w, BANK_BOOTH, 219, 232, 0, op);
        for (int i = 0; i < 120 && !p->storage_kind; i++) tick(w, 1);
        CHECK(p->storage_kind != 0, "bank did not open; player ended at %d,%d", p->x, p->y);
    }

    rc_world_destroy(w);
    if (failures) return 1;
    printf("gym world: PASS (npcs=%d)\n", active);
    return 0;
}
