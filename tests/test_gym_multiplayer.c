// Several players in one gym world (run tools/build_gym.py first): independent movement,
// inventories and banks; NPCs fight the player who attacked them; loot belongs to the killer.
#include "../rc-core/api.h"
#include "../rc-core/config.h"
#include "../rc-core/items.h"
#include "../rc-core/players.h"
#include "../rc-content/content.h"
#include "gym_test_config.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

enum { SKIP = 77, BANK_BOOTH = 10355, BRONZE_AXE = 1351, GOBLIN_A = 3029, GOBLIN_B = 3031 };

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void tick(RcWorld *w, int n) {
    for (int i = 0; i < n; i++) rc_world_tick(w);
}

static int dist(int ax, int ay, int bx, int by) {
    int dx = ax > bx ? ax - bx : bx - ax, dy = ay > by ? ay - by : by - ay;
    return dx > dy ? dx : dy;
}

static int bank_option(const RcWorld *w, int x, int y) {
    for (int op = 0; op < RC_OBJECT_ACTIONS; op++)
        if (rc_world_object_option_supported(w, BANK_BOOTH, x, y, 0, 0, op)
                && strcmp(rc_world_object_option_label(w, BANK_BOOTH, x, y, 0, 0, op), "Bank") == 0)
            return op;
    return -1;
}

static const RcNpc *find_npc(const RcWorld *w, int id_a, int id_b) {
    int count = 0;
    const RcNpc *npcs = rc_get_npcs(w, &count);
    for (int i = 0; i < count; i++) {
        const RcNpcDef *def = rc_npc_base_def_for_npc(&npcs[i]);
        if (npcs[i].active && !npcs[i].is_dead && def && (def->id == id_a || def->id == id_b)) return &npcs[i];
    }
    return NULL;
}

int main(void) {
    struct stat st;
    if (stat("data/regions/mapsquare.catalog", &st) != 0) {
        fprintf(stderr, "gym data not built; run tools/build_gym.py\n");
        return SKIP;
    }
    RcWorldConfig cfg = gym_test_config(2);
    RcWorld *w = rc_world_create_config(&cfg);
    if (!w) {
        fprintf(stderr, "FAIL: world creation\n");
        return 1;
    }
    CHECK(rc_world_install_content(w, rc_content_register_all), "content registration");
    CHECK(w->player_count == 2, "player count %d", w->player_count);

    for (int i = 0; i < 2; i++) {
        CHECK(rc_world_select_player(w, i) >= 0, "select player %d", i);
        CHECK(rc_world_relocate_player(w, GYM_START_X + 2 * i, GYM_START_Y, 0), "relocate player %d", i);
    }
    RcPlayer *p0 = &w->players[0], *p1 = &w->players[1];
    CHECK(p0->x == GYM_START_X && p1->x == GYM_START_X + 2, "start positions %d %d", p0->x, p1->x);
    CHECK(!rc_world_relocate_player(w, 3213, 3428, 0), "relocation outside the pinned gym was accepted");

    // Independent movement along the town road.
    rc_world_select_player(w, 0);
    rc_player_walk_to(w, GYM_START_X - 12, GYM_START_Y);
    rc_world_select_player(w, 1);
    rc_player_walk_to(w, GYM_START_X + 12, GYM_START_Y);
    tick(w, 20);
    CHECK(p0->x == GYM_START_X - 12 && p1->x == GYM_START_X + 12, "walked to %d,%d and %d,%d",
          p0->x, p0->y, p1->x, p1->y);

    // Independent inventories.
    rc_world_select_player(w, 1);
    rc_player_inventory_add(w, BRONZE_AXE, 1, 0);
    CHECK(p1->inventory[0].item_id == BRONZE_AXE, "player 1 did not get the axe");
    CHECK(p0->inventory[0].item_id < 0, "player 0 inventory changed");

    // Both open the Draynor bank at neighbouring booths in the same tick.
    int booths[2][2] = {{219, 232}, {219, 233}};
    for (int i = 0; i < 2; i++) {
        rc_world_select_player(w, i);
        int op = bank_option(w, booths[i][0], booths[i][1]);
        CHECK(op >= 0, "booth %d has no Bank option", i);
        rc_player_interact_object_at(w, BANK_BOOTH, booths[i][0], booths[i][1], 0, op);
    }
    for (int t = 0; t < 120 && !(p0->storage_kind && p1->storage_kind); t++) tick(w, 1);
    CHECK(p0->storage_kind && p1->storage_kind, "banks open: %d %d", p0->storage_kind, p1->storage_kind);
    rc_world_select_player(w, 1);
    CHECK(rc_bank_deposit_slot(w, 0, 1), "player 1 deposit");
    tick(w, 1);
    CHECK(p1->bank[0].item_id == BRONZE_AXE && p0->bank[0].item_id < 0, "banks are not separate");

    // Player 1 (made accurate) kills a goblin; the goblin fights player 1 and the drop is theirs.
    for (int s = 0; s < SKILL_COUNT; s++) {
        p1->skills.base_level[s] = p1->skills.boosted_level[s] = 99;
    }
    p1->skills.base_level[SKILL_STRENGTH] = p1->skills.boosted_level[SKILL_STRENGTH] = 1;  // several hits, so it fights back
    p1->max_hp = p1->current_hp = 990;
    rc_world_select_player(w, 1);
    rc_player_close_storage(w);
    rc_world_select_player(w, 0);
    rc_player_close_storage(w);
    tick(w, 1);
    const RcNpc *chicken = find_npc(w, GOBLIN_A, GOBLIN_B);
    CHECK(chicken != NULL, "no goblin spawned");
    if (chicken) {
        int uid = chicken->uid, cx = chicken->x, cy = chicken->y;
        rc_world_select_player(w, 1);
        CHECK(rc_world_relocate_player(w, cx + 1, cy, 0), "relocate next to the goblin");
        rc_player_attack_npc(w, uid);
        int target_seen = -1;
        for (int t = 0; t < 60; t++) {
            tick(w, 1);
            const RcNpc *c = rc_npc_resolve(w, uid);
            if (c && c->target_uid >= 0) target_seen = c->target_uid;
            if (c) { cx = c->x; cy = c->y; }  // loot drops where it dies
            if (!c || c->is_dead) break;
        }
        CHECK(target_seen == 1, "goblin targeted slot %d", target_seen);
        tick(w, 6);
        int mine = 0, leaked = 0;
        for (int i = 0; i < w->ground_item_count; i++) {
            const RcGroundItem *g = &w->ground_items[i];
            if (!g->active || dist(g->x, g->y, cx, cy) > 3) continue;
            if (rc_ground_item_visible(g, 1)) mine++;
            if (rc_ground_item_visible(g, 0) && !rc_ground_item_visible(g, 1)) leaked++;
        }
        CHECK(mine > 0 && leaked == 0, "goblin loot visible to player 1: %d (leaked %d)", mine, leaked);
    }

    rc_world_destroy(w);
    if (failures) return 1;
    printf("gym multiplayer: PASS\n");
    return 0;
}
