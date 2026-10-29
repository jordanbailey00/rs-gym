// Shared-world contention in the compact mode with eight players (run tools/build_mode.py first):
// when all of them reach for the last shop item, or the one item on the ground, in the same tick,
// exactly one gets it, nothing is duplicated, and the winner rotates from tick to tick instead of
// always being the lowest slot. Stock and item placement are test setup; buys and pickups are the
// players' commands.
#include "../rc-core/api.h"
#include "../rc-core/items.h"
#include "../rc-core/players.h"
#include "../rc-core/shops.h"
#include "../rc-content/content.h"
#include "gym_test_config.h"

enum { SKIP = 77, PLAYERS = 8, COINS = 995, HAMMER = 2347, SHOPKEEPER = 2813, LOGS = 1511 };

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int count(const RcPlayer *p, int item) {
    int n = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        n += p->inventory[i].item_id == item ? p->inventory[i].quantity : 0;
    return n;
}

int main(void) {
    if (!gym_mode_built("compact_osrs_v1")) {
        fprintf(stderr, "mode not built; run tools/build_mode.py compact_osrs_v1\n");
        return SKIP;
    }
    RcWorldConfig cfg = gym_test_config_mode("compact_osrs_v1", PLAYERS);
    RcWorld *w = rc_world_create_config(&cfg);
    if (!w || !rc_world_install_content(w, rc_content_register_all)) {
        fprintf(stderr, "FAIL: world creation\n");
        return 1;
    }
    for (int i = 0; i < PLAYERS; i++) {
        rc_world_select_player(w, i);
        CHECK(rc_world_relocate_player(w, GYM_START_X, GYM_START_Y, 0), "place %d", i);
        rc_content_compact_new_character(w);
        (void)rc_player_inventory_add(w, COINS, 100, 0);
    }
    int shop = rc_shop_for_npc(w, SHOPKEEPER), slot = -1;
    for (int i = 0; shop >= 0 && i < w->shops[shop].slot_count; i++)
        if (w->shops[shop].slots[i].item_id == HAMMER) slot = i;
    CHECK(slot >= 0, "no hammer in the general store");

    // The last hammer: eight buyers in one tick, eight rounds.
    int wins[PLAYERS] = {0};
    for (int round = 0; round < PLAYERS && slot >= 0; round++) {
        w->shops[shop].slots[slot].stock = 1;  // test setup
        int before[PLAYERS];
        for (int i = 0; i < PLAYERS; i++) {
            rc_world_select_player(w, i);
            before[i] = count(w->player, HAMMER);
            (void)rc_shop_open(w, shop);
            (void)rc_shop_buy(w, slot, 1);
        }
        rc_world_tick(w);
        int winners = 0;
        for (int i = 0; i < PLAYERS; i++)
            if (count(&w->players[i], HAMMER) > before[i]) winners++, wins[i]++;
        CHECK(winners == 1 && w->shops[shop].slots[slot].stock == 0,
              "round %d: %d buyers got the last hammer", round, winners);
    }
    int distinct = 0;
    for (int i = 0; i < PLAYERS; i++) distinct += wins[i] > 0;
    CHECK(distinct == PLAYERS, "the last hammer went to only %d different players in 8 rounds", distinct);

    // One item on the ground under everyone: one pickup succeeds, and the winner rotates.
    int picked[PLAYERS] = {0};
    for (int round = 0; round < PLAYERS; round++) {
        rc_world_select_player(w, 0);
        const RcPlayer *p = w->player;
        RcGroundGrant grant = {.item_id = LOGS, .quantity = 1};
        RcGroundPolicy policy = {.lifetime_ticks = 200, .untradeable_lifetime_ticks = 200};
        CHECK(rc_ground_grant(w, &grant, 1, p->x, p->y, p->plane, RC_GROUND_OWNER_NONE, policy)
              == RC_GROUND_GRANT_OK, "drop the logs");  // test setup
        int idx = -1;
        for (int i = 0; i < w->ground_item_count; i++)
            if (w->ground_items[i].active && w->ground_items[i].item_id == LOGS) idx = i;
        int before[PLAYERS];
        for (int i = 0; i < PLAYERS; i++) {
            rc_world_select_player(w, i);
            before[i] = count(w->player, LOGS);
            (void)rc_player_pickup_item(w, idx);
        }
        for (int t = 0; t < 3; t++) rc_world_tick(w);
        int winners = 0;
        for (int i = 0; i < PLAYERS; i++)
            if (count(&w->players[i], LOGS) > before[i]) winners++, picked[i]++;
        CHECK(winners == 1, "round %d: %d players picked up the one log", round, winners);
    }
    distinct = 0;
    for (int i = 0; i < PLAYERS; i++) distinct += picked[i] > 0;
    CHECK(distinct >= PLAYERS / 2, "ground item went to only %d different players", distinct);
    rc_world_destroy(w);
    if (failures) return 1;
    printf("compact shared world: PASS\n");
    return 0;
}
