// Headless smoke test of the compact_osrs_v1 mode (run tools/build_mode.py first): the rules load
// and patch the definitions, every player gets the starter kit, XP follows the mode multiplier and
// cap, the closed recipe table and fixed-price shops work, a chicken drops its mode table to its
// killer, a tree yields logs with an inventory axe, and four players step 300 ticks.
#include "../rc-core/api.h"
#include "../rc-core/items.h"
#include "../rc-core/mode.h"
#include "../rc-core/npc.h"
#include "../rc-core/players.h"
#include "../rc-core/prayer.h"
#include "../rc-core/shops.h"
#include "../rc-core/skills.h"
#include "../rc-core/spells.h"
#include "../rc-core/storage.h"
#include "../rc-content/content.h"
#include "gym_test_config.h"

#include <stdlib.h>
#include <string.h>

enum { SKIP = 77, PLAYERS = 4, CHICKEN = 1173, BONES = 526, RAW_CHICKEN = 2138, FEATHER = 314,
       BRONZE_DAGGER = 1205, SHRIMPS = 315, COINS = 995, BRONZE_AXE = 1351, LOGS = 1511,
       HAMMER = 2347, COPPER_ORE = 436 };

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void tick(RcWorld *w, int n) {
    for (int i = 0; i < n; i++) rc_world_tick(w);
}

static int count_item(const RcInvSlot *slots, int n, int item) {
    int total = 0;
    for (int i = 0; i < n; i++) if (slots[i].item_id == item) total += slots[i].quantity;
    return total;
}

static const RcNpc *nearest_npc(const RcWorld *w, int def_id, int x, int y) {
    int count = 0, best_d = 1 << 30;
    const RcNpc *npcs = rc_get_npcs(w, &count), *best = NULL;
    for (int i = 0; i < count; i++) {
        const RcNpcDef *d = npcs[i].active && !npcs[i].is_dead ? rc_npc_def_for_npc(w, &npcs[i]) : NULL;
        if (!d || d->id != def_id) continue;
        int dist = abs(npcs[i].x - x) + abs(npcs[i].y - y);
        if (dist < best_d) best_d = dist, best = &npcs[i];
    }
    return best;
}

static int option_named(const RcWorld *w, int obj, int x, int y, const char *label) {
    for (int op = 0; op < RC_OBJECT_ACTIONS; op++)
        if (rc_world_object_option_supported(w, obj, x, y, 0, 0, op)
                && !strcmp(rc_world_object_option_label(w, obj, x, y, 0, 0, op), label))
            return op;
    return -1;
}

static void check_rules(const RcModeRules *m) {
    CHECK(m && !strcmp(m->name, "compact_osrs_v1"), "mode rules not active");
    if (!m) return;
    CHECK(m->level_cap == 40 && m->xp_multiplier == 8, "cap %d xp x%d", m->level_cap, m->xp_multiplier);
    const RcNpcDef *chicken = NULL;
    int count = 0;
    const RcNpcDef *defs = rc_npc_defs_all(&count);
    for (int i = 0; i < count; i++) if (defs[i].id == CHICKEN) chicken = &defs[i];
    CHECK(chicken && chicken->hitpoints == 3 && chicken->max_hit == 1, "chicken profile not patched");
    CHECK(g_rc_recipe_count == m->recipe_count && rc_recipe_get(0)
          && rc_recipe_get(0)->output_item == 2349, "recipe table is not the mode's (%d rows)",
          g_rc_recipe_count);
    int strikes = 0, fire_strike_level = 0;
    for (int i = 0; i < RC_MAX_SPELL_DEFS; i++) {
        const RcSpellDef *s = rc_spell_def_get(i);
        if (!s || !s->loaded) continue;
        if (!strcmp(s->name, "Wind Strike") || !strcmp(s->name, "Water Strike"))
            strikes += s->level == 1 && s->max_hit == 4;
        if (!strcmp(s->name, "Fire Strike")) fire_strike_level = s->level;
    }
    CHECK(strikes == 2, "Wind and Water Strike are not level 1, max hit 4");
    CHECK(fire_strike_level > m->level_cap, "Fire Strike is castable (level %d)", fire_strike_level);
    const RcPrayerDef *melee = rc_prayer_def_get(RC_PRAYER_PROTECT_FROM_MELEE);
    const RcPrayerDef *piety = rc_prayer_def_get(RC_PRAYER_PIETY);
    CHECK(melee && melee->level == 15, "Protect from Melee is not level 15");
    CHECK(piety && piety->level > m->level_cap, "Piety is usable");
    const RcItemDef *oak = rc_item_def_get(843);
    CHECK(oak && oak->req_count == 1 && oak->req_level[0] == 15, "oak shortbow is not Ranged 15");
}

static void check_kit_and_xp(RcWorld *w) {
    const RcPlayer *p = rc_get_player(w);
    CHECK(p->equipment[EQUIP_WEAPON].item_id == BRONZE_DAGGER, "no bronze dagger equipped");
    CHECK(count_item(p->inventory, RC_INVENTORY_SIZE, SHRIMPS) == 4, "not four shrimps");
    CHECK(count_item(p->bank, RC_BANK_SIZE, COINS) == 100, "bank coins %d",
          count_item(p->bank, RC_BANK_SIZE, COINS));
    CHECK(count_item(p->bank, RC_BANK_SIZE, BRONZE_AXE) == 1, "no bronze axe in the bank");
    CHECK(p->skills.base_level[SKILL_HITPOINTS] == 10 && p->skills.base_level[SKILL_ATTACK] == 1,
          "starting levels");
    RcSkills s = p->skills;
    rc_add_xp(&s, SKILL_ATTACK, 100);
    CHECK(s.xp[SKILL_ATTACK] == 800, "100 attack XP became %d", s.xp[SKILL_ATTACK]);
    rc_add_xp(&s, SKILL_FIREMAKING, 100);
    CHECK(s.xp[SKILL_FIREMAKING] == 0, "a disabled skill gained XP");
    rc_add_xp(&s, SKILL_MINING, 5000000);
    CHECK(s.base_level[SKILL_MINING] == 40, "mining level %d past the cap", s.base_level[SKILL_MINING]);
}

static void check_shop(RcWorld *w) {
    RcPlayer *p = w->player;
    int shop = rc_shop_for_npc(w, 2813);
    CHECK(shop >= 0, "no general store");
    if (shop < 0) return;
    (void)rc_player_inventory_add(w, COINS, 10, 0);
    (void)rc_player_inventory_add(w, COPPER_ORE, 1, 0);
    CHECK(rc_shop_open(w, shop), "shop did not open");
    int hammer = -1;
    for (int i = 0; i < w->shops[shop].slot_count; i++)
        if (w->shops[shop].slots[i].item_id == HAMMER) hammer = i;
    CHECK(hammer >= 0 && rc_shop_buy_price(w, shop, hammer) == 1, "hammer price");
    int stock = hammer >= 0 ? w->shops[shop].slots[hammer].stock : 0;
    CHECK(hammer >= 0 && rc_shop_buy_expected(w, hammer, 1, HAMMER) == 1, "could not buy a hammer");
    CHECK(count_item(p->inventory, RC_INVENTORY_SIZE, COINS) == 9, "hammer did not cost one coin");
    CHECK(hammer >= 0 && w->shops[shop].slots[hammer].stock == stock - 1, "stock did not drop");
    int ore = rc_inv_find(p->inventory, COPPER_ORE);
    int slots = w->shops[shop].slot_count;
    CHECK(rc_shop_sell_price(w, shop, COPPER_ORE) == 2, "copper ore sells for %d",
          rc_shop_sell_price(w, shop, COPPER_ORE));
    CHECK(ore >= 0 && rc_shop_sell(w, ore, 1) == 1, "could not sell copper ore");
    tick(w, 1);  // sales queue as commands outside a tick
    CHECK(count_item(p->inventory, RC_INVENTORY_SIZE, COINS) == 11, "ore sale paid %d coins",
          count_item(p->inventory, RC_INVENTORY_SIZE, COINS) - 9);
    CHECK(w->shops[shop].slot_count == slots, "a sale added shop stock");
    CHECK(rc_shop_sell_price(w, shop, 229) < 0, "the shop buys vials");
    rc_shop_close(p);
}

static void check_chicken(RcWorld *w) {
    RcPlayer *p = w->player;
    const RcNpc *c = nearest_npc(w, CHICKEN, p->x, p->y);
    CHECK(c != NULL, "no chicken spawned");
    if (!c) return;
    int uid = c->uid, cx = c->x, cy = c->y;
    CHECK(rc_world_relocate_player(w, cx + 1, cy, 0), "relocate beside the chicken");
    CHECK(rc_player_attack_npc(w, uid), "attack rejected");
    int t = 0;
    while (t < 400 && !w->last_loot.npc_def_id) tick(w, 1), t++;
    CHECK(w->last_loot.npc_def_id == CHICKEN && w->last_loot.result == 0,
          "no chicken loot after %d ticks (result %d)", t, w->last_loot.result);
    int bones = 0, meat = 0, feathers = 0, mine = 1;
    for (int i = 0; i < w->ground_item_count; i++) {
        const RcGroundItem *g = &w->ground_items[i];
        if (!g->active) continue;
        bones += g->item_id == BONES;
        meat += g->item_id == RAW_CHICKEN;
        if (g->item_id == FEATHER) feathers += g->quantity;
        if (g->item_id == BONES) mine &= g->owner_uid == p->index;
    }
    tick(w, 3);
    for (int i = 0; i < w->ground_item_count; i++) {
        const RcGroundItem *g = &w->ground_items[i];
        if (!g->active) continue;
        bones += g->item_id == BONES;
        meat += g->item_id == RAW_CHICKEN;
        if (g->item_id == FEATHER) feathers += g->quantity;
    }
    CHECK(bones && meat && feathers >= 3, "chicken drop: bones %d meat %d feathers %d", bones, meat,
          feathers);
    CHECK(mine, "the killer does not own the drop");
}

static void check_woodcutting(RcWorld *w, const RcGymLayout *layout) {
    RcPlayer *p = w->player;
    int best = -1, tx = 0, ty = 0;
    for (int dx = -40; dx <= 40 && best < 0; dx++)
        for (int dy = 0; dy <= 50 && best < 0; dy++) {
            int x = layout->start_x + dx, y = layout->start_y + dy;
            if (option_named(w, 1276, x, y, "Chop down") >= 0) best = 1276, tx = x, ty = y;
            else if (option_named(w, 1278, x, y, "Chop down") >= 0) best = 1278, tx = x, ty = y;
        }
    CHECK(best >= 0, "no tree near the start");
    if (best < 0) return;
    (void)rc_player_inventory_add(w, BRONZE_AXE, 1, 0);
    int xp = p->skills.xp[SKILL_WOODCUTTING];
    CHECK(rc_world_relocate_player(w, tx, ty - 1, 0) || rc_world_relocate_player(w, tx - 1, ty, 0),
          "relocate beside the tree");
    rc_player_interact_object_at(w, best, tx, ty, 0, option_named(w, best, tx, ty, "Chop down"));
    int t = 0;
    while (t < 300 && rc_inv_find(p->inventory, LOGS) < 0) tick(w, 1), t++;
    CHECK(rc_inv_find(p->inventory, LOGS) >= 0, "no logs after %d ticks", t);
    CHECK(p->skills.xp[SKILL_WOODCUTTING] - xp == 200, "a log gave %d XP, not 25 x 8",
          p->skills.xp[SKILL_WOODCUTTING] - xp);
}

int main(void) {
    if (!gym_mode_built("compact_osrs_v1")) {
        fprintf(stderr, "mode not built; run tools/build_mode.py compact_osrs_v1\n");
        return SKIP;
    }
    RcWorldConfig cfg = gym_test_config_mode("compact_osrs_v1", PLAYERS);
    RcWorld *w = rc_world_create_config(&cfg);
    if (!w) {
        fprintf(stderr, "FAIL: world creation\n");
        return 1;
    }
    CHECK(rc_world_install_content(w, rc_content_register_all), "content registration");
    check_rules(rc_mode_active());
    for (int i = 0; i < PLAYERS; i++) {
        rc_world_select_player(w, i);
        CHECK(rc_world_relocate_player(w, GYM_START_X + i, GYM_START_Y, 0), "place player %d", i);
        rc_content_compact_new_character(w);
    }
    rc_world_select_player(w, 0);
    check_kit_and_xp(w);
    check_shop(w);
    rc_world_select_player(w, 1);
    check_chicken(w);
    rc_world_select_player(w, 2);
    check_woodcutting(w, &gym_layout);
    RcTick before = w->tick;
    tick(w, 300);
    CHECK(w->tick == before + 300, "ticks did not advance");
    for (int i = 0; i < PLAYERS; i++)
        CHECK(!w->players[i].is_dead, "player %d died while idle", i);
    rc_world_destroy(w);
    if (failures) return 1;
    printf("compact mode: PASS\n");
    return 0;
}
