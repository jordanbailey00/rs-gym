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
    RAW_BEEF = 2132, COWHIDE = 1739, SMALL_NET = 303, FLY_ROD = 309, FEATHER = 314,
    RAW_SHRIMPS = 317, RAW_TROUT = 335, BURNT_SHRIMP = 7954, BURNT_FISH = 343, NET_SPOT = 1525,
    ROD_SPOT = 1526, BRONZE_PICKAXE = 1265, PURE_ESSENCE = 7936, ESSENCE_ROCK = 34773,
    MIND_ALTAR = 34761, COOKED_MEAT = 2142, BURNT_MEAT = 2146, RANGE = 7183,
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
    struct Team *team;      // co-op: suppliers to trade with and a partner to fight beside
    int post_boss;          // after the kill: hunt this NPC for the rest of the horizon
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

// Throughput by activity (all routes together): kills, ticks spent (travel and town trips
// included) and coins picked up from the kills.
enum { ACT_GOBLIN, ACT_GIANT, ACT_WIZARD, ACT_COUNT };
static struct { int kills, ticks, coins; } activity[ACT_COUNT];

static int activity_of(int npc) {
    return npc == GOBLIN ? ACT_GOBLIN : npc == 2098 ? ACT_GIANT : npc == 510 ? ACT_WIZARD : -1;
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
    int act = activity_of(npc), coins_before = coins(a);
    if (act >= 0) activity[act].kills++;
    int loot[] = {BONES, BIG_BONES, COINS, BRONZE_ARROW, STEEL_ARROW, MITHRIL_ARROW,
                  MIND_RUNE, CHAOS_RUNE, DEATH_RUNE, RAW_BEEF, COWHIDE};
    sc_pickup(a, loot, sc_free_slots(a) > 4 ? 11 : 10, 10);
    if (act >= 0) activity[act].coins += coins(a) - coins_before;
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

// ---- Co-op: suppliers, trades and a shared fight ------------------------------------------------------

typedef struct Team {
    int fighters[2], cook, crafter;
    volatile int waiting[RC_MAX_PLAYERS];   // a fighter at the bank waiting for its supplies
    volatile int in_lair[RC_MAX_PLAYERS];   // a fighter by the bars, waiting for its partner
    int served[RC_MAX_PLAYERS];             // supplier trades completed with each fighter
    int food_made, runes_made, trades, food_ticks, rune_ticks;
    int kill_tick, kill_by;
    int boss_max_hp, participants;          // the largest scaled fight seen
} Team;

static int free_slots_of(const RcPlayer *p) {
    int n = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) n += p->inventory[i].item_id < 0;
    return n;
}

// One trade with `partner`, who runs the same call mirrored: offer `qty` of `give` (all of it for
// qty < 0, nothing for 0), accept once the partner's offer is in, and wait for the commit.
// Returns 1 when the trade completed.
static int trade_with(ScAgent *a, int partner, int give, int qty) {
    RcWorld *w = sc_world(a);
    uint32_t done = rc_trade_completed_count(w);
    for (int t = 0; t < 150 && sc_player(a)->trade.stage != RC_TRADE_STAGE_OPEN; t++) {
        if (t % 20 == 0) (void)rc_trade_request(w, partner);
        sc_wait(a, 1);
    }
    if (sc_player(a)->trade.stage != RC_TRADE_STAGE_OPEN) {
        sc_note(a, "trade with %d never opened (stage %d, result %d)", partner, sc_player(a)->trade.stage,
                sc_player(a)->trade.last_result);
        return 0;
    }
    int slot = give >= 0 ? sc_slot(a, give) : -1;
    if (slot >= 0 && qty) (void)rc_trade_offer(w, slot, qty < 0 ? sc_count(a, give) : qty);
    for (int t = 0; t < 60; t++) {
        sc_wait(a, 1);
        const RcPlayer *p = sc_player(a);
        if (rc_trade_completed_count(w) != done && p->trade.stage == RC_TRADE_STAGE_NONE
                && p->trade.last_result == RC_TRADE_COMPLETED)
            return 1;
        if (p->trade.stage != RC_TRADE_STAGE_OPEN) {
            sc_note(a, "trade with %d closed: result %d", partner, p->trade.last_result);
            return 0;
        }
        int mine = !qty || slot < 0 || p->trade.offer_count > 0;
        int theirs = w->players[partner].trade.offer_count > 0;
        if (mine && theirs && p->trade.accepted_revision != p->trade.revision)
            (void)rc_trade_accept(w, p->trade.revision);
    }
    sc_note(a, "trade with %d did not complete: result %d/%d, offers %d/%d, accepted %u/%u of %u, free %d/%d",
            partner, sc_player(a)->trade.last_result, w->players[partner].trade.last_result,
            sc_player(a)->trade.offer_count, w->players[partner].trade.offer_count,
            sc_player(a)->trade.accepted_revision, w->players[partner].trade.accepted_revision,
            sc_player(a)->trade.revision, sc_free_slots(a), free_slots_of(&w->players[partner]));
    (void)rc_trade_decline(w);
    sc_wait(a, 1);
    return 0;
}

// A fighter's supplies from the team: food from the cook, runes from the runecrafter (for a mage),
// each paid for in coins at the bank.
static void team_supplies(ScAgent *a, Route *r) {
    Team *t = r->team;
    RcWorld *w = sc_world(a);
    go_lm(a, "bank");
    t->waiting[a->index] = 1;
    int pay = coins(a) < 60 ? coins(a) : 60;
    sc_note(a, "at the bank for supplies with %d coins, %d free slots", coins(a), sc_free_slots(a));
    if (trade_with(a, t->cook, COINS, pay)) sc_note(a, "bought %d food from the cook", food_count(a));
    if (r->style == 2) {
        pay = coins(a) < 60 ? coins(a) : 60;
        int before = sc_count(a, MIND_RUNE);
        if (trade_with(a, t->crafter, COINS, pay))
            sc_note(a, "bought %d mind runes from the runecrafter", sc_count(a, MIND_RUNE) - before);
    }
    t->waiting[a->index] = 0;
    (void)w;
}

// Both fighters wait by the bars so they enter together (up to 300 ticks).
static void team_gather(ScAgent *a, Route *r) {
    Team *t = r->team;
    t->in_lair[a->index] = 1;
    for (int k = 0; k < 300; k++) {
        int all = 1;
        for (int f = 0; f < 2; f++) all &= t->in_lair[t->fighters[f]] || sc_world(a)->players[t->fighters[f]].is_dead;
        if (all) break;
        sc_wait(a, 1);
    }
}

// One attempt: into the arena and fight until the boss or the character dies.
// Returns 1 for a credited kill, 0 for a kill without credit or a retreat, -1 for a death.
static int fight_boss(ScAgent *a, Route *r) {
    RcWorld *w = sc_world(a);
    const RcModeBoss *b = &rules->boss;
    if (!go(a, 545, 179)) return 0;  // the lair, by the bars
    if (r->team) team_gather(a, r);
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
        if (r->team && w->mode_boss.max_hp > r->team->boss_max_hp) {
            r->team->boss_max_hp = w->mode_boss.max_hp;
            r->team->participants = __builtin_popcount(w->mode_boss.participants);
        }
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
    if (r->team) r->team->in_lair[a->index] = 0;
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
    if (r->team) team_supplies(a, r);
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
    int trip_food = dangerous ? 12 : 8, last = (int)w->tick, act = activity_of(npc);
    while ((style_level(a, r) < target_level || coins(a) < target_coins) && (int)w->tick < deadline) {
        if (act >= 0) activity[act].ticks += (int)w->tick - last;  // counted as it goes: a horizon
        last = (int)w->tick;                                      // can end the plan mid-loop
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
    if (act >= 0) activity[act].ticks += (int)w->tick - last;
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
        if (res == 1 || (r->team && r->team->kill_tick)) {
            if (r->team && !r->team->kill_tick) r->team->kill_tick = (int)w->tick, r->team->kill_by = a->index;
            r->success = res == 1;
            r->total_ticks = (int)w->tick - r->start_tick;
            sc_note(a, "Scurrius killed with credit at tick %d", r->total_ticks);
            // The rest of the horizon: an ordinary activity after the first kill.
            if (r->post_boss) {
                const char *place = r->post_boss == 510 ? "dark_wizards" : "goblins";
                train_until(a, r, r->post_boss, place, 1, 41, 1 << 30, r->start_tick + 12000);
            }
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
    int wins = 0, total = 0, boss_fight_ticks = 0, boss_attempts = 0;
    memset(activity, 0, sizeof(activity));
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
            Route r = {.style = style, .start_tick = (int)w->tick, .food_target = style == 0 ? 24 : 20,
                       .post_boss = style == 1 ? 510 : 0};
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
            boss_fight_ticks += r.fight_ticks;
            boss_attempts += r.attempts;
            sc_free(&run);
            if (log) fclose(log);
            rc_world_destroy(w);
            fflush(stdout);
        }
    }
    out("\n%d of %d routes killed Scurrius with credit inside the horizon. Logs: "
        "`build/acceptance/route-<style>-<seed>.log`.\n\n", wins, total);
    static const char *names[] = {"Goblins (sewers)", "Hill giants (cave)", "Dark wizards (cave; ranged, after its kill)"};
    out("### Activity throughput (all routes above)\n\nTicks include travel, town trips and waiting for "
        "respawns; coins are those picked up from the kills.\n\n| Activity | Kills | Ticks | Kills per 1,000 "
        "ticks | Coins per kill | Coins per 1,000 ticks |\n|---|---|---|---|---|---|\n");
    for (int k = 0; k < ACT_COUNT; k++) {
        double t = activity[k].ticks > 0 ? activity[k].ticks : 1;
        out("| %s | %d | %d | %.1f | %.1f | %.0f |\n", names[k], activity[k].kills, activity[k].ticks,
            activity[k].kills * 1000.0 / t, activity[k].kills ? (double)activity[k].coins / activity[k].kills : 0.0,
            activity[k].coins * 1000.0 / t);
    }
    out("| Scurrius | %d | %d | %.1f | 150 plus one bundle | - |\n\n", wins, boss_fight_ticks,
        wins * 1000.0 / (boss_fight_ticks ? boss_fight_ticks : 1));
    (void)boss_attempts;
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
    double s = 0;  // the slowest world's stepping time (world creation excluded)
    for (int i = 0; i < threads; i++) s = jobs[i].seconds > s ? jobs[i].seconds : s;
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
        return 77;  // ctest's skip code
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

// ---- Co-op run ----------------------------------------------------------------------------------------

enum { SERVE_TICK = 8800, END_TICK = 12000 };

static int use_station(ScAgent *a, int id, const char *option) {
    int ids[] = {id}, x, y;
    return sc_find_loc(a, ids, 1, 24, &x, &y, NULL) && sc_use_loc(a, id, x, y, option);
}

// Waits at the bank and trades `item` to every fighter who comes for it (the fighter pays coins).
static void serve(ScAgent *a, Team *t, int item, int per_fighter, int mage_only) {
    RcWorld *w = sc_world(a);
    while ((int)w->tick < t->kill_tick + END_TICK * !t->kill_tick && (int)w->tick < END_TICK - 50) {
        for (int f = 0; f < 2; f++) {
            int fi = t->fighters[f];
            if (!t->waiting[fi] || (mage_only && f == 0) || !sc_count(a, item)) continue;
            // No more than the fighter can carry: a trade without room is cancelled.
            int qty = per_fighter < sc_count(a, item) ? per_fighter : sc_count(a, item);
            const RcItemDef *d = rc_item_def_get(item);
            if (d && !d->stackable && qty > free_slots_of(&w->players[fi])) qty = free_slots_of(&w->players[fi]);
            if (qty > 0 && trade_with(a, fi, item, qty)) {
                t->trades++;
                t->served[fi]++;
                sc_note(a, "traded with player %d", fi);
            }
            while (t->waiting[fi]) sc_wait(a, 1);
        }
        sc_wait(a, 1);
        if (!sc_count(a, item) && sc_bank_open(a)) {  // restock from the bank between fighters
            (void)sc_withdraw(a, item, 26);
            sc_bank_close(a);
            if (!sc_count(a, item)) break;
        }
    }
}

// The cook: shrimps with the small net until Fishing and Cooking reach 15, then trout with the fly
// rod and feathers; cooked at the range, banked, then traded to the fighters.
static void cook_plan(ScAgent *a, void *arg) {
    Team *t = arg;
    RcWorld *w = sc_world(a);
    go_lm(a, "bank");
    if (!sc_bank_open(a)) sc_fail(a, "bank");
    (void)sc_withdraw(a, COINS, 1 << 20);
    (void)sc_withdraw(a, SMALL_NET, 1);
    (void)sc_withdraw(a, FLY_ROD, 1);
    (void)sc_withdraw(a, FEATHER, 1 << 20);
    sc_bank_close(a);
    while ((int)w->tick < SERVE_TICK) {
        int trout = level(a, SKILL_FISHING) >= 15 && level(a, SKILL_COOKING) >= 15;
        if (trout && sc_count(a, FEATHER) < 26 && coins(a) >= 26) {
            go_lm(a, "general_store");
            (void)buy(a, SHOPKEEPER, FEATHER, coins(a) < 100 ? coins(a) : 100);
            sc_shop_close(a);
        }
        trout &= sc_count(a, FEATHER) > 0;
        int free_slots = sc_free_slots(a);
        if (trout) {
            (void)go(a, 158, 224);  // the river
            (void)sc_fish(a, ROD_SPOT, "Lure", RAW_TROUT, free_slots, 30);
        } else {
            go_lm(a, "cows");
            (void)sc_fish(a, NET_SPOT, "Small Net", RAW_SHRIMPS, free_slots, 60);
        }
        go_lm(a, "range");
        for (int round = 0; round < 2; round++) {  // a Cook click cooks one kind of food
            if (!sc_count(a, RAW_SHRIMPS) && !sc_count(a, RAW_TROUT)) break;
            if (!use_station(a, RANGE, "Cook")) break;
            for (int k = 0; k < 120 && sc_player(a)->skill_job.kind; k++) sc_wait(a, 1);
        }
        (void)sc_drop_all(a, BURNT_SHRIMP);
        (void)sc_drop_all(a, BURNT_FISH);
        int cooked = sc_count(a, SHRIMPS) + sc_count(a, TROUT);
        go_lm(a, "bank");
        if (sc_bank_open(a)) {
            (void)sc_deposit(a, SHRIMPS, 28);
            (void)sc_deposit(a, TROUT, 28);
            sc_bank_close(a);
            t->food_made += cooked;
        }
    }
    t->food_ticks = (int)w->tick;
    sc_note(a, "cooked %d food; fishing %d cooking %d", t->food_made, level(a, SKILL_FISHING),
            level(a, SKILL_COOKING));
    int item = sc_bank_open(a) && sc_bank_count(a, TROUT) >= 20 ? TROUT : SHRIMPS;
    (void)sc_withdraw(a, item, 26);
    sc_bank_close(a);
    sc_note(a, "serving %d %s, %d free slots", sc_count(a, item), item == TROUT ? "trout" : "shrimps", sc_free_slots(a));
    serve(a, t, item, 20, 0);
}

// The runecrafter: pure essence with the kit's pickaxe, bound into mind runes at the mind altar,
// then traded to the mage.
static void crafter_plan(ScAgent *a, void *arg) {
    Team *t = arg;
    RcWorld *w = sc_world(a);
    go_lm(a, "bank");
    if (!sc_bank_open(a)) sc_fail(a, "bank");
    (void)sc_withdraw(a, BRONZE_PICKAXE, 1);
    sc_bank_close(a);
    int ess[] = {ESSENCE_ROCK};
    while ((int)w->tick < SERVE_TICK) {
        go_lm(a, "essence");
        (void)sc_gather(a, ess, 1, "Mine", PURE_ESSENCE, sc_free_slots(a), 20);
        go_lm(a, "mind_altar");
        int before = sc_count(a, MIND_RUNE);
        if (!use_station(a, MIND_ALTAR, "Craft-rune")) sc_fail(a, "mind altar");
        for (int k = 0; k < 10 && sc_count(a, PURE_ESSENCE); k++) sc_wait(a, 1);
        t->runes_made += sc_count(a, MIND_RUNE) - before;
    }
    t->rune_ticks = (int)w->tick;
    sc_note(a, "crafted %d mind runes; mining %d runecraft %d", t->runes_made, level(a, SKILL_MINING),
            level(a, SKILL_RUNECRAFT));
    go_lm(a, "bank");
    serve(a, t, MIND_RUNE, 1 << 20, 1);
}

int run_coop(void) {
    out("## Four-player co-op\n\nA melee fighter and a mage train as in the solo routes; a cook (fishing, "
        "cooking) and a runecrafter (mining, runecrafting) produce for them and sell at the bank "
        "through ordinary trades; the two fighters then enter Scurrius' arena together, so the "
        "encounter scales to two participants. Log: `build/acceptance/coop.log`.\n\n");
    RcWorld *w = new_world(4, 4001);
    memset(skips, 0, sizeof(skips));
    memset(spent_supplies, 0, sizeof(spent_supplies));
    memset(spent_gear, 0, sizeof(spent_gear));
    FILE *log = fopen("build/acceptance/coop.log", "w");
    Team t = {.fighters = {0, 1}, .cook = 2, .crafter = 3};
    Route melee = {.style = 0, .start_tick = (int)w->tick, .food_target = 24, .team = &t};
    Route magic = {.style = 2, .start_tick = (int)w->tick, .food_target = 20, .team = &t};
    ScRun run;
    sc_init(&run, w, 4, log);
    sc_start(&run, 0, route_plan, &melee);
    sc_start(&run, 1, route_plan, &magic);
    sc_start(&run, 2, cook_plan, &t);
    sc_start(&run, 3, crafter_plan, &t);
    sc_run(&run, w->tick + END_TICK);
    int ok = t.kill_tick > 0 && t.trades >= 1 && t.participants >= 2;
    out("| Team kill | Kill tick | Fight participants | Boss max HP | Supplier trades | All trades | "
        "Food cooked | Mind runes crafted | Deaths (melee/mage/cook/crafter) | State hash |\n"
        "|---|---|---|---|---|---|---|---|---|---|\n");
    out("| %s | %d | %d | %d | %d | %u | %d | %d | %d/%d/%d/%d | %016llx |\n\n", t.kill_tick ? "yes" : "no",
        t.kill_tick, t.participants, t.boss_max_hp, t.trades, rc_trade_completed_count(w), t.food_made,
        t.runes_made, run.agents[0].deaths, run.agents[1].deaths, run.agents[2].deaths, run.agents[3].deaths,
        (unsigned long long)rc_world_state_hash(w));
    out("Production from level 1 (travel and banking included): the cook made %d food in %d ticks (%.0f per "
        "1,000), shrimps until Fishing and Cooking 15, trout after; the runecrafter made %d mind runes in %d "
        "ticks (%.0f per 1,000).\n\n", t.food_made, t.food_ticks, t.food_made * 1000.0 / (t.food_ticks ? t.food_ticks : 1),
        t.runes_made, t.rune_ticks, t.runes_made * 1000.0 / (t.rune_ticks ? t.rune_ticks : 1));
    for (int i = 0; i < 4; i++)
        if (run.agents[i].failed) out("Player %d's plan failed: %s\n\n", i, run.agents[i].failure);
    sc_free(&run);
    if (log) fclose(log);
    rc_world_destroy(w);
    return ok;
}

// ---- Eight-player contention ----------------------------------------------------------------------------

enum { COPPER_ORE = 436, TIN_ORE = 438, CONTEND_TICKS = 1500 };

// Every copy of `item` in the world: inventories, equipment, banks, trade offers and the ground.
static int64_t item_total(const RcWorld *w, int item) {
    int64_t n = 0;
    for (int p = 0; p < w->player_count; p++) {
        const RcPlayer *pl = &w->players[p];
        for (int i = 0; i < RC_INVENTORY_SIZE; i++) n += pl->inventory[i].item_id == item ? pl->inventory[i].quantity : 0;
        for (int i = 0; i < RC_EQUIP_COUNT; i++) n += pl->equipment[i].item_id == item ? pl->equipment[i].quantity : 0;
        for (int i = 0; i < RC_BANK_SIZE; i++) n += pl->bank[i].item_id == item ? pl->bank[i].quantity : 0;
        for (int i = 0; i < pl->trade.offer_count; i++) n += pl->trade.offer[i].item_id == item ? pl->trade.offer[i].quantity : 0;
    }
    for (int i = 0; i < w->ground_item_count; i++)
        if (w->ground_items[i].active && w->ground_items[i].item_id == item) n += w->ground_items[i].quantity;
    return n;
}

static struct {
    int ore[RC_MAX_PLAYERS], bought[RC_MAX_PLAYERS], min_stock, stock_cap;
    int dropped, race_tries, race_winners;
    int seen_participants, bad_scaling, best_max_hp, joined_late_ok;
    int64_t coins_before, coins_paid, scims_before, scims_bought;
} con;

static int scim_slot(const RcWorld *w) {
    for (int i = 0; i < w->shops[0].slot_count; i++)
        if (w->shops[0].slots[i].item_id == BRONZE_SCIM) return i;
    return -1;
}

// Everyone ends in Scurrius' arena; whoever is alive checks each join's scaling.
static void contend_boss(ScAgent *a) {
    RcWorld *w = sc_world(a);
    const RcModeBoss *b = &rules->boss;
    (void)go(a, b->rect[0] + 1 + a->index % 4, b->centre[1] - 2 + a->index / 4);
    for (int t = 0; t < 120 && !sc_player(a)->is_dead; t++) {
        int n = __builtin_popcount(w->mode_boss.participants);
        if (n > con.seen_participants) {
            int want = b->hp * (100 + b->group_scale_pct * (n - 1)) / 100;
            if (w->mode_boss.max_hp != want) con.bad_scaling++;
            con.seen_participants = n;
            if (w->mode_boss.max_hp > con.best_max_hp) con.best_max_hp = w->mode_boss.max_hp;
        }
        sc_wait(a, 1);
    }
}

static void miner_plan(ScAgent *a, void *arg) {
    (void)arg;
    RcWorld *w = sc_world(a);
    go_lm(a, "bank");
    if (!sc_bank_open(a)) sc_fail(a, "bank");
    (void)sc_withdraw(a, BRONZE_PICKAXE, 1);
    sc_bank_close(a);
    go_lm(a, "quarry");
    int copper[] = {10943, 11161}, tin[] = {11360, 11361};
    for (int k = 0; (int)w->tick < CONTEND_TICKS; k++) {
        int ore = k % 2 ? TIN_ORE : COPPER_ORE, before = sc_count(a, ore);
        if (!sc_gather(a, ore == TIN_ORE ? tin : copper, 2, "Mine", ore, before + 1, 20)) sc_wait(a, 2);
        con.ore[a->index] += sc_count(a, ore) - before;
        if (!sc_free_slots(a)) {
            (void)sc_drop_all(a, COPPER_ORE);
            (void)sc_drop_all(a, TIN_ORE);
        }
    }
    contend_boss(a);
}

static void shopper_plan(ScAgent *a, void *arg) {
    (void)arg;
    RcWorld *w = sc_world(a);
    go_lm(a, "bank");
    if (!sc_bank_open(a)) sc_fail(a, "bank");
    (void)sc_withdraw(a, COINS, 1 << 20);
    sc_bank_close(a);
    go_lm(a, "general_store");
    if (!sc_shop_open(a, SHOPKEEPER)) sc_fail(a, "shop");
    // Two scimitars each from a stock of four, all four shoppers at once.
    for (int t = 0; t < 300 && con.bought[a->index] < 2; t++) {
        int got = sc_buy(a, BRONZE_SCIM, 1);
        if (got > 0) con.bought[a->index] += got;
        int k = scim_slot(w);
        if (k >= 0 && w->shops[0].slots[k].stock < con.min_stock) con.min_stock = w->shops[0].slots[k].stock;
        if (got <= 0) sc_wait(a, 10);
    }
    sc_shop_close(a);
    // A ground race: the first shopper drops a scimitar; the others reach for it once it shows.
    if (a->index == 4 && sc_count(a, BRONZE_SCIM)) {
        sc_wait(a, 5);
        con.scims_before = item_total(w, BRONZE_SCIM);
        (void)rc_player_drop_item(w, sc_slot(a, BRONZE_SCIM));
        sc_wait(a, 1);
        con.dropped = (int)w->tick;
    } else if (a->index > 4) {
        while (!con.dropped && (int)w->tick < CONTEND_TICKS) sc_wait(a, 1);
        int before = sc_count(a, BRONZE_SCIM), item[] = {BRONZE_SCIM};
        con.race_tries++;
        (void)sc_pickup(a, item, 1, 6);
        con.race_winners += sc_count(a, BRONZE_SCIM) > before;
    }
    while ((int)w->tick < CONTEND_TICKS) sc_wait(a, 5);
    contend_boss(a);
}

int run_contention(void) {
    out("## Eight-player contention\n\nEight fresh characters at once: four mine the same copper and tin "
        "rocks; four buy from one bronze-scimitar stock of four; one drops a scimitar and three "
        "reach for it; then all eight enter Scurrius' arena one after another. Log: "
        "`build/acceptance/contention.log`.\n\n");
    RcWorld *w = new_world(8, 8001);
    memset(&con, 0, sizeof(con));
    int k = scim_slot(w);
    con.min_stock = con.stock_cap = k >= 0 ? w->shops[0].slots[k].stock : 0;
    con.coins_before = item_total(w, COINS);
    FILE *log = fopen("build/acceptance/contention.log", "w");
    ScRun run;
    sc_init(&run, w, 8, log);
    for (int i = 0; i < 8; i++) sc_start(&run, i, i < 4 ? miner_plan : shopper_plan, NULL);
    int64_t scims_start = item_total(w, BRONZE_SCIM);
    sc_run(&run, w->tick + CONTEND_TICKS + 400);
    int bought = 0, ore_min = 1 << 30, ore_max = 0;
    for (int i = 0; i < 8; i++) bought += con.bought[i];
    for (int i = 0; i < 4; i++) {
        ore_min = con.ore[i] < ore_min ? con.ore[i] : ore_min;
        ore_max = con.ore[i] > ore_max ? con.ore[i] : ore_max;
    }
    // The shop restocks one scimitar per 200 ticks: purchases are bounded by the cap plus restocks.
    int restocks = CONTEND_TICKS / 200 + 1;
    int64_t scims_now = item_total(w, BRONZE_SCIM);
    int64_t coins_now = item_total(w, COINS);
    int ok_shop = con.min_stock >= 0 && bought <= con.stock_cap + restocks && scims_now == scims_start + bought;
    int ok_coins = coins_now == con.coins_before - (int64_t)bought * price_of(0, BRONZE_SCIM);
    int ok_race = con.race_winners == 1;
    int ok_ore = ore_min > 0 && ore_max <= 3 * ore_min;
    int ok_boss = con.bad_scaling == 0 && con.seen_participants == 8
               && con.best_max_hp == rules->boss.hp * (100 + rules->boss.group_scale_pct * 7) / 100;
    out("| Check | Result | Detail |\n|---|---|---|\n");
    out("| Shared rocks | %s | ore per miner %d/%d/%d/%d (no player under a third of the best) |\n",
        ok_ore ? "pass" : "FAIL", con.ore[0], con.ore[1], con.ore[2], con.ore[3]);
    out("| One shop stock | %s | %d bought by four players (%d/%d/%d/%d) from a stock of %d; lowest stock "
        "seen %d; scimitars in the world rose by exactly the purchases |\n", ok_shop ? "pass" : "FAIL",
        bought, con.bought[4], con.bought[5], con.bought[6], con.bought[7], con.stock_cap, con.min_stock);
    out("| Coins | %s | world coins fell by exactly the purchase prices |\n", ok_coins ? "pass" : "FAIL");
    out("| Ground race | %s | %d players reached for one dropped scimitar; %d got it |\n",
        ok_race ? "pass" : "FAIL", con.race_tries, con.race_winners);
    out("| Boss entry | %s | %d participants; maximum HP %d (240 x (1 + 0.75 x 7)); every join checked |\n\n",
        ok_boss ? "pass" : "FAIL", con.seen_participants, con.best_max_hp);
    for (int i = 0; i < 8; i++)
        if (run.agents[i].failed) out("Player %d's plan failed: %s\n\n", i, run.agents[i].failure);
    sc_free(&run);
    if (log) fclose(log);
    rc_world_destroy(w);
    return ok_shop && ok_coins && ok_race && ok_ore && ok_boss;
}
// ---- Economy and recovery ------------------------------------------------------------------------------

enum {
    DAGGER = 1205, KNIFE = 946, HAMMER = 2347, BRONZE_AXE = 1351, LOGS = 1511, ARROW_SHAFT = 52,
    HEADLESS_ARROW = 53, BRONZE_BAR = 2349, BRONZE_ARROWTIPS = 39, FURNACE = 24009, ANVIL = 39620,
};

static struct {
    int steps, failed;
    char failures[512];
    int death_tick[2], regrant, recovered_tick, kit_items, kit_banked, kit_lost;
    int arrows_made, arrow_ticks;
} eco;

static void eco_check(ScAgent *a, int ok, const char *what) {
    eco.steps++;
    if (ok) return;
    eco.failed++;
    size_t n = strlen(eco.failures);
    snprintf(eco.failures + n, sizeof(eco.failures) - n, "%s%s", n ? "; " : "", what);
    sc_note(a, "economy check failed: %s", what);
}

// Conservation through ordinary actions: every step leaves the world's totals as they were, except
// shop trades, which move exactly the listed price and sale value.
static void conservation_plan(ScAgent *a, void *arg) {
    (void)arg;
    RcWorld *w = sc_world(a);
    int64_t coins0 = item_total(w, COINS), shrimps0 = item_total(w, SHRIMPS), dagger0 = item_total(w, DAGGER);
    go_lm(a, "bank");
    if (!sc_bank_open(a)) sc_fail(a, "bank");
    (void)sc_withdraw(a, COINS, 1 << 20);
    eco_check(a, item_total(w, COINS) == coins0, "withdrawing coins changed the total");
    (void)sc_deposit(a, COINS, 50);
    eco_check(a, item_total(w, COINS) == coins0, "depositing coins changed the total");
    (void)sc_withdraw(a, COINS, 1 << 20);
    sc_bank_close(a);
    (void)rc_player_unequip(w, EQUIP_WEAPON);
    sc_wait(a, 1);
    (void)sc_equip(a, DAGGER);
    eco_check(a, item_total(w, DAGGER) == dagger0 && equipped(a, DAGGER), "unequip and equip changed the dagger");
    (void)rc_player_drop_item(w, sc_slot(a, SHRIMPS));
    sc_wait(a, 1);
    eco_check(a, item_total(w, SHRIMPS) == shrimps0, "dropping changed the shrimps");
    int item[] = {SHRIMPS};
    (void)sc_pickup(a, item, 1, 2);
    eco_check(a, item_total(w, SHRIMPS) == shrimps0 && sc_count(a, SHRIMPS) == 4, "picking up changed the shrimps");
    go_lm(a, "general_store");
    int64_t scims0 = item_total(w, BRONZE_SCIM);
    (void)buy(a, SHOPKEEPER, BRONZE_SCIM, 1);
    eco_check(a, item_total(w, COINS) == coins0 - price_of(0, BRONZE_SCIM) && item_total(w, BRONZE_SCIM) == scims0 + 1,
              "buying moved other than the price");
    (void)sc_sell(a, BRONZE_SCIM, 1);
    sc_wait(a, 1);
    int sale = rc_mode_sale_value(rules, BRONZE_SCIM);
    eco_check(a, item_total(w, COINS) == coins0 - price_of(0, BRONZE_SCIM) + sale && item_total(w, BRONZE_SCIM) == scims0,
              "selling back moved other than the sale value");
    eco_check(a, sale < price_of(0, BRONZE_SCIM), "buying and selling back gained coins");
    sc_shop_close(a);
    // A trade with player 1: 10 coins for 2 shrimps; nothing appears or disappears.
    go_lm(a, "bank");
    int64_t c1 = item_total(w, COINS), s1 = item_total(w, SHRIMPS);
    int ok = trade_with(a, 1, COINS, 10);
    eco_check(a, ok && item_total(w, COINS) == c1 && item_total(w, SHRIMPS) == s1 && sc_count(a, SHRIMPS) == 6,
              "the trade changed totals");
}

// Player 1 waits at the bank until player 0 asks to trade.
static void partner_plan(ScAgent *a, void *arg) {
    (void)arg;
    go_lm(a, "bank");
    for (int t = 0; t < 1200 && sc_world(a)->players[0].trade.stage == RC_TRADE_STAGE_NONE; t++) sc_wait(a, 1);
    (void)trade_with(a, 0, SHRIMPS, 2);
}

static const int kit_items[] = {BRONZE_AXE, BRONZE_PICKAXE, HAMMER, KNIFE, SMALL_NET, FLY_ROD, 301, 1733,
                                SHORTBOW, STAFF_OF_AIR, DAGGER};

static int kit_count(const RcPlayer *p, int where) {  // 0 inventory and equipment, 1 bank
    int n = 0;
    for (int k = 0; k < (int)(sizeof(kit_items) / sizeof(kit_items[0])); k++) {
        if (where) for (int i = 0; i < RC_BANK_SIZE; i++) n += p->bank[i].item_id == kit_items[k];
        else {
            for (int i = 0; i < RC_INVENTORY_SIZE; i++) n += p->inventory[i].item_id == kit_items[k];
            for (int i = 0; i < RC_EQUIP_COUNT; i++) n += p->equipment[i].item_id == kit_items[k];
        }
    }
    return n;
}

static int coins_owned(const RcPlayer *p) {
    int n = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) n += p->inventory[i].item_id == COINS ? p->inventory[i].quantity : 0;
    for (int i = 0; i < RC_BANK_SIZE; i++) n += p->bank[i].item_id == COINS ? p->bank[i].quantity : 0;
    return n;
}

// Death with nothing left: everything carried, no coins anywhere, the grave deep in the cave's
// aggressive monsters. The character must earn its way back with fists, get a weapon, die again,
// and find both graves in its bank when they expire.
static void recovery_plan(ScAgent *a, void *arg) {
    Route *r = arg;
    RcWorld *w = sc_world(a);
    go_lm(a, "bank");
    if (!sc_bank_open(a)) sc_fail(a, "bank");
    for (int i = 0; i < RC_BANK_SIZE; i++) {
        int id = sc_player(a)->bank[i].item_id;
        if (id >= 0) (void)sc_withdraw(a, id, 1 << 20);
    }
    sc_bank_close(a);
    go_lm(a, "general_store");
    (void)buy(a, SHOPKEEPER, FEATHER, coins(a));  // spend every coin
    sc_shop_close(a);
    eco.kit_items = kit_count(sc_player(a), 0) + kit_count(sc_player(a), 1);
    eco_check(a, coins_owned(sc_player(a)) == 0, "coins left before the first death");
    for (int round = 0; round < 2; round++) {
        go_lm(a, "dark_wizards");
        for (int t = 0; t < 1500 && !sc_player(a)->is_dead; t++) sc_wait(a, 1);
        eco_check(a, sc_player(a)->is_dead, "never died among the dark wizards");
        eco.death_tick[round] = (int)w->tick;
        while (sc_player(a)->is_dead) sc_wait(a, 1);
        if (round == 1) break;
        const RcPlayer *p = sc_player(a);
        eco.regrant = kit_count(p, 0) + kit_count(p, 1) + coins_owned(p);
        eco_check(a, eco.regrant == 0, "the starter kit or coins came back on respawn");
        // Back from nothing: fists on cows for beef, then goblins for coins, then a scimitar.
        beef_run(a, r, 6);
        while (coins(a) < price_of(0, BRONZE_SCIM) && (int)w->tick < eco.death_tick[0] + 3000) {
            const RcGymLandmark *g = lm("goblins");
            if (region(sc_player(a)->x, sc_player(a)->y) != SEWERS) (void)go(a, g->x, g->y);
            if (fight_one(a, r, GOBLIN, 16, sc_player(a)->max_hp / 3) == 2) beef_run(a, r, 6);
        }
        go_lm(a, "general_store");
        int got = buy(a, SHOPKEEPER, BRONZE_SCIM, 1);
        sc_shop_close(a);
        (void)sc_equip(a, BRONZE_SCIM);
        eco_check(a, got == 1 && equipped(a, BRONZE_SCIM), "could not earn a scimitar back");
        eco.recovered_tick = (int)w->tick;
    }
    // Both graves expire into the bank, the older one first.
    int until = eco.death_tick[1] + rules->recovery_ticks + 20;
    go_lm(a, "bank");
    while ((int)w->tick < until) sc_wait(a, 10);
    eco.kit_banked = kit_count(sc_player(a), 1) + kit_count(sc_player(a), 0);
    int scim_banked = 0;
    for (int i = 0; i < RC_BANK_SIZE; i++) scim_banked += sc_player(a)->bank[i].item_id == BRONZE_SCIM;
    eco.kit_lost = eco.kit_items - eco.kit_banked;
    eco_check(a, eco.kit_lost == 0, "kit items from the first grave are missing");
    eco_check(a, scim_banked == 1, "the scimitar from the second grave is missing");
    int left = 0;
    for (int i = 0; i < w->ground_item_count; i++) left += w->ground_items[i].active && w->ground_items[i].owner_uid == a->index && w->ground_items[i].bank_on_expiry;
    eco_check(a, left == 0, "recovery items still on the ground after expiry");
}

static int station_use(ScAgent *a, int id, const char *option) {
    int ids[] = {id}, x, y;
    return sc_find_loc(a, ids, 1, 24, &x, &y, NULL) && sc_use_loc(a, id, x, y, option);
}

// Bronze arrows from nothing but the kit: logs into shafts, feathers bought, copper and tin into
// bars and arrowtips.
static void arrow_plan(ScAgent *a, void *arg) {
    (void)arg;
    RcWorld *w = sc_world(a);
    int start = (int)w->tick;
    go_lm(a, "bank");
    if (!sc_bank_open(a)) sc_fail(a, "bank");
    int tools[] = {BRONZE_AXE, BRONZE_PICKAXE, HAMMER, KNIFE};
    for (int i = 0; i < 4; i++) (void)sc_withdraw(a, tools[i], 1);
    (void)sc_withdraw(a, FEATHER, 1 << 20);
    (void)sc_withdraw(a, COINS, 1 << 20);
    sc_bank_close(a);
    int trees[] = {1276, 1278}, copper[] = {10943, 11161}, tin[] = {11360, 11361};
    while ((int)w->tick < start + 4000) {
        go_lm(a, "trees");
        (void)sc_gather(a, trees, 2, "Chop down", LOGS, 4, 20);
        while (sc_count(a, LOGS) && sc_use_items(a, KNIFE, LOGS) && sc_make(a, ARROW_SHAFT, 0) > 0) {}
        if (sc_count(a, FEATHER) < sc_count(a, ARROW_SHAFT) && coins(a)) {
            go_lm(a, "general_store");
            (void)buy(a, SHOPKEEPER, FEATHER, sc_count(a, ARROW_SHAFT) - sc_count(a, FEATHER));
            sc_shop_close(a);
        }
        if (sc_count(a, ARROW_SHAFT) && sc_count(a, FEATHER))
            while (sc_use_items(a, ARROW_SHAFT, FEATHER) && sc_make(a, HEADLESS_ARROW, 0) > 0 && sc_count(a, ARROW_SHAFT)) {}
        go_lm(a, "quarry");
        int bars = sc_count(a, HEADLESS_ARROW) / 15 + 1;
        (void)sc_gather(a, copper, 2, "Mine", COPPER_ORE, bars, 20);
        (void)sc_gather(a, tin, 2, "Mine", TIN_ORE, bars, 20);
        go_lm(a, "smithy");
        if (station_use(a, FURNACE, "Smelt")) (void)sc_make(a, BRONZE_BAR, 0);
        while (sc_count(a, BRONZE_BAR) && station_use(a, ANVIL, "Smith") && sc_make(a, BRONZE_ARROWTIPS, 0) > 0) {}
        int before = sc_count(a, BRONZE_ARROW);
        while (sc_count(a, HEADLESS_ARROW) && sc_count(a, BRONZE_ARROWTIPS)
               && sc_use_items(a, HEADLESS_ARROW, BRONZE_ARROWTIPS) && sc_make(a, BRONZE_ARROW, 0) > 0) {}
        eco.arrows_made += sc_count(a, BRONZE_ARROW) - before;
        if (sc_count(a, BRONZE_ARROW) == before && !sc_count(a, HEADLESS_ARROW)) sc_wait(a, 5);
    }
    eco.arrow_ticks = (int)w->tick - start;
    sc_note(a, "made %d bronze arrows in %d ticks; fletching %d smithing %d", eco.arrows_made, eco.arrow_ticks,
            level(a, SKILL_FLETCHING), level(a, SKILL_SMITHING));
}

int run_economy(void) {
    memset(&eco, 0, sizeof(eco));
    out("## Economy and recovery\n\n");
    // Zero-work loops in the rules: nothing sells for what it costs, and doses split evenly.
    int rows = 0, gains = 0;
    char gain_list[256] = "";
    for (int sh = 0; sh < rules->shop_count; sh++)
        for (int k = 0; k < rules->shops[sh].stock_count; k++) {
            int item = rules->shops[sh].stock[k].item, price = rules->shops[sh].stock[k].price;
            int sale = rc_mode_sale_value(rules, item);
            rows++;
            if (sale >= price) {
                gains++;
                size_t n = strlen(gain_list);
                snprintf(gain_list + n, sizeof(gain_list) - n, " %d", item);
            }
        }
    static const int potion[] = {2434, 139, 141, 143};  // prayer potion (4), (3), (2), (1)
    int doses_ok = 1, full = rc_mode_sale_value(rules, potion[0]);
    for (int d = 1; d < 4; d++) doses_ok &= rc_mode_sale_value(rules, potion[d]) * 4 <= (4 - d) * full;
    out("| Check | Result | Detail |\n|---|---|---|\n");
    out("| Buy and sell back | %s | %d shop rows; %d sell for at least their price%s |\n", gains ? "FAIL" : "pass",
        rows, gains, gain_list);
    out("| Dose splitting | %s | prayer potion (3)/(2)/(1) sell for at most their share of a (4) (%d) |\n",
        doses_ok ? "pass" : "FAIL", full);

    // Conservation through ordinary actions (two players).
    RcWorld *w = new_world(2, 6001);
    memset(skips, 0, sizeof(skips));
    FILE *log = fopen("build/acceptance/economy.log", "w");
    ScRun run;
    sc_init(&run, w, 2, log);
    sc_start(&run, 0, conservation_plan, NULL);
    sc_start(&run, 1, partner_plan, NULL);
    sc_run(&run, w->tick + 1500);
    int conservation_fail = eco.failed;
    out("| Conservation | %s | bank, equip, drop and pick up, shop buy and sell, a trade: %d checks%s%s |\n",
        eco.failed ? "FAIL" : "pass", eco.steps, eco.failed ? ": " : "", eco.failures);
    sc_free(&run);
    rc_world_destroy(w);

    // Death with nothing, twice.
    eco.failures[0] = 0;
    w = new_world(1, 6002);
    memset(skips, 0, sizeof(skips));
    Route r = {.style = 0, .start_tick = (int)w->tick};
    sc_init(&run, w, 1, log);
    sc_start(&run, 0, recovery_plan, &r);
    sc_run(&run, w->tick + 6000);
    int recovery_fail = eco.failed - conservation_fail || run.agents[0].failed;
    out("| Death from zero | %s | all %d kit items and every coin carried or spent; died at tick %d; respawned "
        "with nothing and no starter grant; a scimitar earned back by tick %d; died again at tick %d; "
        "both graves expired into the bank: %d of %d kit items and the scimitar, none left on the ground%s%s |\n",
        recovery_fail ? "FAIL" : "pass", eco.kit_items, eco.death_tick[0], eco.recovered_tick, eco.death_tick[1],
        eco.kit_banked, eco.kit_items, recovery_fail ? ": " : "",
        run.agents[0].failed ? run.agents[0].failure : eco.failures);
    out("| Full bank at expiry | covered by test | tests/test_compact_boss.c fills the bank (test setup); the "
        "expiring items stay private on the ground and bank once a slot frees |\n");
    sc_free(&run);
    rc_world_destroy(w);

    // Arrow production from the kit.
    w = new_world(1, 6003);
    sc_init(&run, w, 1, log);
    sc_start(&run, 0, arrow_plan, NULL);
    sc_run(&run, w->tick + 4200);
    out("| Arrow production | %s | %d bronze arrows from logs, bought feathers and smithed tips in %d ticks "
        "from a fresh character |\n\n", eco.arrows_made > 0 ? "pass" : "FAIL", eco.arrows_made, eco.arrow_ticks);
    sc_free(&run);
    rc_world_destroy(w);
    if (log) fclose(log);
    return !eco.failed && !recovery_fail && eco.arrows_made > 0;
}
