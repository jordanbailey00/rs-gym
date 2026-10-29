// Gym shops and death rules (run tools/build_gym.py first): prices follow stock, stock is shared
// and restocks; on death players keep 3 items (4 with Protect Item), the rest go to an owner-only
// gravestone, or to the floor for everyone in the wilderness, and they respawn in town.
#include "../rc-core/api.h"
#include "../rc-core/area_flags.h"
#include "../rc-core/config.h"
#include "../rc-core/items.h"
#include "../rc-core/pathfinding.h"
#include "../rc-core/players.h"
#include "../rc-core/prayer.h"
#include "../rc-core/shops.h"
#include "../rc-content/content.h"
#include "gym_test_config.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

enum {
    SKIP = 77, COINS = 995, BRONZE_AXE = 1351, STEEL_AXE = 1353, MITHRIL_AXE = 1355,
    ADAMANT_AXE = 1357, RUNE_AXE = 1359, LOGS = 1511, BONES = 526, BOB = 10619, SHOP_KEEPER = 2813,
};

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void tick(RcWorld *w, int n) {
    for (int i = 0; i < n; i++) rc_world_tick(w);
}

static int count_item(const RcPlayer *p, int item) {
    int n = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        if (p->inventory[i].item_id == item) n += p->inventory[i].quantity;
    return n;
}

static void clear_items(RcPlayer *p) {
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) {
        p->inventory[i] = (RcInvSlot){.item_id = -1, .generation = p->inventory[i].generation + 1};
    }
    for (int i = 0; i < RC_EQUIP_COUNT; i++) {
        p->equipment[i] = (RcInvSlot){.item_id = -1, .generation = p->equipment[i].generation + 1};
    }
}

static const RcNpc *find_npc(const RcWorld *w, int id) {
    int count = 0;
    const RcNpc *npcs = rc_get_npcs(w, &count);
    for (int i = 0; i < count; i++) {
        const RcNpcDef *def = rc_npc_base_def_for_npc(&npcs[i]);
        if (npcs[i].active && !npcs[i].is_dead && def && def->id == id) return &npcs[i];
    }
    return NULL;
}

static int stand_near(RcWorld *w, int x, int y) {
    for (int r = 1; r <= 4; r++)
        for (int dx = -r; dx <= r; dx++)
            for (int dy = -r; dy <= r; dy++)
                if ((dx == -r || dx == r || dy == -r || dy == r) && !rc_tile_blocked(&w->map, x + dx, y + dy, 0)
                        && rc_world_relocate_player(w, x + dx, y + dy, 0))
                    return 1;
    return 0;
}

static int open_shop_of(RcWorld *w, int npc_id) {
    const RcNpc *npc = find_npc(w, npc_id);
    if (!npc || !stand_near(w, npc->x, npc->y)) return -1;
    rc_player_interact_npc(w, npc->uid, 2);  // Trade
    for (int t = 0; t < 40 && w->player->shop_open < 0; t++) tick(w, 1);
    return w->player->shop_open;
}

static int shop_slot(const RcWorld *w, int shop, int item) {
    for (int i = 0; i < w->shops[shop].slot_count; i++)
        if (w->shops[shop].slots[i].item_id == item) return i;
    return -1;
}

static int ground_count(const RcWorld *w, int x, int y, int viewer, int item) {
    int n = 0;
    for (int i = 0; i < w->ground_item_count; i++) {
        const RcGroundItem *g = &w->ground_items[i];
        if (g->active && g->x == x && g->y == y && g->item_id == item && rc_ground_item_visible(g, viewer))
            n += g->quantity;
    }
    return n;
}

static void shops(RcWorld *w) {
    RcPlayer *p0 = &w->players[0], *p1 = &w->players[1];
    rc_world_select_player(w, 0);
    clear_items(p0);
    rc_player_inventory_add(w, COINS, 5000, 0);
    int bob = open_shop_of(w, BOB);
    CHECK(bob >= 0, "Bob's shop did not open");
    if (bob < 0) return;
    int steel = shop_slot(w, bob, STEEL_AXE), bronze = shop_slot(w, bob, BRONZE_AXE);
    int first = rc_shop_buy_price(w, bob, steel);
    CHECK(first == 200, "steel axe at base stock costs %d, not 200", first);
    rc_shop_buy(w, steel, 1);
    tick(w, 1);
    CHECK(count_item(p0, STEEL_AXE) == 1 && count_item(p0, COINS) == 4800, "bought steel axe: %d axes, %d coins",
          count_item(p0, STEEL_AXE), count_item(p0, COINS));
    CHECK(rc_shop_buy_price(w, bob, steel) > first, "price did not rise as stock fell (%d)",
          rc_shop_buy_price(w, bob, steel));
    int sell = rc_shop_sell_price(w, bob, STEEL_AXE);
    CHECK(sell > 0 && sell < first, "sell price %d", sell);
    rc_shop_sell(w, rc_inv_find(p0->inventory, STEEL_AXE), 1);
    tick(w, 1);
    CHECK(count_item(p0, STEEL_AXE) == 0 && count_item(p0, COINS) == 4800 + sell, "sold steel axe: %d coins",
          count_item(p0, COINS));

    rc_shop_buy(w, bronze, 2);
    tick(w, 1);
    CHECK(count_item(p0, BRONZE_AXE) == 2, "bought %d bronze axes", count_item(p0, BRONZE_AXE));
    int left = w->shops[bob].slots[bronze].stock;
    CHECK(left == 8, "bronze axe stock %d", left);
    // Shared stock: player 1 trades against the same shop.
    rc_world_select_player(w, 1);
    clear_items(p1);
    rc_player_inventory_add(w, COINS, 100, 0);
    CHECK(open_shop_of(w, BOB) == bob, "player 1 could not open Bob's shop");
    rc_shop_buy(w, bronze, 1);
    tick(w, 1);
    CHECK(count_item(p1, BRONZE_AXE) == 1 && w->shops[bob].slots[bronze].stock == 7, "shared stock: %d",
          w->shops[bob].slots[bronze].stock);
    // A tool shop does not buy logs; the general store buys anything tradeable.
    rc_player_inventory_add(w, LOGS, 1, 0);
    CHECK(rc_shop_sell_price(w, bob, LOGS) < 0, "Bob buys logs");
    int general = rc_shop_for_npc(w, SHOP_KEEPER);
    CHECK(general >= 0 && rc_shop_sell_price(w, general, LOGS) >= 0, "general store refuses logs");
    // Walking away closes the shop.
    rc_player_walk_to(w, p1->x + 2, p1->y);
    tick(w, 2);
    CHECK(p1->shop_open < 0, "shop stayed open after walking");
    // Bronze axes restock one per 100 ticks.
    tick(w, 320);
    CHECK(w->shops[bob].slots[bronze].stock == 10, "bronze axes restocked to %d", w->shops[bob].slots[bronze].stock);
    rc_world_select_player(w, 0);
}

static void die_at(RcWorld *w, int x, int y) {
    RcPlayer *p = w->player;
    CHECK(rc_world_relocate_player(w, x, y, 0), "relocate to %d,%d", x, y);
    p->current_hp = 0;
    tick(w, 1);
    CHECK(p->is_dead, "player did not die");
}

static void death(RcWorld *w) {
    RcPlayer *p = w->player;
    rc_world_select_player(w, 0);
    clear_items(p);
    int items[] = {BRONZE_AXE, STEEL_AXE, RUNE_AXE, MITHRIL_AXE, ADAMANT_AXE};
    for (int i = 0; i < 5; i++) rc_player_inventory_add(w, items[i], 1, 0);
    rc_player_inventory_add(w, COINS, 300, 0);
    int x = GYM_START_X, y = GYM_START_Y - 3;
    die_at(w, x, y);
    CHECK(count_item(p, RUNE_AXE) && count_item(p, ADAMANT_AXE) && count_item(p, MITHRIL_AXE),
          "kept items wrong");
    CHECK(!count_item(p, STEEL_AXE) && !count_item(p, BRONZE_AXE) && !count_item(p, COINS), "lost items kept");
    CHECK(ground_count(w, x, y, 0, STEEL_AXE) == 1 && ground_count(w, x, y, 0, COINS) == 300,
          "gravestone missing items");
    CHECK(ground_count(w, x, y, 1, STEEL_AXE) == 0, "gravestone visible to another player");
    tick(w, 5);
    CHECK(!p->is_dead && p->current_hp == p->max_hp, "no respawn: dead %d hp %d", p->is_dead, p->current_hp);
    CHECK(p->x == w->respawn_x && p->y == w->respawn_y, "respawned at %d,%d not %d,%d", p->x, p->y,
          w->respawn_x, w->respawn_y);

    // Protect Item keeps a fourth item.
    clear_items(p);
    for (int i = 0; i < 5; i++) rc_player_inventory_add(w, items[i], 1, 0);
    p->skills.base_level[SKILL_PRAYER] = p->skills.boosted_level[SKILL_PRAYER] = 99;
    p->current_prayer_points = 990;
    p->active_prayers |= 1u << RC_PRAYER_PROTECT_ITEM;
    die_at(w, x + 1, y);
    CHECK(count_item(p, STEEL_AXE) == 1 && count_item(p, BRONZE_AXE) == 0, "Protect Item kept %d steel %d bronze",
          count_item(p, STEEL_AXE), count_item(p, BRONZE_AXE));
    tick(w, 5);

    // In the wilderness the drop is public.
    clear_items(p);
    for (int i = 0; i < 5; i++) rc_player_inventory_add(w, items[i], 1, 0);
    int wx = -1, wy = -1;
    for (int ty = y; ty < y + 120 && wx < 0; ty++)
        if (rc_wilderness_level_at(x, ty, 0) > 0 && !rc_tile_blocked(&w->map, x, ty, 0)) { wx = x; wy = ty; }
    CHECK(wx >= 0, "no wilderness tile north of the start");
    if (wx >= 0) {
        die_at(w, wx, wy);
        CHECK(ground_count(w, wx, wy, 1, BRONZE_AXE) == 1, "wilderness drop not visible to other players");
        tick(w, 5);
    }
}

static void burying(RcWorld *w) {
    RcPlayer *p = w->player;
    rc_world_select_player(w, 0);
    clear_items(p);
    rc_player_inventory_add(w, BONES, 1, 0);
    int xp = p->skills.xp[SKILL_PRAYER];
    rc_player_interact_inventory_item(w, rc_inv_find(p->inventory, BONES), 0);
    tick(w, 2);
    CHECK(count_item(p, BONES) == 0 && p->skills.xp[SKILL_PRAYER] == xp + 4,
          "bury: %d bones left, prayer xp +%d", count_item(p, BONES), p->skills.xp[SKILL_PRAYER] - xp);
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
    CHECK(w->shop_count == 3 && w->death_rules, "gym rules: %d shops", w->shop_count);
    for (int i = 0; i < 2; i++) {
        rc_world_select_player(w, i);
        CHECK(rc_world_relocate_player(w, GYM_START_X + i, GYM_START_Y, 0), "relocate player %d", i);
    }
    shops(w);
    death(w);
    burying(w);
    rc_world_destroy(w);
    if (failures) return 1;
    printf("gym rules: PASS\n");
    return 0;
}
