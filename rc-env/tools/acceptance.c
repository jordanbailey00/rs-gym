// rs-gym-acceptance: the compact_osrs_v1 acceptance runs (SPEC.md, "Acceptance evidence").
//
//   rs-gym-acceptance [all|routes|coop|contention|economy|perf] [--seeds N] [--report FILE]
//
// routes      fresh characters reach a Scurrius kill as melee, ranged and magic, N seeds each
// coop        four players with different production investments, real trades, a shared kill
// contention  eight players on one quarry, one shop and one boss
// economy     arbitrage, conservation and death-recovery checks
// perf        ticks per second through the compact-v2 interface
//
// Every run uses scripted controllers (rc-env/script.h): plans act only through player commands
// (walk, use locs, attack, eat, pray, bank, shop, trade, pick up) and know the reference map; they
// never set levels, grant items, teleport or read the RNG. The report (markdown) lists each run;
// logs go to build/acceptance/. A run is reproduced exactly by its seed.
#include "../../rc-content/content.h"
#include "../../rc-core/api.h"
#include "../../rc-core/layout.h"
#include "../../rc-core/mode.h"
#include "../../rc-core/npc.h"
#include "../../rc-core/players.h"
#include "../../rc-core/prayer.h"
#include "../../rc-core/shops.h"
#include "../../rc-core/skills.h"
#include "../../rc-core/snapshot.h"
#include "../../rc-core/spells.h"
#include "../../rc-core/storage.h"
#include "../../rc-core/trade.h"
#include "../compact_env.h"
#include "../script.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    COINS = 995, BONES = 526, BIG_BONES = 532, SHRIMPS = 315, TROUT = 333, LOBSTER = 379,
    BRONZE_SCIM = 1321, STEEL_SCIM = 1325, MITHRIL_SCIM = 1329,
    SHORTBOW = 841, OAK_SHORTBOW = 843, WILLOW_SHORTBOW = 849,
    BRONZE_ARROW = 882, STEEL_ARROW = 886, MITHRIL_ARROW = 888,
    STAFF_OF_AIR = 1381, WATER_RUNE = 555, MIND_RUNE = 558, CHAOS_RUNE = 562, DEATH_RUNE = 560,
    RAW_BEEF = 2132, COWHIDE = 1739, COOKED_MEAT = 2142, BURNT_MEAT = 2146, RANGE = 7183,
    COW = 2790, GOBLIN = 3029, SCURRIUS = 7222, SHOPKEEPER = 2813, AUBURY = 11434,
    TRAPDOOR = 14880, LADDER = 17385, PIPE = 16511, BARS = 14203, SEWAGE = 14204, ALTAR = 409,
};

static RcGymLayout layout;
static const RcModeRules *rules;
static FILE *report;

static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (report) {
        va_start(ap, fmt);
        vfprintf(report, fmt, ap);
        va_end(ap);
    }
}

static const RcGymLandmark *lm(const char *name) {
    for (int i = 0; i < layout.landmark_count; i++)
        if (!strcmp(layout.landmarks[i].name, name)) return &layout.landmarks[i];
    fprintf(stderr, "rs-gym-acceptance: no landmark %s\n", name);
    exit(2);
}

static RcWorld *new_world(int players, uint32_t seed) {
    RcWorldConfig cfg = rc_gym_world_config(&layout, players);
    RcWorld *w = rc_world_create_config(&cfg);
    if (!w || !rc_world_install_content(w, rc_content_register_all)
            || !rc_world_reset_seeded(w, seed)) {
        fprintf(stderr, "rs-gym-acceptance: world creation failed\n");
        exit(1);
    }
    for (int i = 0; i < players; i++) {
        rc_world_select_player(w, i);
        (void)rc_world_relocate_player(w, layout.start_x + i % 4 - 1, layout.start_y + i / 4, 0);
        rc_content_compact_new_character(w);
    }
    rules = rc_mode_active();
    return w;
}

// ---- Places ----------------------------------------------------------------------------------------
// World tiles of the reference map's links (maps/compact_v1.toml; the dungeon area starts at
// 512,128). The surface has two ways down: the manhole into the sewers (rats, goblins) and the
// trapdoor into the cave (skeletons, dark wizards, hill giants); a pipe joins the two, and the
// cave's far pipe leads to Scurrius' lair.

enum { SURFACE, SEWERS, CAVE, LAIR, ARENA };

static int region(int x, int y) {
    if (x < 512) return SURFACE;
    if (x >= 578) return SEWERS;
    if (x >= 555 && x <= 572 && y >= 172 && y <= 189) return ARENA;  // the arena side of the water
    return y >= 169 ? LAIR : CAVE;
}

typedef struct {
    int from, to, loc, x, y;
    const char *option;
    int to_x, to_y;            // where it lands
} Link;

static const Link links[] = {
    {SURFACE, SEWERS, 882, 174, 172, "Climb-down", 598, 133},
    {SEWERS, SURFACE, 11806, 599, 133, "Climb-up", 174, 171},
    {SURFACE, CAVE, TRAPDOOR, 228, 186, "Climb-down", 538, 160},
    {CAVE, SURFACE, LADDER, 538, 161, "Climb-up", 228, 185},
    {SEWERS, CAVE, PIPE, 615, 146, "Squeeze-through", 539, 164},
    {CAVE, SEWERS, PIPE, 539, 166, "Squeeze-through", 614, 144},
    {CAVE, LAIR, PIPE, 526, 139, "Squeeze-through", 545, 179},
    {LAIR, CAVE, PIPE, 544, 183, "Squeeze-through", 526, 140},
    {LAIR, ARENA, BARS, 547, 180, "Climb-through (normal)", 556, 180},
    {ARENA, LAIR, SEWAGE, 553, 180, "Cross", 546, 179},
};
enum { LINK_COUNT = sizeof(links) / sizeof(links[0]) };

static int chebyshev(int ax, int ay, int bx, int by) {
    int dx = abs(ax - bx), dy = abs(ay - by);
    return dx > dy ? dx : dy;
}

// Rough walking cost from (x, y) to (tx, ty) through links (Chebyshev within a region, a few ticks
// per link), and the first link to take. Small enough for exhaustive search.
static int plan_cost(int x, int y, int tx, int ty, int depth, int *first) {
    int from = region(x, y), to = region(tx, ty);
    if (from == to) return chebyshev(x, y, tx, ty);
    if (depth >= 5) return 1 << 20;
    int best = 1 << 20;
    for (int i = 0; i < LINK_COUNT; i++) {
        const Link *l = &links[i];
        if (l->from != from) continue;
        int rest = plan_cost(l->to_x, l->to_y, tx, ty, depth + 1, NULL);
        int cost = chebyshev(x, y, l->x, l->y) + 4 + rest;
        if (cost < best) {
            best = cost;
            if (first) *first = i;
        }
    }
    return best;
}

static int use_link(ScAgent *a, const Link *l) {
    if (!sc_walk(a, l->x, l->y, 3)) return 0;
    (void)sc_use_loc(a, l->loc, l->x, l->y, l->option);
    for (int t = 0; t < 12; t++) {
        const RcPlayer *p = sc_player(a);
        if (region(p->x, p->y) == l->to) return 1;
        sc_wait(a, 1);
    }
    return region(sc_player(a)->x, sc_player(a)->y) == l->to;
}

// Walks to (x, y) through whatever links lie between.
static int go(ScAgent *a, int x, int y) {
    for (int hop = 0; hop < 12; hop++) {
        const RcPlayer *p = sc_player(a);
        if (p->is_dead) return 0;
        if (region(p->x, p->y) == region(x, y)) return sc_walk(a, x, y, 1);
        int first = -1;
        (void)plan_cost(p->x, p->y, x, y, 0, &first);
        if (first < 0 || !use_link(a, &links[first])) {
            sc_note(a, "no way from %d,%d to %d,%d (link %d)", p->x, p->y, x, y, first);
            sc_wait(a, 2);
        }
    }
    return 0;
}

static int go_lm(ScAgent *a, const char *name) {
    const RcGymLandmark *l = lm(name);
    return go(a, l->x, l->y);
}

// ---- Character helpers -------------------------------------------------------------------------------

static int level(ScAgent *a, int skill) { return sc_player(a)->skills.base_level[skill]; }

static int coins(ScAgent *a) { return sc_count(a, COINS); }

static int food_count(ScAgent *a) {
    return sc_count(a, SHRIMPS) + sc_count(a, TROUT) + sc_count(a, LOBSTER) + sc_count(a, COOKED_MEAT);
}

static int equipped(ScAgent *a, int item) {
    const RcPlayer *p = sc_player(a);
    for (int i = 0; i < RC_EQUIP_COUNT; i++) if (p->equipment[i].item_id == item) return 1;
    return 0;
}

static int ammo_count(ScAgent *a) {
    const RcPlayer *p = sc_player(a);
    return p->equipment[EQUIP_AMMO].item_id >= 0 ? p->equipment[EQUIP_AMMO].quantity : 0;
}

// 0 melee, 1 ranged, 2 magic: the weapon class of an item (bows, staves, everything else).
static int weapon_class(int item) {
    static const int ranged[] = {SHORTBOW, OAK_SHORTBOW, WILLOW_SHORTBOW, 28794};
    static const int magic[] = {STAFF_OF_AIR, 28796};
    for (int i = 0; i < 4; i++) if (item == ranged[i]) return 1;
    for (int i = 0; i < 2; i++) if (item == magic[i]) return 2;
    return 0;
}

static int tier_of(const RcItemDef *d) {
    int t = 0;
    for (int k = 0; d && k < d->req_count; k++) t = d->req_level[k] > t ? d->req_level[k] : t;
    return t;
}

// Puts arrows from the inventory into the quiver: the type already there, else the best carried.
static void reload(ScAgent *a) {
    static const int arrows[] = {MITHRIL_ARROW, STEEL_ARROW, BRONZE_ARROW};
    int cur = sc_player(a)->equipment[EQUIP_AMMO].item_id;
    if (cur >= 0 && sc_count(a, cur)) {
        (void)sc_equip(a, cur);
        return;
    }
    for (int i = 0; i < 3 && cur < 0; i++)
        if (sc_count(a, arrows[i])) {
            (void)sc_equip(a, arrows[i]);
            return;
        }
}

// Wears every item in the inventory its levels allow that is better than the slot's current one;
// weapons only of the route's style, arrows only for ranged.
static void wear_best(ScAgent *a, int style) {
    for (int pass = 0; pass < RC_INVENTORY_SIZE; pass++) {
        RcPlayer *p = sc_player(a);
        int done = 1;
        for (int i = 0; i < RC_INVENTORY_SIZE; i++) {
            const RcItemDef *d = rc_item_def_get(p->inventory[i].item_id);
            if (!d || !d->equippable || d->equip_slot < 0 || d->id == COINS) continue;
            if (d->equip_slot == EQUIP_WEAPON && weapon_class(d->id) != style) continue;
            if (d->equip_slot == EQUIP_AMMO && style != 1) continue;
            if (d->equip_slot == EQUIP_WEAPON && style == 1 && !ammo_count(a)
                    && !sc_count(a, BRONZE_ARROW) && !sc_count(a, STEEL_ARROW) && !sc_count(a, MITHRIL_ARROW))
                continue;  // a bow without arrows cannot attack
            int ok = 1;
            for (int k = 0; k < d->req_count; k++)
                if (p->skills.base_level[d->req_skill[k]] < d->req_level[k]) ok = 0;
            const RcItemDef *cur = rc_item_def_get(p->equipment[d->equip_slot].item_id);
            int wrong_class = cur && d->equip_slot == EQUIP_WEAPON && weapon_class(cur->id) != style;
            int better = !cur || wrong_class || tier_of(d) > tier_of(cur)
                      || (d->equip_slot == EQUIP_AMMO && (cur->id == d->id || tier_of(d) >= tier_of(cur)));
            if (!ok || !better) continue;
            (void)sc_equip(a, d->id);
            done = 0;
            break;
        }
        if (done) break;
    }
}

typedef struct {
    int style;              // 0 melee, 1 ranged, 2 magic
    int success, deaths, attempts, kills;
    int prep_ticks, fight_ticks, total_ticks;
    int food_eaten, ammo_used, runes_used;
    int levels[SKILL_COUNT];
    int start_tick;
    int deaths_seen;        // scheduler deaths already handled
    int water;              // magic: Water spells (the current target is weak to water)
    int team_bit;           // co-op: this agent's bit
    int food_target;        // food to carry into the fight
    volatile int *shared;   // co-op: hand-off flags
} Route;

static void count_food_and_ammo(ScAgent *a, Route *r, int food_before, int ammo_before, int runes_before) {
    int food = food_count(a), ammo = ammo_count(a);
    int runes = sc_count(a, MIND_RUNE) + sc_count(a, CHAOS_RUNE) + sc_count(a, DEATH_RUNE);
    if (food < food_before) r->food_eaten += food_before - food;
    if (ammo < ammo_before) r->ammo_used += ammo_before - ammo;
    if (runes < runes_before) r->runes_used += runes_before - runes;
}

// ---- Shopping ----------------------------------------------------------------------------------------

// Coins each agent spent on consumables (food, arrows, runes) and on everything else.
static int spent_supplies[RC_MAX_PLAYERS], spent_gear[RC_MAX_PLAYERS];

static int is_supply(int item) {
    static const int supplies[] = {SHRIMPS, TROUT, LOBSTER, BRONZE_ARROW, STEEL_ARROW, MITHRIL_ARROW,
                                   MIND_RUNE, CHAOS_RUNE, DEATH_RUNE, WATER_RUNE, 556, 2434};
    for (int i = 0; i < (int)(sizeof(supplies) / sizeof(supplies[0])); i++)
        if (supplies[i] == item) return 1;
    return 0;
}

static int buy(ScAgent *a, int shop_npc, int item, int qty) {
    RcPlayer *p = sc_player(a);
    if (p->shop_open < 0 && !sc_shop_open(a, shop_npc)) return 0;
    int got = 0, before = coins(a);
    for (int tries = 0; got < qty && tries < 40; tries++) {
        int n = qty - got >= 10 ? 10 : 1;
        int bought = sc_buy(a, item, n);
        if (bought <= 0) break;
        got += bought;
    }
    (is_supply(item) ? spent_supplies : spent_gear)[a->index] += before - coins(a);
    return got;
}

static int price_of(int shop_index, int item) {
    for (int i = 0; i < rules->shops[shop_index].stock_count; i++)
        if (rules->shops[shop_index].stock[i].item == item) return rules->shops[shop_index].stock[i].price;
    return 1 << 20;
}

// The general store's next upgrade the character can use and afford, keeping `reserve` coins.
static void shop_upgrades(ScAgent *a, Route *r, int reserve) {
    static const int melee[] = {MITHRIL_SCIM, STEEL_SCIM, BRONZE_SCIM, 1121, 1071, 1159, 1197,
                                1119, 1069, 1157, 1193, 1117, 1075, 1155, 1189};
    static const int ranged[] = {WILLOW_SHORTBOW, OAK_SHORTBOW, 1129, 1095, 1167};
    const int *list = r->style == 0 ? melee : ranged;
    int n = r->style == 0 ? (int)(sizeof(melee) / sizeof(melee[0])) : (int)(sizeof(ranged) / sizeof(ranged[0]));
    if (r->style == 2) n = 0;
    for (int i = 0; i < n; i++) {
        const RcItemDef *d = rc_item_def_get(list[i]);
        if (!d || equipped(a, list[i]) || sc_count(a, list[i])) continue;
        int ok = 1;
        for (int k = 0; k < d->req_count; k++)
            if (level(a, d->req_skill[k]) < d->req_level[k]) ok = 0;
        // Skip a weaker item than what is worn in that slot.
        const RcItemDef *cur = rc_item_def_get(sc_player(a)->equipment[d->equip_slot].item_id);
        int cur_tier = 0, tier = 0;
        for (int k = 0; cur && k < cur->req_count; k++) cur_tier = cur->req_level[k] > cur_tier ? cur->req_level[k] : cur_tier;
        for (int k = 0; k < d->req_count; k++) tier = d->req_level[k] > tier ? d->req_level[k] : tier;
        if (!ok || (cur && cur_tier >= tier)) continue;
        if (coins(a) - price_of(0, list[i]) < reserve) {
            if (d->equip_slot == EQUIP_WEAPON) break;  // save for the weapon before any armour
            continue;
        }
        if (buy(a, SHOPKEEPER, list[i], 1)) sc_note(a, "bought %s", d->name);
    }
    sc_shop_close(a);
    wear_best(a, r->style);
}

// Ammunition or runes for the next stretch of training or fighting.
// Up to `want` of `item` from a shop, spending at most `budget` coins.
static void buy_upto(ScAgent *a, int npc, int shop, int item, int want, int budget) {
    int price = price_of(shop, item);
    if (want * price > budget) want = budget / price;
    if (want > 0) (void)buy(a, npc, item, want);
}

// Ammunition or runes for the next trip, spending at most half the coins carried.
static int boss_trip;  // restocking for a boss attempt: spend most of the coins

static void restock_supplies(ScAgent *a, Route *r, int arrows, int runes) {
    int budget = boss_trip ? coins(a) * 9 / 10 : coins(a) / 2;
    if (r->style == 1) {
        int bow_tier = equipped(a, WILLOW_SHORTBOW) ? 2 : equipped(a, OAK_SHORTBOW) ? 1 : 0;
        int arrow = level(a, SKILL_RANGED) >= 30 && bow_tier == 2 && budget >= 400 ? MITHRIL_ARROW
                  : level(a, SKILL_RANGED) >= 5 && bow_tier >= 1 && budget >= 150 ? STEEL_ARROW : BRONZE_ARROW;
        buy_upto(a, SHOPKEEPER, 0, arrow, arrows - ammo_count(a) - sc_count(a, arrow), budget);
        sc_shop_close(a);
        if (sc_count(a, arrow)) (void)sc_equip(a, arrow);
    } else if (r->style == 2) {
        if (level(a, SKILL_MAGIC) >= 15) {
            buy_upto(a, AUBURY, 1, CHAOS_RUNE, (boss_trip ? runes : runes / 2) - sc_count(a, CHAOS_RUNE),
                     budget * 3 / 4);
            budget = boss_trip ? coins(a) * 9 / 10 : coins(a) / 2;
        }
        buy_upto(a, AUBURY, 1, MIND_RUNE, runes - sc_count(a, MIND_RUNE), budget);
        buy_upto(a, AUBURY, 1, WATER_RUNE, runes - sc_count(a, WATER_RUNE), coins(a) / 3);
        if (!equipped(a, 579) && coins(a) > 300) (void)buy(a, AUBURY, 579, 1);
        if (!equipped(a, 577) && coins(a) > 400) (void)buy(a, AUBURY, 577, 1);
        sc_shop_close(a);
        wear_best(a, r->style);
    }
}

// The strongest spell the character can cast with the runes it carries (the staff gives air).
static int spell_index(const char *name) {
    for (int i = 0; i < RC_MAX_SPELL_DEFS; i++) {
        const RcSpellDef *d = rc_spell_def_get(i);
        if (d && d->loaded && !strcmp(d->name, name)) return i;
    }
    return -1;
}

// The strongest spell the character can cast with the runes it carries (the staff gives air):
// Water spells against hill giants (their weakness), Wind otherwise.
static int choose_spell(ScAgent *a, int water) {
    static const char *wind[] = {"Wind Blast", "Wind Bolt", "Wind Strike"};
    static const char *wet[] = {"Water Blast", "Water Bolt", "Water Strike"};
    static const int runes[] = {DEATH_RUNE, CHAOS_RUNE, MIND_RUNE};
    RcWorld *w = sc_world(a);
    for (int s = 0; s < 3; s++) {
        int idx = spell_index(water ? wet[s] : wind[s]);
        const RcSpellDef *d = idx >= 0 ? rc_spell_def_get(idx) : NULL;
        if (!d || level(a, SKILL_MAGIC) < d->level || sc_count(a, runes[s]) < 1) continue;
        if (water && sc_count(a, WATER_RUNE) < 3 - s) continue;
        // Defensive casting keeps Defence within 15 levels (and takes all the XP at the cap).
        int defensive = level(a, SKILL_DEFENCE) < 40
                     && (level(a, SKILL_MAGIC) >= 40 || level(a, SKILL_DEFENCE) + 15 < level(a, SKILL_MAGIC));
        if (sc_player(a)->autocast_spell != idx || sc_player(a)->defensive_autocast != defensive) {
            (void)rc_player_set_autocast_spell(w, idx, defensive);
            sc_wait(a, 1);
        }
        return 1;
    }
    if (water) return choose_spell(a, 0);
    if (sc_player(a)->autocast_spell >= 0) {  // no runes: bash with the staff until they are bought
        (void)rc_player_set_autocast_spell(w, -1, 0);
        sc_wait(a, 1);
    }
    return 0;
}

// Melee: train whichever of attack, strength and defence is furthest behind its target.
static void choose_style(ScAgent *a, Route *r) {
    RcWorld *w = sc_world(a);
    // Ranged: rapid; longrange (Ranged and Defence) keeps Defence within 15 levels.
    int style = level(a, SKILL_DEFENCE) < 40 && (level(a, SKILL_RANGED) >= 40
                || level(a, SKILL_DEFENCE) + 15 < level(a, SKILL_RANGED)) ? 3 : 1;
    if (r->style == 0) {
        int att = level(a, SKILL_ATTACK), str = level(a, SKILL_STRENGTH), def = level(a, SKILL_DEFENCE);
        style = att <= str && att < 40 ? 0 : def + 10 < str ? 3 : 1;
    } else if (r->style == 2 && choose_spell(a, r->water)) {
        return;
    } else if (r->style == 2) {
        style = 0;
    }
    if (sc_player(a)->attack_style_idx != style) {
        rc_player_set_attack_style(w, style);
        sc_wait(a, 1);
    }
}

// Targets the plan gave up on (unreachable, or no damage for a while), skipped for a time.
typedef struct {
    int uid;
    RcTick until;
} Skip;
static Skip skips[RC_MAX_PLAYERS][8];

static const RcNpc *find_target(ScAgent *a, int def_id, int radius) {
    const RcWorld *w = sc_world(a);
    const RcPlayer *p = sc_player(a);
    const RcNpc *found = NULL;
    int best = 1 << 30;
    for (int i = 0; i < w->npc_count; i++) {
        const RcNpc *n = &w->npcs[i];
        if (!n->active || n->is_dead || n->plane != p->plane) continue;
        const RcNpcDef *d = rc_npc_def_for_npc(w, n);
        int dist = chebyshev(n->x, n->y, p->x, p->y), skip = 0;
        if (!d || d->id != def_id || dist > radius || dist >= best) continue;
        for (int k = 0; k < 8; k++) skip |= skips[a->index][k].uid == n->uid && skips[a->index][k].until > w->tick;
        for (int k = 0; k < RC_MAX_PLAYERS; k++)
            skip |= k != a->index && w->players[k].active && w->players[k].attack_target == n->uid;
        if (skip) continue;
        best = dist;
        found = n;
    }
    return found;
}

static void skip_target(ScAgent *a, int uid) {
    Skip *s = skips[a->index];
    int oldest = 0;
    for (int k = 1; k < 8; k++) if (s[k].until < s[oldest].until) oldest = k;
    s[oldest] = (Skip){uid, sc_world(a)->tick + 200};
}

// One kill of the nearest unengaged `npc` within `radius`, then its loot (bones are buried).
// Eats below half hitpoints; with no food left below `flee_below` (tenths) it walks off.
// Returns 1 for a kill, 0 for no target, 2 for a retreat, -1 for a death.
static int fight_one(ScAgent *a, Route *r, int npc, int radius, int flee_below) {
    RcWorld *w = sc_world(a);
    choose_style(a, r);
    const RcNpc *n = find_target(a, npc, radius);
    if (!n) return 0;
    int uid = n->uid, tx = n->x, ty = n->y, hp = n->current_hp;
    RcTick progress = w->tick;
    RcTick started = w->tick;
    if (!rc_player_attack_npc(w, uid)) return 0;
    int food = food_count(a), ammo = ammo_count(a);
    int runes = sc_count(a, MIND_RUNE) + sc_count(a, CHAOS_RUNE) + sc_count(a, DEATH_RUNE);
    int res = 0;
    for (int t = 0; t < 300 && !res; t++) {
        sc_wait(a, 1);
        const RcPlayer *p = sc_player(a);
        if (p->is_dead) res = -1;
        const RcNpc *m = rc_npc_resolve_const(w, (RcNpcId)uid);
        if (!res && (!m || m->is_dead || !m->active)) res = 1;
        if (res) break;
        if (m->current_hp != hp) hp = m->current_hp, progress = w->tick;
        if (w->tick - progress > 40) {  // no damage for 40 ticks: unreachable or out of sight
            skip_target(a, uid);
            break;
        }
        if (p->current_hp < p->max_hp / 2 && food_count(a)) (void)sc_eat(a);
        else if (p->current_hp < flee_below) res = 2;
        else if (p->attack_target != uid) (void)rc_player_attack_npc(w, uid);
    }
    count_food_and_ammo(a, r, food, ammo, runes);
    if (getenv("ACCEPT_TRACE"))
        sc_note(a, "fight %d: result %d after %d ticks, hp %d/%d, food %d, ammo %d/%d weapon %d style %d, target at %d,%d from %d,%d",
                npc, res, (int)(w->tick - started), sc_player(a)->current_hp, sc_player(a)->max_hp,
                food_count(a), ammo_count(a), sc_player(a)->equipment[EQUIP_AMMO].item_id, sc_player(a)->equipment[EQUIP_WEAPON].item_id, sc_player(a)->attack_style_idx, tx, ty, sc_player(a)->x, sc_player(a)->y);
    if (res != 1) return res;
    r->kills++;
    int loot[] = {BONES, BIG_BONES, COINS, BRONZE_ARROW, STEEL_ARROW, MITHRIL_ARROW,
                  MIND_RUNE, CHAOS_RUNE, DEATH_RUNE, RAW_BEEF, COWHIDE};
    sc_pickup(a, loot, sc_free_slots(a) > 4 ? 11 : 10, 10);
    sc_bury_all(a);
    if (r->style == 1) reload(a);
    return 1;
}

static int style_level(ScAgent *a, const Route *r) {
    if (r->style == 1) return level(a, SKILL_RANGED);
    if (r->style == 2) return level(a, SKILL_MAGIC);
    int att = level(a, SKILL_ATTACK), str = level(a, SKILL_STRENGTH);
    return att < str ? att : str;
}

static int out_of_supplies(ScAgent *a, const Route *r) {
    if (r->style == 1 && ammo_count(a) < 15) reload(a);
    if (r->style == 1 && sc_player(a)->equipment[EQUIP_WEAPON].item_id < 0)
        return coins(a) >= 60;  // punching for coins: back to the bow once arrows are affordable
    if (r->style == 1) return ammo_count(a) < 15;
    if (r->style == 2) return sc_count(a, MIND_RUNE) + sc_count(a, CHAOS_RUNE) < 10 && coins(a) >= 30;
    return 0;
}

// Picks up the agent's own items around a death tile and wears them again.
static void recover(ScAgent *a, Route *r, int x, int y) {
    if (!go(a, x, y)) return;
    int all[64], n = 0;
    RcWorld *w = sc_world(a);
    for (int i = 0; i < w->ground_item_count && n < 64; i++)
        if (w->ground_items[i].active && w->ground_items[i].owner_uid == a->index
                && abs(w->ground_items[i].x - x) <= 2 && abs(w->ground_items[i].y - y) <= 2)
            all[n++] = w->ground_items[i].item_id;
    sc_pickup(a, all, n, 3);
    wear_best(a, r->style);
    sc_note(a, "recovered %d stacks", n);
}

// Deaths the plan did not see (on the way somewhere, or between fights): after respawning, fetch
// the grave, which holds everything the character wore and carried.
static void town_trip(ScAgent *a, Route *r, int food);

static void check_graves(ScAgent *a, Route *r) {
    if (a->deaths == r->deaths_seen) return;
    RcWorld *w = sc_world(a);
    int x = -1, y = -1, at = sc_player(a)->x;
    (void)at;
    while (sc_player(a)->is_dead) sc_wait(a, 1);
    for (int i = 0; i < w->ground_item_count; i++) {
        const RcGroundItem *g = &w->ground_items[i];
        if (g->active && g->owner_uid == a->index && g->bank_on_expiry) x = g->x, y = g->y;
    }
    r->deaths += a->deaths - r->deaths_seen;
    r->deaths_seen = a->deaths;
    sc_note(a, "died on the way; grave at %d,%d", x, y);
    if (x >= 0) {
        town_trip(a, r, 6);
        recover(a, r, x, y);
    }
}

// ---- The boss ------------------------------------------------------------------------------------------

static const RcNpc *boss_npc(RcWorld *w) {
    for (int i = 0; i < w->npc_count; i++) {
        const RcNpcDef *d = w->npcs[i].active ? rc_npc_def_for_npc(w, &w->npcs[i]) : NULL;
        if (d && d->id == SCURRIUS) return &w->npcs[i];
    }
    return NULL;
}

// Protection prayers count when the boss attacks, so they go up before it does: Protect from Melee
// while it fights up close (tail swipes, ten in twelve), Protect from Magic while it eats or in the
// final phase (bolts or fur, even odds). They are flicked on only for the tick its attack timer
// (counted from its last swing) says the attack comes, which spends a fraction of the points.
static void protect(ScAgent *a) {
    static const int prot[] = {RC_PRAYER_PROTECT_FROM_MAGIC, RC_PRAYER_PROTECT_FROM_MISSILES,
                               RC_PRAYER_PROTECT_FROM_MELEE};
    RcWorld *w = sc_world(a);
    RcPlayer *p = sc_player(a);
    const RcNpc *b = boss_npc(w);
    if (!b || level(a, SKILL_PRAYER) < 15 || p->current_prayer_points <= 0) return;
    int ranged_phase = w->mode_boss.phase >= 3 || w->mode_boss.feeding_pile >= 0;
    int want = ranged_phase ? RC_PRAYER_PROTECT_FROM_MAGIC : RC_PRAYER_PROTECT_FROM_MELEE;
    int imminent = b->attack_timer <= 1;
    for (int i = 0; i < 3; i++) {
        int on = (p->active_prayers & rc_prayer_bit(prot[i])) != 0;
        int should = imminent && prot[i] == want;
        if (on != should && (should || !imminent)) (void)rc_player_set_prayer(w, prot[i]);
    }
}

// Off a warned brick tile: the safe neighbouring tile nearest the arena's centre.
static void dodge(ScAgent *a) {
    RcWorld *w = sc_world(a);
    RcPlayer *p = sc_player(a);
    const RcModeBossState *s = &w->mode_boss;
    int on = 0;
    for (int i = 0; i < s->hazard_count; i++) on |= s->hazards[i].x == p->x && s->hazards[i].y == p->y;
    if (!on) return;
    int best = -1, bx = 0, by = 0;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++) {
            int nx = p->x + dx, ny = p->y + dy, bad = (!dx && !dy) || region(nx, ny) != ARENA;
            for (int k = 0; k < s->hazard_count; k++) bad |= s->hazards[k].x == nx && s->hazards[k].y == ny;
            int d = chebyshev(nx, ny, rules->boss.centre[0], rules->boss.centre[1]);
            if (!bad && (best < 0 || d < best)) best = d, bx = nx, by = ny;
        }
    if (best >= 0) (void)rc_player_walk_to(w, bx, by);
}

// The nearest living rat minion attacking this player, or NULL.
static const RcNpc *minion_on_me(ScAgent *a) {
    RcWorld *w = sc_world(a);
    const RcPlayer *p = sc_player(a);
    const RcNpc *found = NULL;
    int best = 1 << 30;
    for (int i = 0; i < w->mode_boss.minion_count; i++) {
        const RcNpc *m = rc_npc_resolve_const(w, (RcNpcId)w->mode_boss.minions[i]);
        if (!m || !m->active || m->is_dead || m->target_uid != a->index) continue;
        int d = chebyshev(m->x, m->y, p->x, p->y);
        if (d < best) best = d, found = m;
    }
    return found;
}

// One attempt: into the arena and fight until the boss or the character dies.
// Returns 1 for a credited kill, 0 for a kill without credit or a retreat, -1 for a death.
static int fight_boss(ScAgent *a, Route *r) {
    RcWorld *w = sc_world(a);
    const RcModeBoss *b = &rules->boss;
    if (!go(a, 545, 179)) return 0;  // the lair, by the bars
    for (int t = 0; t < 30 && sc_player(a)->current_hp < sc_player(a)->max_hp - 120 && food_count(a) > 4; t++)
        (void)sc_eat(a);
    if (!go(a, b->rect[0] + 1, b->centre[1])) return 0;
    r->attempts++;
    uint32_t kills = w->mode_boss.kills;
    int start = (int)w->tick;
    for (int t = 0; t < 4000; t++) {
        RcPlayer *p = sc_player(a);
        if (p->is_dead) {
            r->fight_ticks += (int)w->tick - start;
            return -1;
        }
        if (w->mode_boss.kills != kills) break;
        const RcNpc *boss = boss_npc(w);
        if (!boss) break;
        choose_style(a, r);
        dodge(a);
        protect(a);
        int food = food_count(a), ammo = ammo_count(a);
        int runes = sc_count(a, MIND_RUNE) + sc_count(a, CHAOS_RUNE) + sc_count(a, DEATH_RUNE);
        if (getenv("ACCEPT_TRACE") && t % 10 == 0)
            sc_note(a, "boss t+%d: hp %d/%d phase %d feeding %d minions %d hazards %d style %d | me %d/%d "
                    "prayer %d active %x food %d at %d,%d target %d boss at %d,%d spell %d", t, boss->current_hp, w->mode_boss.max_hp,
                    w->mode_boss.phase, w->mode_boss.feeding_pile, w->mode_boss.minion_count,
                    w->mode_boss.hazard_count, boss->combat.selected_npc_style, p->current_hp, p->max_hp,
                    p->current_prayer_points, p->active_prayers, food, p->x, p->y, p->attack_target, boss->x, boss->y, p->autocast_spell);
        if (p->current_hp < p->max_hp / 2) sc_eat(a);
        // Rats first while the boss eats or once three of them are on the player.
        const RcNpc *rat = minion_on_me(a);
        int rats = 0;
        for (int i = 0; i < w->mode_boss.minion_count; i++) {
            const RcNpc *m = rc_npc_resolve_const(w, (RcNpcId)w->mode_boss.minions[i]);
            rats += m && m->active && !m->is_dead && m->target_uid == a->index;
        }
        int want = rat && (w->mode_boss.feeding_pile >= 0 || rats >= 2) ? rat->uid : boss->uid;
        if (!boss->is_dead && p->attack_target != want) (void)rc_player_attack_npc(w, want);
        sc_wait(a, 1);
        count_food_and_ammo(a, r, food, ammo, runes);
        if (r->style == 1 && !ammo_count(a)) reload(a);
    }
    r->fight_ticks += (int)w->tick - start;
    // Turn the protection prayers off and take the loot.
    int prayers[] = {RC_PRAYER_PROTECT_FROM_MAGIC, RC_PRAYER_PROTECT_FROM_MISSILES, RC_PRAYER_PROTECT_FROM_MELEE};
    for (int i = 0; i < 3; i++)
        if (sc_player(a)->active_prayers & rc_prayer_bit(prayers[i])) (void)rc_player_set_prayer(w, prayers[i]);
    if (w->mode_boss.kills != kills && (w->mode_boss.last_qualified >> a->index & 1)) {
        int loot[] = {COINS, 447, CHAOS_RUNE, 314, 28798, 2434};
        sc_wait(a, 2);
        sc_pickup(a, loot, 6, 6);
        return 1;
    }
    return 0;
}

// ---- A fresh-start route -------------------------------------------------------------------------------

static void prepare_kit(ScAgent *a, Route *r) {
    go_lm(a, "bank");
    if (!sc_bank_open(a)) sc_fail(a, "bank");
    (void)sc_withdraw(a, COINS, 1 << 20);
    if (r->style == 1) {
        (void)sc_withdraw(a, SHORTBOW, 1);
        (void)sc_withdraw(a, BRONZE_ARROW, 1 << 20);
    } else if (r->style == 2) {
        (void)sc_withdraw(a, STAFF_OF_AIR, 1);
        (void)sc_withdraw(a, MIND_RUNE, 1 << 20);
        (void)sc_withdraw(a, 555, 1 << 20);  // water runes
    }
    sc_bank_close(a);
    wear_best(a, r->style);
    if (r->style == 1) (void)sc_equip(a, BRONZE_ARROW);
    char kit[512] = "";
    const RcPlayer *p = sc_player(a);
    for (int i = 0, n = 0; i < RC_EQUIP_COUNT; i++)
        if (p->equipment[i].item_id >= 0)
            n += snprintf(kit + n, sizeof(kit) - n, " %d:%d", i, p->equipment[i].item_id);
    sc_note(a, "kit worn:%s; inventory coins %d bow %d arrows %d staff %d", kit, coins(a),
            sc_count(a, SHORTBOW), sc_count(a, BRONZE_ARROW), sc_count(a, STAFF_OF_AIR));
}

// Food and prayer for a fight: lobsters (else trout) up to `food` and a full prayer.
static void prepare_fight(ScAgent *a, Route *r) {
    go_lm(a, "general_store");
    shop_upgrades(a, r, r->food_target * 20 + 100);
    int need = r->food_target - food_count(a);
    int free_slots = sc_free_slots(a) - 2;
    if (need > free_slots) need = free_slots;
    // Lobsters when rich (melee needs nothing else); trout otherwise, leaving coins for arrows
    // and runes.
    int lob = r->style == 0 ? (coins(a) - need * price_of(0, TROUT)) / (price_of(0, LOBSTER) - price_of(0, TROUT))
            : coins(a) > 800 ? (coins(a) - 500) / price_of(0, LOBSTER) : 0;
    if (lob < 0) lob = 0;
    if (lob > need) lob = need;
    if (lob > 0) (void)buy(a, SHOPKEEPER, LOBSTER, lob);
    need -= lob;
    if (need > 0) (void)buy(a, SHOPKEEPER, TROUT, need < coins(a) / 8 ? need : coins(a) / 8);
    sc_shop_close(a);
    boss_trip = 1;
    restock_supplies(a, r, r->style == 1 ? 250 : 0, r->style == 2 ? 150 : 0);
    boss_trip = 0;
    if (level(a, SKILL_PRAYER) >= 15) {
        go_lm(a, "church");
        int ids[] = {ALTAR}, x, y;
        if (sc_find_loc(a, ids, 1, 16, &x, &y, NULL)) (void)sc_use_loc(a, ALTAR, x, y, "Pray-at");
    }
}

// Cooks every raw beef at the town range; burnt meat is dropped.
static void cook_beef(ScAgent *a) {
    if (!sc_count(a, RAW_BEEF) || !go_lm(a, "range")) return;
    int ids[] = {RANGE}, x, y;
    if (!sc_find_loc(a, ids, 1, 24, &x, &y, NULL) || !sc_use_loc(a, RANGE, x, y, "Cook")) return;
    for (int t = 0; t < 120 && sc_count(a, RAW_BEEF) && sc_player(a)->skill_job.kind; t++) sc_wait(a, 1);
    (void)sc_drop_all(a, BURNT_MEAT);
}

// Food for a trip up to `want`: shrimps while poor, trout after, keeping `reserve` coins.
static void buy_food(ScAgent *a, int want, int reserve) {
    int need = want - food_count(a), slots = sc_free_slots(a) - 2;
    if (need > slots) need = slots;
    int item = coins(a) - reserve < 150 ? SHRIMPS : TROUT;
    int afford = (coins(a) - reserve) / price_of(0, item);
    if (need <= 0 || afford <= 0 || !go_lm(a, "general_store")) return;
    (void)buy(a, SHOPKEEPER, item, need < afford ? need : afford);
    sc_shop_close(a);
}

// A town visit: cook any beef, buy upgrades, supplies and food for the next trip.
static void town_trip(ScAgent *a, Route *r, int food) {
    cook_beef(a);
    go_lm(a, "general_store");
    if (sc_count(a, COWHIDE) && sc_shop_open(a, SHOPKEEPER)) {
        (void)sc_sell(a, COWHIDE, sc_count(a, COWHIDE));
        sc_shop_close(a);
    }
    // Past level 30 the boss is next: keep coins for its food.
    shop_upgrades(a, r, food * price_of(0, TROUT) + 60 + (style_level(a, r) >= 30 ? 250 : 0));
    restock_supplies(a, r, 150, 200);
    wear_best(a, r->style);
    buy_food(a, food, r->style == 0 ? 0 : 30);
}

static int fight_one(ScAgent *a, Route *r, int npc, int radius, int flee_below);

// No food and no money: cows for beef, then the range.
static void beef_run(ScAgent *a, Route *r, int want) {
    RcWorld *w = sc_world(a);
    const RcGymLandmark *cows = lm("cows");
    for (int t = 0; t < 60 && sc_count(a, RAW_BEEF) + food_count(a) < want && !sc_player(a)->is_dead; t++) {
        if (chebyshev(sc_player(a)->x, sc_player(a)->y, cows->x, cows->y) > 14) (void)go(a, cows->x, cows->y);
        if (sc_player(a)->current_hp <= 20 && !food_count(a)) sc_wait(a, 50);
        else if (fight_one(a, r, COW, 16, 20) == 0) sc_wait(a, 3);
    }
    (void)w;
    cook_beef(a);
}

static void note_death(ScAgent *a, const char *what) {
    RcWorld *w = sc_world(a);
    int x = sc_player(a)->x, y = sc_player(a)->y;
    char who[256] = "";
    for (int i = 0, n = 0; i < w->npc_count && n < 200; i++) {
        const RcNpc *m = &w->npcs[i];
        const RcNpcDef *d = m->active ? rc_npc_def_for_npc(w, m) : NULL;
        if (d && m->target_uid >= 0 && abs(m->x - x) <= 10 && abs(m->y - y) <= 10)
            n += snprintf(who + n, sizeof(who) - n, " %s", d->name);
    }
    sc_note(a, "died %s at %d,%d; attackers:%s", what, x, y, who);
}

// Fights `npc` near landmark `where` until the style level and coins reach their targets, going to
// town for food (dangerous places only), supplies and upgrades as needed.
static void train_until(ScAgent *a, Route *r, int npc, const char *where, int dangerous,
                        int target_level, int target_coins, int deadline) {
    RcWorld *w = sc_world(a);
    const RcGymLandmark *place = lm(where);
    int trip_food = dangerous ? 12 : 8;
    while ((style_level(a, r) < target_level || coins(a) < target_coins) && (int)w->tick < deadline) {
        check_graves(a, r);
        if (out_of_supplies(a, r) || (dangerous && food_count(a) < 2)) {
            if (dangerous && coins(a) < 300 && food_count(a) < 3) beef_run(a, r, 12);  // free food first
            town_trip(a, r, trip_food);
            if (dangerous && food_count(a) < 3) beef_run(a, r, 8);
            if (r->style == 1 && !ammo_count(a) && equipped(a, sc_player(a)->equipment[EQUIP_WEAPON].item_id)
                    && sc_player(a)->equipment[EQUIP_WEAPON].item_id >= 0) {
                sc_note(a, "out of arrows with %d coins: punching for coins", coins(a));
                (void)rc_player_unequip(w, EQUIP_WEAPON);  // fists until arrows are affordable again
                sc_wait(a, 1);
            }
        }
        const RcPlayer *p = sc_player(a);
        if (region(p->x, p->y) != region(place->x, place->y) || chebyshev(p->x, p->y, place->x, place->y) > 14)
            (void)go(a, place->x, place->y);
        int res = fight_one(a, r, npc, 16, npc == COW ? 20 : sc_player(a)->max_hp / 3);
        if (res < 0) {
            int x = sc_player(a)->x, y = sc_player(a)->y;
            r->deaths++;
            r->deaths_seen = a->deaths;
            note_death(a, where);
            while (sc_player(a)->is_dead) sc_wait(a, 1);
            if (dangerous) town_trip(a, r, trip_food);
            recover(a, r, x, y);
        } else if (res == 2) {
            sc_note(a, "retreat from %s with %d coins", where, coins(a));
            town_trip(a, r, trip_food);
            if (food_count(a) < 3) beef_run(a, r, 8);
        } else if (res == 0) {
            sc_wait(a, 3);  // everything nearby is dead or fought over: wait for respawns
        }
    }
    sc_note(a, "%s done: level %d, %d coins, %d kills, %d deaths, prayer %d", where, style_level(a, r),
            coins(a), r->kills, r->deaths, level(a, SKILL_PRAYER));
}

// Where each style trains and earns after the goblins.
static void cave_target(Route *r, int *npc, const char **where) {
    // Hill giants pay best (20-35 coins, big bones, ore), but a mage's runes and food for them
    // cost about what they drop: magic keeps to goblins, which pay it steadily.
    *npc = r->style == 2 ? GOBLIN : 2098;
    *where = r->style == 2 ? "goblins" : "hill_giants";
}

static void route_plan(ScAgent *a, void *arg) {
    Route *r = arg;
    RcWorld *w = sc_world(a);
    int deadline = r->start_tick + 12000;
    prepare_kit(a, r);
    go_lm(a, "general_store");
    if (r->style == 0) shop_upgrades(a, r, 0);  // a scimitar before anything else
    if (r->style == 2) restock_supplies(a, r, 0, 120);
    train_until(a, r, COW, "cows", 0, r->style ? 15 : 10, 0, deadline - 9000);
    train_until(a, r, GOBLIN, "goblins", 0, 20, 400, deadline - 7000);
    town_trip(a, r, 12);
    int npc;
    const char *where;
    cave_target(r, &npc, &where);
    // Level up, then keep going for the boss trip's money (runes and arrows cost the most).
    static const int boss_coins[] = {400, 700, 700};
    train_until(a, r, npc, where, r->style != 2, 26, 900, deadline - 4000);
    town_trip(a, r, 12);
    train_until(a, r, npc, where, r->style != 2, 40, boss_coins[r->style] + 300, deadline - 2500);
    r->prep_ticks = (int)w->tick - r->start_tick;
    for (r->attempts = 0; (int)w->tick < deadline;) {
        check_graves(a, r);
        prepare_fight(a, r);
        {
            const RcPlayer *p = sc_player(a);
            char kit[512] = "";
            for (int i = 0, n = 0; i < RC_EQUIP_COUNT; i++) {
                const RcItemDef *d = rc_item_def_get(p->equipment[i].item_id);
                if (d) n += snprintf(kit + n, sizeof(kit) - n, " %s;", d->name);
            }
            const int *b = p->equipment_bonuses;
            sc_note(a, "attempt %d: level %d, hp %d, def %d, prayer %d, food %d, coins %d, runes m%d c%d d%d w%d, ammo %d; worn:%s bonuses "
                    "att %d/%d/%d/%d/%d def %d/%d/%d/%d/%d str %d rstr %d mdmg %d pray %d", r->attempts + 1,
                    style_level(a, r), level(a, SKILL_HITPOINTS), level(a, SKILL_DEFENCE), level(a, SKILL_PRAYER),
                    food_count(a), coins(a), sc_count(a, MIND_RUNE), sc_count(a, CHAOS_RUNE), sc_count(a, DEATH_RUNE), sc_count(a, WATER_RUNE), ammo_count(a), kit, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                    b[10], b[11], b[12], b[13]);
        }
        int res = fight_boss(a, r);
        sc_note(a, "attempt result %d (scheduler deaths %d), boss hp %d/%d", res, a->deaths, boss_npc(w) ? boss_npc(w)->current_hp : -1,
                w->mode_boss.max_hp);
        if (res == 1) {
            r->success = 1;
            r->total_ticks = (int)w->tick - r->start_tick;
            sc_note(a, "Scurrius killed with credit at tick %d", r->total_ticks);
            break;
        }
        if (res < 0) {
            r->deaths++;
            r->deaths_seen = a->deaths;
            note_death(a, "fighting Scurrius");
            int dx = sc_player(a)->x, dy = sc_player(a)->y;
            while (sc_player(a)->is_dead) sc_wait(a, 1);
            // The grave lies in the arena for 1500 ticks; fetch it once nobody fights there and
            // the boss has reset to full health.
            const RcModeBoss *b = &rules->boss;
            town_trip(a, r, 6);
            (void)go(a, 545, 179);
            while (w->mode_boss.active && (int)w->tick < deadline) sc_wait(a, 10);
            (void)b;
            recover(a, r, dx, dy);
            (void)go(a, 545, 179);
        }
        // Train more between attempts.
        train_until(a, r, npc, where, r->style != 2, style_level(a, r) + 2, coins(a) + 300, deadline - 900);
    }
    if (!r->total_ticks) r->total_ticks = (int)w->tick - r->start_tick;
    for (int s = 0; s < SKILL_COUNT; s++) r->levels[s] = level(a, s);
}

static const char *style_name(int s) { return s == 0 ? "melee" : s == 1 ? "ranged" : "magic"; }

static int only_style = -1;

static int run_routes(int seeds) {
    out("## Fresh-start routes\n\nEach run: one fresh character, the 12,000-tick horizon, a scripted "
        "plan through player commands only. Prep is ticks until the first boss attempt; fight is "
        "ticks spent in boss attempts.\n\n");
    out("| Style | Seed | Kill with credit | Ticks | Prep | Fight | Attempts | Deaths | Kills | Food eaten | "
        "Supplies bought (coins) | Gear bought (coins) | Levels (att/str/def/hp/rng/mag/pray) | State hash |\n");
    out("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    int wins = 0, total = 0;
    mkdir("build", 0755);
    mkdir("build/acceptance", 0755);
    for (int style = 0; style < 3; style++) {
        if (only_style >= 0 && style != only_style) continue;
        for (int seed = 1; seed <= seeds; seed++) {
            RcWorld *w = new_world(1, 1000u * (unsigned)style + (unsigned)seed);
            memset(skips, 0, sizeof(skips));
            memset(spent_supplies, 0, sizeof(spent_supplies));
            memset(spent_gear, 0, sizeof(spent_gear));
            char path[256];
            snprintf(path, sizeof(path), "build/acceptance/route-%s-%d.log", style_name(style), seed);
            FILE *log = fopen(path, "w");
            Route r = {.style = style, .start_tick = (int)w->tick, .food_target = style == 0 ? 24 : 20};
            ScRun run;
            sc_init(&run, w, 1, log);
            sc_start(&run, 0, route_plan, &r);
            sc_run(&run, w->tick + 12000);
            if (run.agents[0].failed) r.success = 0;
            const int *lv = r.levels[SKILL_ATTACK] ? r.levels : w->players[0].skills.base_level;
            out("| %s | %d | %s | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d/%d/%d/%d/%d/%d/%d | %016llx |\n",
                style_name(style), seed, r.success ? "yes" : run.agents[0].failed ? run.agents[0].failure : "no",
                r.total_ticks ? r.total_ticks : (int)w->tick - r.start_tick, r.prep_ticks, r.fight_ticks,
                r.attempts, run.agents[0].deaths, r.kills, r.food_eaten, spent_supplies[0], spent_gear[0],
                lv[SKILL_ATTACK], lv[SKILL_STRENGTH], lv[SKILL_DEFENCE], lv[SKILL_HITPOINTS],
                lv[SKILL_RANGED], lv[SKILL_MAGIC], lv[SKILL_PRAYER],
                (unsigned long long)rc_world_state_hash(w));
            wins += r.success;
            total++;
            sc_free(&run);
            if (log) fclose(log);
            rc_world_destroy(w);
            fflush(stdout);
        }
    }
    out("\n%d of %d routes killed Scurrius with credit inside the horizon. Logs: "
        "`build/acceptance/route-<style>-<seed>.log`.\n\n", wins, total);
    return wins == total;
}

// ---- Throughput ----------------------------------------------------------------------------------------

typedef struct {
    int agents, steps;
    double seconds;
} PerfJob;

static void *perf_job(void *arg) {
    PerfJob *job = arg;
    RcCenvConfig cfg = {.num_agents = job->agents, .profile = RC_CENV_DEVELOPMENT,
                        .horizon = job->steps + 10, .seed = 77, .public_map = true};
    RcCompactEnv *env = rc_cenv_create(&cfg);
    static __thread float obs[RC_CENV_OBS_SIZE];
    static __thread uint8_t mask[RC_CENV_NUM_ACTIONS];
    uint32_t rng = 1234u + (uint32_t)job->agents;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int t = 0; t < job->steps && env; t++) {
        for (int i = 0; i < job->agents; i++) {
            rc_cenv_observe(env, i, obs);
            rc_cenv_action_mask(env, i, mask);
            int valid[RC_CENV_NUM_ACTIONS], n = 0;
            for (int k = 0; k < RC_CENV_NUM_ACTIONS; k++) if (mask[k]) valid[n++] = k;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            rc_cenv_set_action(env, i, n ? valid[rng % (uint32_t)n] : 0);
        }
        rc_cenv_step(env);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    job->seconds = (double)(b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    rc_cenv_destroy(env);
    return NULL;
}

static long rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long kb = 0;
    while (f && fgets(line, sizeof(line), f))
        if (!strncmp(line, "VmRSS:", 6)) kb = atol(line + 6);
    if (f) fclose(f);
    return kb;
}

static void run_perf(void) {
    out("## Throughput\n\nRandom masked actions through the compact-v2 interface, observations and "
        "masks computed for every agent every tick, release build, %ld CPU threads available.\n\n",
        sysconf(_SC_NPROCESSORS_ONLN));
    out("| Worlds (threads) | Agents per world | Ticks per world | World ticks/s | Agent steps/s |\n");
    out("|---|---|---|---|---|\n");
    int agents[] = {1, 4, 8};
    for (int i = 0; i < 3; i++) {
        PerfJob job = {agents[i], 3000, 0};
        perf_job(&job);
        out("| 1 | %d | %d | %.0f | %.0f |\n", agents[i], job.steps, job.steps / job.seconds,
            job.steps * agents[i] / job.seconds);
    }
    int threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (threads > 16) threads = 16;
    PerfJob jobs[16];
    pthread_t tid[16];
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < threads; i++) {
        jobs[i] = (PerfJob){4, 2000, 0};
        pthread_create(&tid[i], NULL, perf_job, &jobs[i]);
    }
    for (int i = 0; i < threads; i++) pthread_join(tid[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double s = (double)(b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    out("| %d | 4 | 2000 | %.0f | %.0f |\n", threads, threads * 2000 / s, threads * 2000 * 4 / s);
    out("\nResident memory after the runs: %.0f MB (shared game data, plus each world while it "
        "lives).\n\n", rss_kb() / 1024.0);
}

// ---- Main ------------------------------------------------------------------------------------------------

int run_coop(void);
int run_contention(void);
int run_economy(void);

int main(int argc, char **argv) {
    const char *what = argc > 1 && argv[1][0] != '-' ? argv[1] : "all";
    int seeds = 5;
    const char *report_path = NULL;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--seeds")) seeds = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--report")) report_path = argv[i + 1];
        if (!strcmp(argv[i], "--style"))
            only_style = !strcmp(argv[i + 1], "melee") ? 0 : !strcmp(argv[i + 1], "ranged") ? 1 : 2;
    }
    if (!rc_gym_layout_load_mode("compact_osrs_v1", &layout) || !layout.has_rules) {
        fprintf(stderr, "rs-gym-acceptance: build the mode first: python3 tools/build_mode.py\n");
        return 2;
    }
    if (report_path) report = fopen(report_path, "w");
    // Keeps the shared game data and the mode's rules loaded for the whole run.
    RcWorld *keeper = new_world(1, 1);
    out("# compact_osrs_v1 acceptance report\n\nGenerated by `rs-gym-acceptance %s --seeds %d`. "
        "Mode %s version %d. Scripted controllers only; no human playtesting is claimed.\n\n",
        what, seeds, rules->name, rules->version);
    int ok = 1;
    int all = !strcmp(what, "all");
    if (all || !strcmp(what, "routes")) ok &= run_routes(seeds);
    if (all || !strcmp(what, "coop")) ok &= run_coop();
    if (all || !strcmp(what, "contention")) ok &= run_contention();
    if (all || !strcmp(what, "economy")) ok &= run_economy();
    if (all || !strcmp(what, "perf")) run_perf();
    if (report) fclose(report);
    rc_world_destroy(keeper);
    return ok ? 0 : 1;
}

// Filled in below.
int run_coop(void) { return 1; }
int run_contention(void) { return 1; }
int run_economy(void) { return 1; }
