// What a compact-v2 agent sees and can do (rc-env/compact_env.h; layouts in rc-env/README.md).
// Everything here reads only what the agent's player could see: its own state, the 25x25 window
// around it, and the interface it has open. Entity lists are packed in a fixed order (the attack
// target first, then nearest first, then by id) and truncated, so overflow is deterministic; the
// actions index into the lists of the agent's last observation.
#include "compact_internal.h"

#include "../rc-core/api.h"
#include "../rc-core/consumables.h"
#include "../rc-core/npc.h"
#include "../rc-core/object_runtime.h"
#include "../rc-core/objects.h"
#include "../rc-core/pathfinding.h"
#include "../rc-core/players.h"
#include "../rc-core/prayer.h"
#include "../rc-core/shops.h"
#include "../rc-core/skilling.h"
#include "../rc-core/skills.h"
#include "../rc-core/spells.h"
#include "../rc-core/storage.h"
#include "../rc-core/trade.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// rc-env/travel.c
int rc_env_travel_distance(int landmark, int x, int y, int plane);
int rc_env_travel_waypoint(const RcWorldMap *map, int landmark, int x, int y, int plane,
                           int max_steps, int *wx, int *wy);

enum {
    CAT_FOOD = 0, CAT_POTION, CAT_MELEE, CAT_RANGED, CAT_MAGIC, CAT_ARMOUR, CAT_AMMO, CAT_RUNE,
    CAT_TOOL, CAT_ORE_BAR, CAT_FLETCHING, CAT_MATERIAL, CAT_BONES, CAT_OTHER,
};
enum { COINS = 995 };

static int max_i(int a, int b) { return a > b ? a : b; }
static int dist(int ax, int ay, int bx, int by) { return max_i(abs(ax - bx), abs(ay - by)); }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// ---- Tables ----------------------------------------------------------------------------------------

static void add_vocab(RcCompactEnv *env, int item) {
    if (item < 0 || item >= RC_MAX_ITEM_DEFS || env->vocab[item]) return;
    env->vocab[item] = (int16_t)++env->vocab_size;
}

static int has_word(const char *name, const char *word) {
    return name && strstr(name, word) != NULL;
}

static uint8_t classify(const RcCompactEnv *env, int item) {
    const RcModeRules *m = env->rules;
    const RcItemDef *d = rc_item_def_get(item);
    if (!d) return CAT_OTHER;
    if (item == COINS) return CAT_OTHER;
    for (int i = 0; i < m->axe_count; i++) if ((int)m->axes[i].item_id == item) return CAT_TOOL;
    for (int i = 0; i < m->pickaxe_count; i++) if ((int)m->pickaxes[i].item_id == item) return CAT_TOOL;
    for (int i = 0; i < m->fish_count; i++)
        if ((int)m->fish[i].tool == item) return CAT_TOOL;
    for (int i = 0; i < m->recipe_count; i++)
        for (int k = 0; k < m->recipes[i].recipe.tool_count; k++)
            if ((int)m->recipes[i].recipe.tools[k] == item) return CAT_TOOL;
    for (int i = 0; i < m->bury_count; i++) if (m->bury[i].item == item) return CAT_BONES;
    if (has_word(d->name, "potion")) return CAT_POTION;
    for (int op = 0; op < RC_ITEM_ACTION_COUNT; op++)
        if (rc_consumable_action(item, op) == 1) return CAT_FOOD;
    if (d->equippable && d->equip_slot == EQUIP_AMMO) return CAT_AMMO;
    if (d->equippable && d->equip_slot == EQUIP_WEAPON)
        return has_word(d->name, "bow") ? CAT_RANGED : has_word(d->name, "staff") ? CAT_MAGIC : CAT_MELEE;
    if (d->equippable) return CAT_ARMOUR;
    if (has_word(d->name, " rune")) return CAT_RUNE;
    if (has_word(d->name, " ore") || has_word(d->name, " bar") || !strcmp(d->name, "Coal"))
        return CAT_ORE_BAR;
    if (has_word(d->name, "logs") || has_word(d->name, "shaft") || has_word(d->name, "Headless")
            || has_word(d->name, "(u)") || has_word(d->name, "arrowtips"))
        return CAT_FLETCHING;
    if (has_word(d->name, "hide") || has_word(d->name, "eather") || has_word(d->name, "Flax")
            || has_word(d->name, "string") || has_word(d->name, "studs") || has_word(d->name, "Thread")
            || has_word(d->name, "Feather") || has_word(d->name, "essence"))
        return CAT_MATERIAL;
    return CAT_OTHER;
}

void rc_cenv_build_tables(RcCompactEnv *env) {
    const RcModeRules *m = env->rules;
    env->vocab = calloc(RC_MAX_ITEM_DEFS, sizeof(*env->vocab));
    env->category = calloc(RC_MAX_ITEM_DEFS, 1);
    env->tier = calloc(RC_MAX_ITEM_DEFS, 1);
    if (!env->vocab || !env->category || !env->tier) return;
    // Every item the mode can put in the world, in a fixed order.
    for (int k = 0; k < RC_MODE_START_KINDS; k++)
        for (int i = 0; i < m->start_count[k]; i++) add_vocab(env, m->start[k][i].item);
    for (int s = 0; s < m->shop_count; s++)
        for (int i = 0; i < m->shops[s].stock_count; i++) add_vocab(env, m->shops[s].stock[i].item);
    for (int i = 0; i < m->recipe_count; i++) {
        const RcRecipe *r = &m->recipes[i].recipe;
        add_vocab(env, (int)r->output_item);
        for (int k = 0; k < r->input_count; k++) add_vocab(env, (int)r->inputs[k].item_id);
        for (int k = 0; k < r->tool_count; k++) add_vocab(env, (int)r->tools[k]);
    }
    for (int i = 0; i < m->gather_count; i++) add_vocab(env, (int)m->gather[i].product);
    for (int i = 0; i < m->fish_count; i++)
        for (int c = 0; c < m->fish[i].catch_count; c++) add_vocab(env, (int)m->fish[i].catches[c].product);
    for (int i = 0; i < m->cook_count; i++) {
        add_vocab(env, (int)m->cook[i].cooked);
        add_vocab(env, (int)m->cook[i].burnt);
    }
    for (int d = 0; d < m->drop_count; d++) {
        for (int i = 0; i < m->drops[d].always_count; i++) add_vocab(env, m->drops[d].always[i].item);
        for (int r = 0; r < m->drops[d].roll_count; r++)
            for (int i = 0; i < m->drops[d].rolls[r].count; i++) add_vocab(env, m->drops[d].rolls[r].pick[i].item);
    }
    for (int i = 0; i < m->sale_count; i++) add_vocab(env, m->sale[i].item);
    for (int item = 0; item < RC_MAX_ITEM_DEFS; item++) {
        if (!env->vocab[item]) continue;
        env->category[item] = classify(env, item);
        const RcItemDef *d = rc_item_def_get(item);
        int top = 0;
        for (int r = 0; d && r < d->req_count; r++) top = max_i(top, d->req_level[r]);
        env->tier[item] = (uint8_t)top;
    }
    // Item pairs that open a menu or charge something, from the mode's recipes.
    static const struct { uint8_t menu; int tool; } tool_menus[] = {
        {RC_MAKE_MENU_KNIFE, 946}, {RC_MAKE_MENU_NEEDLE, 1733}, {RC_MAKE_MENU_CHISEL, 1755},
    };
    env->combo_count = 0;
    for (int i = 0; i < m->recipe_count && env->combo_count < RC_CENV_COMBOS; i++) {
        const RcModeRecipe *x = &m->recipes[i];
        int a = -1, b = -1;
        for (int t = 0; t < 3; t++)
            if (tool_menus[t].menu == x->station && x->recipe.input_count)
                a = tool_menus[t].tool, b = (int)x->recipe.inputs[0].item_id;
        if (x->station == RC_MAKE_MENU_COMBINE && x->recipe.input_count >= 2)
            a = (int)x->recipe.inputs[0].item_id, b = (int)x->recipe.inputs[1].item_id;
        if (a < 0) continue;
        int dup = 0;
        for (int k = 0; k < env->combo_count; k++)
            dup |= env->combos[k].a == a && env->combos[k].b == b;
        if (!dup) env->combos[env->combo_count++] = (typeof(env->combos[0])){a, b};
    }
    for (int i = 0; i < m->bone_count && env->combo_count < RC_CENV_COMBOS; i++)
        if (m->bone[i].charge_item)
            env->combos[env->combo_count++] = (typeof(env->combos[0])){m->bone[i].charge_item, m->bone[i].item};
    // The enabled prayers and spells, in definition order.
    int n = 0;
    for (int id = 0; id < RC_MAX_PRAYER_DEFS && n < RC_CENV_PRAYERS; id++) {
        const RcPrayerDef *d = rc_prayer_def_get(id);
        if (d && d->loaded && d->level <= m->level_cap) env->prayer_ids[n++] = id;
    }
    for (; n < RC_CENV_PRAYERS; n++) env->prayer_ids[n] = -1;
    n = 0;
    for (int id = 0; id < RC_MAX_SPELL_DEFS && n < RC_CENV_SPELLS; id++) {
        const RcSpellDef *d = rc_spell_def_get(id);
        if (d && d->loaded && d->level <= m->level_cap) env->spell_ids[n++] = id;
    }
    for (; n < RC_CENV_SPELLS; n++) env->spell_ids[n] = -1;
}

// Every loc in the mode's area with an option that works there, found once: placements are
// static, so an observation only filters this list by window and checks for depletion.
static void index_locs(RcCompactEnv *env) {
    RcWorld *w = env->world;
    const RcGymLayout *l = &env->layout;
    int cap = 1024;
    env->locs = malloc((size_t)cap * sizeof(*env->locs));
    env->loc_count = 0;
    for (int x = l->min_rx * 64; env->locs && x < (l->max_rx + 1) * 64; x++) {
        for (int y = l->min_ry * 64; y < (l->max_ry + 1) * 64; y++) {
            RcObjectPlacement rows[6];
            int count = rc_object_placements_at(x, y, 0, rows, 6);
            for (int r = 0; r < count; r++) {
                RcCenvLoc loc = {.id = (int)rows[r].obj_id, .x = x, .y = y, .plane = 0,
                                 .ops = {-1, -1, -1}};
                int k = 0;
                for (int op = 0; op < RC_OBJECT_ACTIONS && k < 3; op++)
                    if (rc_world_object_option_supported(w, loc.id, x, y, 0, 0, op)) loc.ops[k++] = (int8_t)op;
                if (!k) continue;
                if (env->loc_count == cap) {
                    RcCenvLoc *grown = realloc(env->locs, (size_t)(cap *= 2) * sizeof(*env->locs));
                    if (!grown) return;
                    env->locs = grown;
                }
                env->locs[env->loc_count++] = loc;
            }
        }
    }
}

void rc_cenv_index_locs(RcCompactEnv *env) {
    if (!env->locs) index_locs(env);
}

void rc_cenv_free_tables(RcCompactEnv *env) {
    free(env->locs);
    env->locs = NULL;
    free(env->vocab);
    free(env->category);
    free(env->tier);
    env->vocab = NULL;
    env->category = env->tier = NULL;
}

int rc_cenv_in_arena(const RcCompactEnv *env, const RcPlayer *p) {
    const RcModeRules *m = env->rules;
    return m->has_boss && p->plane == 0 && p->x >= m->boss.rect[0] && p->x <= m->boss.rect[2]
        && p->y >= m->boss.rect[1] && p->y <= m->boss.rect[3];
}

// ---- Encoding helpers ------------------------------------------------------------------------------

static void encode_item(const RcCompactEnv *env, float *o, int item, int quantity, uint32_t state) {
    memset(o, 0, RC_CENV_ITEM * sizeof(float));
    if (item < 0 || item >= RC_MAX_ITEM_DEFS || quantity <= 0) return;
    o[0] = 1;
    o[1 + env->category[item]] = 1;
    o[15] = env->tier[item] / 40.0f;
    o[16] = log2f((float)quantity + 1) / 16.0f;
    int value = rc_mode_sale_value(env->rules, item);
    o[17] = clamp01((value > 0 ? (float)value : 0) / 1000.0f);
    o[18] = clamp01(state / 1000.0f);
    o[19] = env->vocab_size ? env->vocab[item] / (float)env->vocab_size : 0;
}

static float vocab_of(const RcCompactEnv *env, int item) {
    return item >= 0 && item < RC_MAX_ITEM_DEFS && env->vocab_size
        ? env->vocab[item] / (float)env->vocab_size : 0;
}

static int npc_family(const RcCompactEnv *env, int def_id) {
    static const int families[] = {1173, 2790, 2856, 3029, 70, 510, 2098};
    for (int i = 0; i < 7; i++) if (families[i] == def_id) return i;
    const RcModeRules *m = env->rules;
    if (m->has_boss && def_id == m->boss.npc) return 7;
    if (m->has_boss && def_id == m->boss.minion) return 8;
    for (int i = 0; i < m->fish_count; i++) if ((int)m->fish[i].npc_id == def_id) return 10;
    return 9;  // shopkeepers and services
}

enum {
    LOC_TREE, LOC_OAK, LOC_WILLOW, LOC_COPPER, LOC_TIN, LOC_IRON, LOC_COAL, LOC_MITHRIL, LOC_ESSENCE,
    LOC_FLAX, LOC_FURNACE, LOC_ANVIL, LOC_RANGE, LOC_WHEEL, LOC_AIR, LOC_WATER, LOC_MIND, LOC_CHAOS,
    LOC_DEATH, LOC_BANK, LOC_PRAYER, LOC_LINK, LOC_FOOD_PILE, LOC_OTHER, LOC_KINDS,
};

static int loc_kind(const RcCompactEnv *env, int id) {
    const RcModeRules *m = env->rules;
    for (int i = 0; i < m->gather_count; i++) {
        if ((int)m->gather[i].obj_id != id) continue;
        switch (m->gather[i].product) {
        case 1511: return LOC_TREE;
        case 1521: return LOC_OAK;
        case 1519: return LOC_WILLOW;
        case 436: return LOC_COPPER;
        case 438: return LOC_TIN;
        case 440: return LOC_IRON;
        case 453: return LOC_COAL;
        case 447: return LOC_MITHRIL;
        case 7936: return LOC_ESSENCE;
        case 1779: return LOC_FLAX;
        default: return LOC_OTHER;
        }
    }
    for (int i = 0; i < m->station_count; i++) {
        if (m->stations[i].loc != id) continue;
        return m->stations[i].station == RC_MAKE_MENU_FURNACE ? LOC_FURNACE
             : m->stations[i].station == RC_MAKE_MENU_ANVIL ? LOC_ANVIL : LOC_WHEEL;
    }
    for (int i = 0; i < m->cook_object_count; i++) if ((int)m->cook_objects[i] == id) return LOC_RANGE;
    for (int i = 0; i < m->recipe_count; i++) {
        if (m->recipes[i].station != RC_MAKE_MENU_ALTAR || m->recipes[i].loc != id) continue;
        switch (m->recipes[i].recipe.output_item) {
        case 556: return LOC_AIR;
        case 555: return LOC_WATER;
        case 558: return LOC_MIND;
        case 562: return LOC_CHAOS;
        default: return LOC_DEATH;
        }
    }
    if (id == 10355) return LOC_BANK;
    if (id == 409) return LOC_PRAYER;
    if (id == 14880 || id == 17385 || id == 16511 || id == 14203 || id == 14204) return LOC_LINK;
    if (id == 14205 || id == 14206) return LOC_FOOD_PILE;
    return LOC_OTHER;
}

// ---- The agent's view ------------------------------------------------------------------------------

typedef struct { int key, index; } Ranked;

static int cmp_ranked(const void *a, const void *b) {
    const Ranked *x = a, *y = b;
    return x->key != y->key ? (x->key < y->key ? -1 : 1) : (x->index < y->index ? -1 : 1);
}

static int in_window(const RcPlayer *p, int x, int y, int plane) {
    return plane == p->plane && abs(x - p->x) <= RC_CENV_RADIUS && abs(y - p->y) <= RC_CENV_RADIUS;
}

void rc_cenv_refresh_view(RcCompactEnv *env, int agent) {
    RcWorld *w = env->world;
    RcCenvView *v = &env->views[agent];
    const RcPlayer *p = &w->players[agent];
    memset(v, 0, sizeof(*v));
    v->x = p->x;
    v->y = p->y;
    v->plane = p->plane;
    v->valid = 1;
    // NPCs: the attack target first, then nearest, then lowest uid.
    Ranked ranked[512];
    int n = 0;
    for (int i = 0; i < w->npc_count && n < 512; i++) {
        const RcNpc *npc = &w->npcs[i];
        if (!npc->active || npc->is_dead || !in_window(p, npc->x, npc->y, npc->plane)) continue;
        int key = npc->uid == p->attack_target ? -1 : dist(npc->x, npc->y, p->x, p->y);
        ranked[n++] = (Ranked){key, npc->uid};
    }
    qsort(ranked, (size_t)n, sizeof(Ranked), cmp_ranked);
    for (int i = 0; i < n && v->npc_count < RC_CENV_NPCS; i++) v->npc_uid[v->npc_count++] = ranked[i].index;
    // Locs with a working option that are still there (not depleted), nearest first, then in
    // index order.
    n = 0;
    for (int i = 0; i < env->loc_count && n < 512; i++) {
        const RcCenvLoc *l = &env->locs[i];
        RcObjectPlacement current;
        if (!in_window(p, l->x, l->y, l->plane)
                || !rc_world_object_current_placement(w, l->id, l->x, l->y, l->plane, 0, &current))
            continue;
        ranked[n++] = (Ranked){dist(l->x, l->y, p->x, p->y), i};
    }
    qsort(ranked, (size_t)n, sizeof(Ranked), cmp_ranked);
    for (int i = 0; i < n && v->loc_count < RC_CENV_LOCS; i++) v->locs[v->loc_count++] = env->locs[ranked[i].index];
    // Ground items visible to this player, nearest first.
    n = 0;
    for (int i = 0; i < w->ground_item_count && n < 512; i++) {
        const RcGroundItem *g = &w->ground_items[i];
        if (!g->active || g->arrival_timer || !in_window(p, g->x, g->y, g->plane)
                || !rc_ground_item_visible(g, p->index))
            continue;
        ranked[n++] = (Ranked){dist(g->x, g->y, p->x, p->y), i};
    }
    qsort(ranked, (size_t)n, sizeof(Ranked), cmp_ranked);
    for (int i = 0; i < n && v->ground_count < RC_CENV_GROUND; i++) {
        int g = ranked[i].index;
        v->ground[v->ground_count] = g;
        v->ground_uid[v->ground_count] = w->ground_items[g].uid;
        v->ground_version[v->ground_count++] = w->ground_items[g].version;
    }
    // Other players, nearest first.
    n = 0;
    for (int i = 0; i < w->player_count; i++) {
        const RcPlayer *o = &w->players[i];
        if (i == agent || !o->active || !in_window(p, o->x, o->y, o->plane)) continue;
        ranked[n++] = (Ranked){dist(o->x, o->y, p->x, p->y), i};
    }
    qsort(ranked, (size_t)n, sizeof(Ranked), cmp_ranked);
    for (int i = 0; i < n && v->player_count < RC_CENV_PLAYERS; i++) v->players[v->player_count++] = ranked[i].index;
    // Bank: the first occupied slots, while the bank is open.
    if (p->storage_kind == RC_STORAGE_BANK)
        for (int i = 0; i < RC_BANK_SIZE && v->bank_count < RC_CENV_BANK_SLOTS; i++)
            if (p->bank[i].item_id >= 0 && p->bank[i].quantity > 0) v->bank_slot[v->bank_count++] = i;
    // Production menu: the ready options.
    if (p->make_menu) {
        RcPlayer *previous = w->player;
        w->player = &w->players[agent];
        for (int i = 0; i < w->make_option_count && v->make_count < RC_CENV_MAKE_SLOTS; i++)
            if (rc_skilling_make_option_ready(w, i)) v->make_option[v->make_count++] = i;
        w->player = previous;
    }
}

// ---- Observation -----------------------------------------------------------------------------------

static int incoming(const RcWorld *w, const RcPlayer *p, int style_class, int *soonest) {
    int count = 0;
    *soonest = 0;
    for (int i = 0; i < p->num_pending_hits && i < RC_MAX_PENDING_HITS; i++) {
        const RcPendingHit *h = &p->pending_hits[i];
        if (!h->active) continue;
        int s = h->attack_style == COMBAT_RANGED ? 1 : h->attack_style == COMBAT_MAGIC ? 2
              : h->attack_style == COMBAT_NONE ? 3 : 0;
        if (s != style_class) continue;
        int ticks = h->apply_tick > w->tick ? (int)(h->apply_tick - w->tick) : 0;
        if (!count || ticks < *soonest) *soonest = ticks;
        count++;
    }
    return count;
}

static void encode_self(RcCompactEnv *env, int agent, float *o) {
    const RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    int ox = env->layout.min_rx * 64, oy = env->layout.min_ry * 64;
    int dungeon = p->x >= 512;
    o[0] = ((p->x - ox) & 127) / 128.0f;
    o[1] = ((p->y - oy) & 127) / 128.0f;
    o[2] = (float)dungeon;
    o[3] = p->max_hp ? clamp01((float)p->current_hp / p->max_hp) : 0;
    o[4] = p->max_hp / 400.0f;
    o[5] = clamp01(p->current_prayer_points / 400.0f);
    o[6] = p->run_energy / 10000.0f;
    o[7] = p->running;
    o[8] = p->special_energy / 10000.0f;
    o[9] = p->is_dead;
    o[10] = p->is_dead && p->respawn_tick > w->tick ? clamp01((p->respawn_tick - w->tick) / 10.0f) : 0;
    o[11] = p->attack_style_idx / 4.0f;
    const RcNpc *target = p->attack_target >= 0 ? rc_npc_resolve_const(w, (RcNpcId)p->attack_target) : NULL;
    if (target && in_window(p, target->x, target->y, target->plane)) {
        o[12] = 1;
        o[13] = (target->x - p->x) / (float)RC_CENV_RADIUS;
        o[14] = (target->y - p->y) / (float)RC_CENV_RADIUS;
        o[15] = target->spawn_hp ? clamp01((float)target->current_hp / target->spawn_hp) : 0;
    }
    o[16] = clamp01(p->attack_timer / 6.0f);
    o[17 + (p->skill_job.kind < 6 ? p->skill_job.kind : 0)] = 1;
    o[23] = p->skill_ready_tick > w->tick ? clamp01((p->skill_ready_tick - w->tick) / 8.0f) : 0;
    o[24] = p->interaction.active;
    o[25] = p->route_idx < p->route_len;
    o[26 + (p->make_menu < RC_MAKE_MENU_COUNT ? p->make_menu : 0)] = 1;
    o[37] = p->shop_open >= 0;
    o[38] = p->storage_kind == RC_STORAGE_BANK;
    o[39] = p->trade.stage / 2.0f;
    for (int i = 0; i < RC_CENV_PRAYERS; i++)
        o[40 + i] = env->prayer_ids[i] >= 0 && (p->active_prayers >> env->prayer_ids[i] & 1);
    for (int i = 0; i < RC_CENV_SPELLS; i++) o[58 + i] = env->spell_ids[i] >= 0 && p->autocast_spell == env->spell_ids[i];
    o[64] = p->autocast_spell >= 0;
    for (int s = 0; s < 4; s++) {  // projectiles and blows in flight at the agent: style, timing
        int soonest, count = incoming(w, p, s, &soonest);
        o[65 + s] = clamp01(count / 3.0f);
        o[69 + s] = count ? clamp01(soonest / 6.0f) : 0;
    }
    o[73] = rc_cenv_in_arena(env, p);
    int free_slots = 0;
    int64_t coins = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) {
        free_slots += p->inventory[i].item_id < 0;
        if (p->inventory[i].item_id == COINS) coins += p->inventory[i].quantity;
    }
    o[74] = free_slots / 28.0f;
    o[75] = log2f((float)coins + 1) / 20.0f;
    for (int i = 0; i < 12 && i < 14; i++) o[76 + i] = clamp01((p->equipment_bonuses[i] + 50) / 200.0f);
    o[88] = rc_combat_level(&p->skills) / 126.0f;
    o[89] = p->trade.stage == RC_TRADE_STAGE_REQUESTED;
}

static void encode_skills(const RcCompactEnv *env, int agent, float *o) {
    const RcPlayer *p = &env->world->players[agent];
    static const int skills[RC_CENV_SKILLS] = {
        SKILL_ATTACK, SKILL_STRENGTH, SKILL_DEFENCE, SKILL_HITPOINTS, SKILL_RANGED, SKILL_MAGIC,
        SKILL_PRAYER, SKILL_MINING, SKILL_SMITHING, SKILL_WOODCUTTING, SKILL_FLETCHING,
        SKILL_FISHING, SKILL_COOKING, SKILL_CRAFTING, SKILL_RUNECRAFT,
    };
    int cap = env->rules->level_cap;
    for (int i = 0; i < RC_CENV_SKILLS; i++) {
        int s = skills[i], level = p->skills.base_level[s];
        o[i * 3] = level / (float)cap;
        o[i * 3 + 1] = p->skills.boosted_level[s] / (float)cap;
        if (level < cap && level < 99) {
            int lo = RC_XP_TABLE[level - 1], hi = RC_XP_TABLE[level];
            o[i * 3 + 2] = clamp01((float)(p->skills.xp[s] - lo) / (float)(hi - lo));
        } else {
            o[i * 3 + 2] = 1;
        }
    }
}

static void encode_tiles(RcCompactEnv *env, int agent, float *o) {
    const RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    for (int dy = -RC_CENV_RADIUS; dy <= RC_CENV_RADIUS; dy++)
        for (int dx = -RC_CENV_RADIUS; dx <= RC_CENV_RADIUS; dx++) {
            int t = (dy + RC_CENV_RADIUS) * RC_CENV_SIDE + (dx + RC_CENV_RADIUS);
            o[t] = rc_tile_blocked(&w->map, p->x + dx, p->y + dy, p->plane) ? 1.0f : 0.0f;
        }
    float *hazard = o + RC_CENV_TILES, *occupied = o + 2 * RC_CENV_TILES;
    const RcModeBossState *b = &w->mode_boss;
    for (int i = 0; i < b->hazard_count; i++) {
        int dx = b->hazards[i].x - p->x, dy = b->hazards[i].y - p->y;
        if (p->plane || abs(dx) > RC_CENV_RADIUS || abs(dy) > RC_CENV_RADIUS) continue;
        int ticks = b->hazards[i].land_tick > w->tick ? (int)(b->hazards[i].land_tick - w->tick) : 0;
        hazard[(dy + RC_CENV_RADIUS) * RC_CENV_SIDE + dx + RC_CENV_RADIUS] = 1.0f - ticks / 4.0f;
    }
    for (int i = 0; i < w->npc_count; i++) {
        const RcNpc *npc = &w->npcs[i];
        if (!npc->active || npc->is_dead || npc->plane != p->plane) continue;
        const RcNpcDef *d = rc_npc_def_for_npc(w, npc);
        int size = d && d->size > 0 ? d->size : 1;
        for (int sx = 0; sx < size; sx++)
            for (int sy = 0; sy < size; sy++) {
                int dx = npc->x + sx - p->x, dy = npc->y + sy - p->y;
                if (abs(dx) <= RC_CENV_RADIUS && abs(dy) <= RC_CENV_RADIUS)
                    occupied[(dy + RC_CENV_RADIUS) * RC_CENV_SIDE + dx + RC_CENV_RADIUS] = 0.5f;
            }
    }
    for (int i = 0; i < w->player_count; i++) {
        const RcPlayer *o2 = &w->players[i];
        int dx = o2->x - p->x, dy = o2->y - p->y;
        if (i == agent || !o2->active || o2->plane != p->plane || abs(dx) > RC_CENV_RADIUS
                || abs(dy) > RC_CENV_RADIUS)
            continue;
        occupied[(dy + RC_CENV_RADIUS) * RC_CENV_SIDE + dx + RC_CENV_RADIUS] = 1.0f;
    }
}

static int last_style_class(const RcNpc *npc) {
    switch (npc->combat.selected_npc_style) {
    case COMBAT_RANGED: return 1;
    case COMBAT_MAGIC: return 2;
    case COMBAT_NONE: return -1;
    default: return 0;
    }
}

static void encode_npcs(RcCompactEnv *env, int agent, float *o) {
    RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    const RcCenvView *v = &env->views[agent];
    for (int k = 0; k < v->npc_count; k++) {
        float *e = o + k * RC_CENV_NPC;
        const RcNpc *npc = rc_npc_resolve_const(w, (RcNpcId)v->npc_uid[k]);
        const RcNpcDef *d = npc ? rc_npc_def_for_npc(w, npc) : NULL;
        if (!npc || !d) continue;
        e[0] = 1;
        e[1] = (npc->x - p->x) / (float)RC_CENV_RADIUS;
        e[2] = (npc->y - p->y) / (float)RC_CENV_RADIUS;
        e[3 + npc_family(env, d->id)] = 1;
        e[14] = npc->spawn_hp ? clamp01((float)npc->current_hp / npc->spawn_hp) : 0;
        e[15] = d->size / 3.0f;
        e[16] = npc->uid == p->attack_target;
        e[17] = npc->target_uid == agent;
        e[18] = npc->target_uid >= 0;
        int style = npc->target_uid >= 0 ? last_style_class(npc) : -1;
        if (style >= 0) e[19 + style] = 1;
        const RcModeRules *m = env->rules;
        if (m->has_boss && d->id == m->boss.npc) {
            e[22] = w->mode_boss.phase / 3.0f;
            e[23] = w->mode_boss.feeding_pile >= 0;
        }
        for (int o2 = 0; o2 < RC_NPC_OPTION_COUNT; o2++) {
            if (!d->options[o2][0]) continue;
            if (rc_npc_def_option_is_attack(d, o2)) e[24] = 1;
            else e[25] = 1;
        }
        e[26] = d->combat_level > 0 ? clamp01(d->combat_level / 300.0f) : 0;
    }
}

static void encode_locs(RcCompactEnv *env, int agent, float *o) {
    const RcPlayer *p = &env->world->players[agent];
    const RcCenvView *v = &env->views[agent];
    for (int k = 0; k < v->loc_count; k++) {
        float *e = o + k * RC_CENV_LOC;
        const RcCenvLoc *l = &v->locs[k];
        e[0] = 1;
        e[1] = (l->x - p->x) / (float)RC_CENV_RADIUS;
        e[2] = (l->y - p->y) / (float)RC_CENV_RADIUS;
        e[3 + loc_kind(env, l->id)] = 1;
        for (int i = 0; i < 3; i++) e[3 + LOC_KINDS + i] = l->ops[i] >= 0;
    }
}

static void encode_ground(RcCompactEnv *env, int agent, float *o) {
    const RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    const RcCenvView *v = &env->views[agent];
    for (int k = 0; k < v->ground_count; k++) {
        float *e = o + k * RC_CENV_GROUND_ITEM;
        const RcGroundItem *g = &w->ground_items[v->ground[k]];
        e[0] = (g->x - p->x) / (float)RC_CENV_RADIUS;
        e[1] = (g->y - p->y) / (float)RC_CENV_RADIUS;
        encode_item(env, e + 2, g->item_id, g->quantity, g->state_id);
        e[22] = g->owner_uid == agent;
        e[23] = g->owner_uid == RC_GROUND_OWNER_NONE;
    }
}

static void encode_players(RcCompactEnv *env, int agent, float *o) {
    const RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    const RcCenvView *v = &env->views[agent];
    for (int k = 0; k < v->player_count; k++) {
        float *e = o + k * RC_CENV_PLAYER;
        const RcPlayer *q = &w->players[v->players[k]];
        e[0] = 1;
        e[1] = (q->x - p->x) / (float)RC_CENV_RADIUS;
        e[2] = (q->y - p->y) / (float)RC_CENV_RADIUS;
        e[3] = q->max_hp ? clamp01((float)q->current_hp / q->max_hp) : 0;
        e[4] = rc_combat_level(&q->skills) / 126.0f;
        e[5] = q->attack_target >= 0;
        e[6] = p->trade.stage == RC_TRADE_STAGE_OPEN && p->trade.partner == q->index;
        e[7] = q->trade.stage == RC_TRADE_STAGE_REQUESTED && q->trade.partner == agent;
        e[8] = q->is_dead;
        e[9] = rc_cenv_in_arena(env, q);
    }
}

static void encode_hazards(RcCompactEnv *env, int agent, float *o) {
    const RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    const RcModeBossState *b = &w->mode_boss;
    Ranked ranked[RC_MODE_BOSS_HAZARDS];
    int n = 0;
    for (int i = 0; i < b->hazard_count; i++)
        if (!p->plane && abs(b->hazards[i].x - p->x) <= RC_CENV_RADIUS && abs(b->hazards[i].y - p->y) <= RC_CENV_RADIUS)
            ranked[n++] = (Ranked){(int)(b->hazards[i].land_tick - w->tick) * 64
                                       + dist(b->hazards[i].x, b->hazards[i].y, p->x, p->y), i};
    qsort(ranked, (size_t)n, sizeof(Ranked), cmp_ranked);
    for (int k = 0; k < n && k < RC_CENV_HAZARDS; k++) {
        const RcModeHazard *h = &b->hazards[ranked[k].index];
        o[k * 4] = 1;
        o[k * 4 + 1] = (h->x - p->x) / (float)RC_CENV_RADIUS;
        o[k * 4 + 2] = (h->y - p->y) / (float)RC_CENV_RADIUS;
        o[k * 4 + 3] = h->land_tick > w->tick ? clamp01((h->land_tick - w->tick) / 4.0f) : 0;
    }
}

static void encode_landmarks(RcCompactEnv *env, int agent, float *o) {
    if (!env->cfg.public_map) return;
    const RcPlayer *p = &env->world->players[agent];
    for (int i = 0; i < env->layout.landmark_count && i < RC_CENV_LANDMARKS; i++) {
        const RcGymLandmark *lm = &env->layout.landmarks[i];
        int walk = rc_env_travel_distance(i, p->x, p->y, p->plane);
        if (walk < 0) continue;  // another area: reached through a link
        o[i * 4] = 1;
        o[i * 4 + 1] = (lm->x - p->x) / 128.0f;
        o[i * 4 + 2] = (lm->y - p->y) / 128.0f;
        o[i * 4 + 3] = clamp01(walk / 256.0f);
    }
}

static void encode_interfaces(RcCompactEnv *env, int agent, float *obs) {
    RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    const RcCenvView *v = &env->views[agent];
    float *o = obs + RC_COBS_SHOP;
    if (p->shop_open >= 0 && p->shop_open < w->shop_count) {
        const RcShopState *s = &w->shops[p->shop_open];
        o[0] = 1;
        o[1 + (p->shop_open < 2 ? p->shop_open : 1)] = 1;
        int coins = 0;
        for (int i = 0; i < RC_INVENTORY_SIZE; i++)
            if (p->inventory[i].item_id == COINS) coins += p->inventory[i].quantity;
        o[3] = log2f((float)coins + 1) / 20.0f;
        for (int i = 0; i < s->slot_count && i < RC_CENV_SHOP_SLOTS; i++) {
            float *e = o + 4 + i * 5;
            int price = rc_shop_buy_price(w, p->shop_open, i);
            e[0] = 1;
            e[1] = vocab_of(env, s->slots[i].item_id);
            e[2] = clamp01(price / 1000.0f);
            e[3] = clamp01(s->slots[i].stock / 500.0f);
            e[4] = s->slots[i].stock > 0 && coins >= price;
        }
    }
    o = obs + RC_COBS_BANK;
    if (p->storage_kind == RC_STORAGE_BANK) {
        o[0] = 1;
        int used = 0;
        for (int i = 0; i < RC_BANK_SIZE; i++) used += p->bank[i].item_id >= 0;
        o[1] = used / 800.0f;
        for (int k = 0; k < v->bank_count; k++) {
            const RcInvSlot *s = &p->bank[v->bank_slot[k]];
            float *e = o + 2 + k * 4;
            e[0] = 1;
            e[1] = vocab_of(env, s->item_id);
            e[2] = log2f((float)s->quantity + 1) / 20.0f;
            e[3] = clamp01(rc_mode_sale_value(env->rules, s->item_id) / 1000.0f);
        }
    }
    o = obs + RC_COBS_MAKE;
    if (p->make_menu) {
        o[p->make_menu < 11 ? p->make_menu : 0] = 1;
        o[11] = 1;
        for (int k = 0; k < v->make_count; k++) {
            const RcRecipe *r = rc_recipe_get(w->make_options[v->make_option[k]].recipe);
            float *e = o + 12 + k * 4;
            if (!r) continue;
            int fee = 0;
            for (int i = 0; i < r->input_count; i++)
                if (r->inputs[i].item_id == COINS) fee = r->inputs[i].quantity;
            e[0] = 1;
            e[1] = vocab_of(env, (int)r->output_item);
            e[2] = r->output_qty / 15.0f;
            e[3] = clamp01(fee / 100.0f);
        }
    }
    o = obs + RC_COBS_TRADE;
    o[p->trade.stage < 3 ? p->trade.stage : 0] = 1;
    if (p->trade.stage == RC_TRADE_STAGE_OPEN && p->trade.partner >= 0) {
        const RcPlayer *q = &w->players[p->trade.partner];
        o[3] = (q->x - p->x) / (float)RC_TRADE_RANGE;
        o[4] = (q->y - p->y) / (float)RC_TRADE_RANGE;
        o[5] = (p->trade.revision & 1) ? 1.0f : 0.0f;
        o[6] = p->trade.accepted_revision == p->trade.revision;
        o[7] = q->trade.accepted_revision == p->trade.revision;
        o[8] = p->trade.offer_count / 28.0f;
        o[9] = q->trade.offer_count / 28.0f;
        for (int side = 0; side < 2; side++) {
            const RcPlayer *who = side ? q : p;
            for (int k = 0; k < who->trade.offer_count && k < RC_CENV_OFFER_SLOTS; k++) {
                const RcInvSlot *s = &who->trade.offer[k];
                float *e = o + 10 + side * RC_CENV_OFFER_SLOTS * 4 + k * 4;
                e[0] = 1;
                e[1] = vocab_of(env, s->item_id);
                e[2] = log2f((float)s->quantity + 1) / 20.0f;
                e[3] = clamp01(rc_mode_sale_value(env->rules, s->item_id) / 1000.0f);
            }
        }
    }
}

void rc_cenv_encode(RcCompactEnv *env, int agent, float *obs) {
    memset(obs, 0, RC_CENV_OBS_SIZE * sizeof(float));
    if (!env->views[agent].valid) rc_cenv_refresh_view(env, agent);
    const RcPlayer *p = &env->world->players[agent];
    encode_self(env, agent, obs + RC_COBS_SELF);
    encode_skills(env, agent, obs + RC_COBS_SKILLS);
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        encode_item(env, obs + RC_COBS_INVENTORY + i * RC_CENV_ITEM, p->inventory[i].item_id,
                    p->inventory[i].quantity, p->inventory[i].state_id);
    for (int i = 0; i < RC_EQUIP_COUNT && i < 11; i++)
        encode_item(env, obs + RC_COBS_EQUIPMENT + i * RC_CENV_ITEM, p->equipment[i].item_id,
                    p->equipment[i].quantity, p->equipment[i].state_id);
    encode_tiles(env, agent, obs + RC_COBS_TILES);
    encode_npcs(env, agent, obs + RC_COBS_NPCS);
    encode_locs(env, agent, obs + RC_COBS_LOCS);
    encode_ground(env, agent, obs + RC_COBS_GROUND);
    encode_players(env, agent, obs + RC_COBS_PLAYERS);
    encode_hazards(env, agent, obs + RC_COBS_HAZARDS);
    encode_landmarks(env, agent, obs + RC_COBS_LANDMARKS);
    encode_interfaces(env, agent, obs);
    float *e = obs + RC_COBS_EPISODE;
    e[env->profile < 4 ? env->profile : 0] = 1;
    e[4] = env->horizon ? clamp01((float)(env->world->tick - env->start_tick) / env->horizon) : 0;
    e[5] = env->success;
    e[6] = clamp01(env->agents[agent].m.boss_credits / 10.0f);
    e[7] = env->num_agents / 8.0f;
    e[8] = env->horizon / 12000.0f;
    e[9] = env->agents[agent].terminated;  // the task completed
    e[10] = env->agents[agent].truncated;  // the horizon ran out
}

// ---- Mask ------------------------------------------------------------------------------------------

static int first_item_option(int item, int alt) {
    const RcItemDef *d = rc_item_def_get(item);
    int found = -1;
    for (int o = 0; d && o < RC_ITEM_ACTION_COUNT; o++) {
        const char *a = d->inventory_actions[o];
        if (!a[0] || !strcmp(a, "Drop") || !strcmp(a, "Examine") || !strcmp(a, "Destroy")) continue;
        if (!alt) return o;
        found = o;
    }
    return alt ? found : -1;
}

static int item_option_is_equip(int item, int op) {
    const RcItemDef *d = rc_item_def_get(item);
    if (!d || op < 0) return 0;
    const char *a = d->inventory_actions[op];
    return !strcmp(a, "Wield") || !strcmp(a, "Wear") || !strcmp(a, "Equip");
}

static int npc_other_option(const RcNpcDef *d, int which) {
    for (int o = 0, k = 0; d && o < RC_NPC_OPTION_COUNT; o++) {
        if (!d->options[o][0] || rc_npc_def_option_is_attack(d, o) || !strcmp(d->options[o], "Examine"))
            continue;
        if (k++ == which) return o;
    }
    return -1;
}

static int npc_attackable(const RcNpcDef *d) {
    for (int o = 0; d && o < RC_NPC_OPTION_COUNT; o++)
        if (d->options[o][0] && rc_npc_def_option_is_attack(d, o)) return 1;
    return 0;
}

static int bank_open(const RcPlayer *p) { return p->storage_kind == RC_STORAGE_BANK; }

void rc_cenv_mask(RcCompactEnv *env, int agent, uint8_t *mask) {
    memset(mask, 0, RC_CENV_NUM_ACTIONS);
    RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    const RcCenvAgent *a = &env->agents[agent];
    mask[RC_CACT_WAIT] = 1;
    if (p->is_dead || a->terminated || a->truncated) return;
    if (!env->views[agent].valid) rc_cenv_refresh_view(env, agent);
    const RcCenvView *v = &env->views[agent];
    mask[RC_CACT_STOP] = p->route_idx < p->route_len || p->attack_target >= 0 || p->skill_action
                      || p->interaction.active;
    mask[RC_CACT_RUN] = 1;
    for (int dy = -RC_CENV_RADIUS; dy <= RC_CENV_RADIUS; dy++)
        for (int dx = -RC_CENV_RADIUS; dx <= RC_CENV_RADIUS; dx++)
            if ((dx || dy) && !rc_tile_blocked(&w->map, p->x + dx, p->y + dy, p->plane))
                mask[RC_CACT_MOVE + (dy + RC_CENV_RADIUS) * RC_CENV_SIDE + dx + RC_CENV_RADIUS] = 1;
    for (int i = 0; env->cfg.public_map && i < env->layout.landmark_count && i < RC_CENV_LANDMARKS; i++)
        mask[RC_CACT_TRAVEL + i] = rc_env_travel_distance(i, p->x, p->y, p->plane) > 2;
    for (int k = 0; k < v->npc_count; k++) {
        const RcNpc *npc = rc_npc_resolve_const(w, (RcNpcId)v->npc_uid[k]);
        const RcNpcDef *d = npc ? rc_npc_def_for_npc(w, npc) : NULL;
        if (!d || npc->is_dead) continue;
        mask[RC_CACT_ATTACK + k] = npc_attackable(d) && npc->uid != p->attack_target;
        for (int j = 0; j < 2; j++) mask[RC_CACT_NPC_OP + k * 2 + j] = npc_other_option(d, j) >= 0;
    }
    for (int k = 0; k < v->loc_count; k++)
        for (int j = 0; j < 3; j++) mask[RC_CACT_LOC_OP + k * 3 + j] = v->locs[k].ops[j] >= 0;
    int has_coins = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) {
        int item = p->inventory[i].item_id;
        if (item < 0) continue;
        if (item == COINS) has_coins = p->inventory[i].quantity;
        mask[RC_CACT_ITEM + i] = first_item_option(item, 0) >= 0;
        int alt = first_item_option(item, 1);
        mask[RC_CACT_ITEM_ALT + i] = alt >= 0 && alt != first_item_option(item, 0);
        mask[RC_CACT_DROP + i] = 1;
        if (bank_open(p)) mask[RC_CACT_DEPOSIT + i] = 1;
        if (p->shop_open >= 0) mask[RC_CACT_SELL + i] = rc_shop_sell_price(w, p->shop_open, item) > 0;
        const RcItemDef *d = rc_item_def_get(item);
        if (p->trade.stage == RC_TRADE_STAGE_OPEN) mask[RC_CACT_TRADE_OFFER + i] = d && d->tradeable;
    }
    for (int i = 0; i < RC_EQUIP_COUNT && i < 11; i++) mask[RC_CACT_UNEQUIP + i] = p->equipment[i].item_id >= 0;
    for (int c = 0; c < env->combo_count; c++)
        mask[RC_CACT_COMBINE + c] = rc_inv_find(p->inventory, env->combos[c].a) >= 0
                                 && rc_inv_find(p->inventory, env->combos[c].b) >= 0;
    for (int k = 0; k < v->ground_count; k++) mask[RC_CACT_PICKUP + k] = 1;
    for (int i = 0; i < RC_CENV_PRAYERS; i++) {
        int id = env->prayer_ids[i];
        const RcPrayerDef *d = id >= 0 ? rc_prayer_def_get(id) : NULL;
        int active = id >= 0 && (p->active_prayers >> id & 1);
        mask[RC_CACT_PRAYER + i] = d && (active || (p->skills.base_level[SKILL_PRAYER] >= d->level
                                                    && p->current_prayer_points > 0));
    }
    for (int i = 0; i < 4; i++) mask[RC_CACT_STYLE + i] = i != p->attack_style_idx;
    for (int i = 0; i < RC_CENV_SPELLS; i++) {
        int id = env->spell_ids[i];
        const RcSpellDef *d = id >= 0 ? rc_spell_def_get(id) : NULL;
        mask[RC_CACT_AUTOCAST + i] = d && p->skills.boosted_level[SKILL_MAGIC] >= d->level
                                  && p->autocast_spell != id;
    }
    for (int k = 0; k < v->make_count; k++) mask[RC_CACT_MAKE_ONE + k] = mask[RC_CACT_MAKE_ALL + k] = 1;
    if (bank_open(p)) {
        mask[RC_CACT_DEPOSIT_INVENTORY] = 1;
        mask[RC_CACT_DEPOSIT_EQUIPMENT] = 1;
        for (int k = 0; k < v->bank_count; k++) mask[RC_CACT_WITHDRAW_ONE + k] = mask[RC_CACT_WITHDRAW_ALL + k] = 1;
    }
    if (p->shop_open >= 0 && p->shop_open < w->shop_count) {
        const RcShopState *s = &w->shops[p->shop_open];
        for (int i = 0; i < s->slot_count && i < RC_CENV_SHOP_SLOTS; i++) {
            int price = rc_shop_buy_price(w, p->shop_open, i);
            mask[RC_CACT_BUY_ONE + i] = s->slots[i].stock > 0 && has_coins >= price;
            mask[RC_CACT_BUY_TEN + i] = s->slots[i].stock > 0 && has_coins >= price;
        }
    }
    mask[RC_CACT_CLOSE] = bank_open(p) || p->shop_open >= 0 || p->make_menu;
    for (int k = 0; k < v->player_count; k++) {
        const RcPlayer *q = &w->players[v->players[k]];
        mask[RC_CACT_TRADE_REQUEST + k] = p->trade.stage != RC_TRADE_STAGE_OPEN && !q->is_dead
            && abs(q->x - p->x) <= RC_TRADE_RANGE && abs(q->y - p->y) <= RC_TRADE_RANGE;
    }
    if (p->trade.stage == RC_TRADE_STAGE_OPEN) {
        for (int k = 0; k < p->trade.offer_count && k < RC_CENV_OFFER_SLOTS; k++)
            mask[RC_CACT_TRADE_WITHDRAW + k] = 1;
        mask[RC_CACT_TRADE_ACCEPT] = p->trade.accepted_revision != p->trade.revision;
    }
    mask[RC_CACT_TRADE_DECLINE] = p->trade.stage != RC_TRADE_STAGE_NONE;
}

// ---- Actions ---------------------------------------------------------------------------------------

static RcCenvResult accepted(int ok) {
    return ok ? RC_CENV_OK : RC_CENV_REJECTED;
}

RcCenvResult rc_cenv_execute(RcCompactEnv *env, int agent, int a) {
    RcWorld *w = env->world;
    rc_world_select_player(w, agent);
    RcPlayer *p = w->player;
    const RcCenvView *v = &env->views[agent];
    if (a < 0 || a >= RC_CENV_NUM_ACTIONS) return RC_CENV_OUT_OF_RANGE;
    if (a == RC_CACT_WAIT) return RC_CENV_OK;
    if (p->is_dead) return RC_CENV_DEAD;
    uint8_t mask[RC_CENV_NUM_ACTIONS];
    rc_cenv_mask(env, agent, mask);
    if (!mask[a]) return RC_CENV_MASKED;
    if (a == RC_CACT_STOP) return accepted(rc_player_walk_to(w, p->x, p->y));
    if (a == RC_CACT_RUN) return accepted(rc_player_set_running(w, !p->running));
    if (a < RC_CACT_TRAVEL) {
        int t = a - RC_CACT_MOVE;
        return accepted(rc_player_walk_to(w, v->x + t % RC_CENV_SIDE - RC_CENV_RADIUS,
                                          v->y + t / RC_CENV_SIDE - RC_CENV_RADIUS));
    }
    if (a < RC_CACT_ATTACK) {
        int x, y;
        if (!rc_env_travel_waypoint(&w->map, a - RC_CACT_TRAVEL, p->x, p->y, p->plane, 24, &x, &y))
            return RC_CENV_STALE;
        return accepted(rc_player_walk_to(w, x, y));
    }
    if (a < RC_CACT_NPC_OP) {
        const RcNpc *npc = rc_npc_resolve_const(w, (RcNpcId)v->npc_uid[a - RC_CACT_ATTACK]);
        if (!npc || npc->is_dead) return RC_CENV_STALE;
        return accepted(rc_player_attack_npc(w, npc->uid));
    }
    if (a < RC_CACT_LOC_OP) {
        int k = (a - RC_CACT_NPC_OP) / 2, j = (a - RC_CACT_NPC_OP) % 2;
        const RcNpc *npc = rc_npc_resolve_const(w, (RcNpcId)v->npc_uid[k]);
        const RcNpcDef *d = npc ? rc_npc_def_for_npc(w, npc) : NULL;
        int op = npc_other_option(d, j);
        if (!d || op < 0 || npc->is_dead) return RC_CENV_STALE;
        rc_player_interact_npc(w, npc->uid, op);
        return RC_CENV_OK;
    }
    if (a < RC_CACT_ITEM) {
        const RcCenvLoc *l = &v->locs[(a - RC_CACT_LOC_OP) / 3];
        int op = l->ops[(a - RC_CACT_LOC_OP) % 3];
        return accepted(rc_player_interact_object_at(w, l->id, l->x, l->y, l->plane, op));
    }
    if (a < RC_CACT_DROP) {
        int slot = (a - RC_CACT_ITEM) % 28, item = p->inventory[slot].item_id;
        int op = first_item_option(item, a >= RC_CACT_ITEM_ALT);
        if (op < 0) return RC_CENV_STALE;
        if (item_option_is_equip(item, op)) return accepted(rc_item_result_accepted(rc_player_equip(w, slot)));
        int consume = rc_consumable_action(item, op);
        if (consume == 1) return accepted(rc_player_eat(w, slot));
        if (consume == 2) return accepted(rc_player_drink(w, slot));
        return accepted(rc_player_interact_inventory_item(w, slot, op));
    }
    if (a < RC_CACT_UNEQUIP)
        return accepted(rc_item_result_accepted(rc_player_drop_item(w, a - RC_CACT_DROP)));
    if (a < RC_CACT_COMBINE)
        return accepted(rc_item_result_accepted(rc_player_unequip(w, a - RC_CACT_UNEQUIP)));
    if (a < RC_CACT_PICKUP) {
        int c = a - RC_CACT_COMBINE;
        int sa = rc_inv_find(p->inventory, env->combos[c].a), sb = rc_inv_find(p->inventory, env->combos[c].b);
        return accepted(rc_player_use_inventory_item_on_inventory_item(w, sa, sb));
    }
    if (a < RC_CACT_PRAYER) {
        int k = a - RC_CACT_PICKUP, g = v->ground[k];
        if (g >= w->ground_item_count || !w->ground_items[g].active || w->ground_items[g].uid != v->ground_uid[k])
            return RC_CENV_STALE;
        return accepted(rc_item_result_accepted(rc_player_pickup_item(w, g)));
    }
    if (a < RC_CACT_STYLE) {
        RcPrayerResult r = rc_player_set_prayer(w, env->prayer_ids[a - RC_CACT_PRAYER]);
        return accepted(rc_prayer_result_accepted(r));
    }
    if (a < RC_CACT_AUTOCAST) {
        rc_player_set_attack_style(w, a - RC_CACT_STYLE);
        return RC_CENV_OK;
    }
    if (a < RC_CACT_MAKE_ONE)
        return accepted(rc_spell_result_accepted(rc_player_set_autocast_spell(w, env->spell_ids[a - RC_CACT_AUTOCAST], 0)));
    if (a < RC_CACT_DEPOSIT) {
        int all = a >= RC_CACT_MAKE_ALL;
        int k = a - (all ? RC_CACT_MAKE_ALL : RC_CACT_MAKE_ONE);
        return accepted(rc_skilling_make(w, v->make_option[k], all ? 0 : 1));
    }
    if (a < RC_CACT_DEPOSIT_INVENTORY) {
        int slot = a - RC_CACT_DEPOSIT;
        return accepted(rc_bank_deposit_slot(w, slot, 1 << 30) >= 0);
    }
    if (a == RC_CACT_DEPOSIT_INVENTORY) return accepted(rc_bank_deposit_all(w, false) >= 0);
    if (a == RC_CACT_DEPOSIT_EQUIPMENT) return accepted(rc_bank_deposit_all(w, true) >= 0);
    if (a < RC_CACT_BUY_ONE) {
        int all = a >= RC_CACT_WITHDRAW_ALL;
        int k = a - (all ? RC_CACT_WITHDRAW_ALL : RC_CACT_WITHDRAW_ONE);
        if (p->bank[v->bank_slot[k]].item_id < 0) return RC_CENV_STALE;
        return accepted(rc_bank_withdraw_slot(w, v->bank_slot[k], all ? 1 << 30 : 1) >= 0);
    }
    if (a < RC_CACT_SELL) {
        int ten = a >= RC_CACT_BUY_TEN, slot = a - (ten ? RC_CACT_BUY_TEN : RC_CACT_BUY_ONE);
        return accepted(rc_shop_buy(w, slot, ten ? 10 : 1));
    }
    if (a < RC_CACT_CLOSE) {
        int slot = a - RC_CACT_SELL;
        return accepted(rc_shop_sell(w, slot, p->inventory[slot].quantity));
    }
    if (a == RC_CACT_CLOSE) {
        if (bank_open(p)) (void)rc_player_close_storage(w);
        rc_shop_close(p);
        rc_skilling_close_make(p);
        return RC_CENV_OK;
    }
    if (a < RC_CACT_TRADE_OFFER) return accepted(rc_trade_request(w, v->players[a - RC_CACT_TRADE_REQUEST]));
    if (a < RC_CACT_TRADE_WITHDRAW) {
        int slot = a - RC_CACT_TRADE_OFFER;
        return accepted(rc_trade_offer(w, slot, p->inventory[slot].quantity));
    }
    if (a < RC_CACT_TRADE_ACCEPT) {
        int k = a - RC_CACT_TRADE_WITHDRAW;
        return accepted(rc_trade_withdraw(w, k, p->trade.offer[k].quantity));
    }
    if (a == RC_CACT_TRADE_ACCEPT) return accepted(rc_trade_accept(w, p->trade.revision));
    return accepted(rc_trade_decline(w));
}

const char *rc_cenv_action_name(int a, char *buf, int cap) {
    static const struct { int base; const char *name; } ranges[] = {
        {RC_CACT_WAIT, "wait"}, {RC_CACT_STOP, "stop"}, {RC_CACT_RUN, "run"}, {RC_CACT_MOVE, "move"},
        {RC_CACT_TRAVEL, "travel"}, {RC_CACT_ATTACK, "attack"}, {RC_CACT_NPC_OP, "npc-op"},
        {RC_CACT_LOC_OP, "loc-op"}, {RC_CACT_ITEM, "item"}, {RC_CACT_ITEM_ALT, "item-alt"},
        {RC_CACT_DROP, "drop"}, {RC_CACT_UNEQUIP, "unequip"}, {RC_CACT_COMBINE, "combine"},
        {RC_CACT_PICKUP, "pickup"}, {RC_CACT_PRAYER, "prayer"}, {RC_CACT_STYLE, "style"},
        {RC_CACT_AUTOCAST, "autocast"}, {RC_CACT_MAKE_ONE, "make-one"}, {RC_CACT_MAKE_ALL, "make-all"},
        {RC_CACT_DEPOSIT, "deposit"}, {RC_CACT_DEPOSIT_INVENTORY, "deposit-inventory"},
        {RC_CACT_DEPOSIT_EQUIPMENT, "deposit-equipment"}, {RC_CACT_WITHDRAW_ONE, "withdraw-one"},
        {RC_CACT_WITHDRAW_ALL, "withdraw-all"}, {RC_CACT_BUY_ONE, "buy-one"}, {RC_CACT_BUY_TEN, "buy-ten"},
        {RC_CACT_SELL, "sell"}, {RC_CACT_CLOSE, "close"}, {RC_CACT_TRADE_REQUEST, "trade-request"},
        {RC_CACT_TRADE_OFFER, "trade-offer"}, {RC_CACT_TRADE_WITHDRAW, "trade-withdraw"},
        {RC_CACT_TRADE_ACCEPT, "trade-accept"}, {RC_CACT_TRADE_DECLINE, "trade-decline"},
        {RC_CENV_NUM_ACTIONS, NULL},
    };
    if (a < 0 || a >= RC_CENV_NUM_ACTIONS) {
        snprintf(buf, (size_t)cap, "invalid %d", a);
        return buf;
    }
    for (int i = 0; ranges[i + 1].name || ranges[i + 1].base == RC_CENV_NUM_ACTIONS; i++) {
        if (a >= ranges[i + 1].base) continue;
        if (ranges[i + 1].base - ranges[i].base == 1) snprintf(buf, (size_t)cap, "%s", ranges[i].name);
        else snprintf(buf, (size_t)cap, "%s %d", ranges[i].name, a - ranges[i].base);
        return buf;
    }
    snprintf(buf, (size_t)cap, "action %d", a);
    return buf;
}
