// What agents see (observations), what they may do (masks) and how their
// actions become game commands.
#include "env_internal.h"

#include "../rc-core/api.h"
#include "../rc-core/area_flags.h"
#include "../rc-core/combat.h"
#include "../rc-core/consumables.h"
#include "../rc-core/interaction.h"
#include "../rc-core/items.h"
#include "../rc-core/npc.h"
#include "../rc-core/object_runtime.h"
#include "../rc-core/objects.h"
#include "../rc-core/pathfinding.h"
#include "../rc-core/prayer.h"
#include "../rc-core/shops.h"
#include "../rc-core/skilling.h"
#include "../rc-core/skills.h"
#include "../rc-core/spells.h"
#include "../rc-core/storage.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define NEAR_RANGE 16
#define RC_MAX_NPC_DEFS_ENV 65536
#define COINS 995
#define TINDERBOX 590
#define HAMMER 2347

static const int tracked_skills[RC_OBS_SKILL_COUNT] = {
    SKILL_ATTACK, SKILL_STRENGTH, SKILL_DEFENCE, SKILL_HITPOINTS, SKILL_RANGED,
    SKILL_PRAYER, SKILL_MAGIC, SKILL_WOODCUTTING, SKILL_MINING, SKILL_FISHING,
    SKILL_FIREMAKING, SKILL_COOKING, SKILL_SMITHING,
};
static const int prayers[RC_ACT_PRAYERS] = {
    RC_PRAYER_THICK_SKIN, RC_PRAYER_BURST_OF_STRENGTH, RC_PRAYER_CLARITY_OF_THOUGHT,
    RC_PRAYER_PROTECT_ITEM, RC_PRAYER_ROCK_SKIN, RC_PRAYER_PROTECT_FROM_MAGIC,
    RC_PRAYER_PROTECT_FROM_MISSILES, RC_PRAYER_PROTECT_FROM_MELEE,
};
static const char *const spells[RC_ACT_SPELLS] = {
    "Wind Strike", "Water Strike", "Earth Strike", "Fire Strike",
    "Wind Bolt", "Water Bolt", "Earth Bolt", "Fire Bolt",
};
static const int move_dirs[8][2] = {
    {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1},
};
static const int move_distances[RC_ACT_MOVE_DISTANCES] = {1, 5, 15};

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static float log_scale(double v, double top) {
    return v > 0 ? clampf((float)(log1p(v) / top), 0.0f, 1.0f) : 0.0f;
}

static int chebyshev(int ax, int ay, int bx, int by) {
    int dx = abs(ax - bx), dy = abs(ay - by);
    return dx > dy ? dx : dy;
}

// ---- Item features ---------------------------------------------------------------

enum {
    CAT_COINS = 0, CAT_FOOD, CAT_RAW_FOOD, CAT_LOGS, CAT_ORE, CAT_BAR, CAT_AXE,
    CAT_PICKAXE, CAT_FISHING, CAT_TOOL, CAT_MELEE, CAT_RANGED, CAT_MAGIC,
    CAT_ARMOUR, CAT_BONES, CAT_OTHER, CAT_COUNT,
};

typedef struct {
    uint8_t category, tier;  // tier: 0-255 (requirement level, heal, ...)
    int8_t primary_op;       // inventory option of the item's main action, or one of:
} ItemInfo;
enum { OP_NONE = -1, OP_EQUIP = RC_ITEM_ACTION_COUNT };

static ItemInfo item_info[RC_MAX_ITEM_DEFS];
static uint8_t npc_options[RC_MAX_NPC_DEFS_ENV];  // bit 0: option 0 handled, bit 1: option 2
static int spell_ids[RC_ACT_SPELLS];
static pthread_once_t item_info_once = PTHREAD_ONCE_INIT;
static const RcWorld *item_info_world;

static int name_has(const char *name, const char *part) {
    return name && strstr(name, part) != NULL;
}

static int tool_level(const RcSkillTool *tools, int count, int item_id) {
    for (int i = 0; i < count; i++)
        if ((int)tools[i].item_id == item_id) return tools[i].level;
    return -1;
}

static ItemInfo classify_item(const RcWorld *w, int id) {
    const RcItemDef *def = rc_item_def_get(id);
    const RcSkillingRules *r = w->skilling;
    ItemInfo info = {CAT_OTHER, 0};
    if (!def || !def->loaded) return info;
    int level = 0;
    const RcConsumableDef *food = rc_consumable_get(w, id);
    const RcCookRule *cook = rc_skilling_cook_rule(w, id);
    const RcFireRule *fire = rc_skilling_fire_rule(w, id);
    if (id == COINS) {
        info.category = CAT_COINS;
    } else if (name_has(def->name, "ones") && strcmp(def->inventory_actions[0], "Bury") == 0) {
        info.category = CAT_BONES;
    } else if (food) {
        info.category = CAT_FOOD;
        level = food->heal * 4;
    } else if (cook) {
        info.category = CAT_RAW_FOOD;
        level = cook->level;
    } else if (fire) {
        info.category = CAT_LOGS;
        level = fire->level;
    } else if (r && (level = tool_level(r->axes, r->axe_count, id)) >= 0) {
        info.category = CAT_AXE;
    } else if (r && (level = tool_level(r->pickaxes, r->pickaxe_count, id)) >= 0) {
        info.category = CAT_PICKAXE;
    } else if (id == TINDERBOX || id == HAMMER) {
        info.category = CAT_TOOL;
        level = 0;
    } else {
        level = 0;
        for (int i = 0; r && i < r->gather_count; i++)
            if ((int)r->gather[i].product == id && r->gather[i].skill == SKILL_MINING) {
                info.category = CAT_ORE;
                level = r->gather[i].level;
            }
        for (int i = 0; r && i < r->fish_count; i++)
            if ((int)r->fish[i].tool == id || (int)r->fish[i].bait == id)
                info.category = CAT_FISHING;
        for (int i = 0; i < w->make_option_count; i++) {
            const RcRecipe *recipe = rc_recipe_get(w->make_options[i].recipe);
            if (recipe && (int)recipe->output_item == id
                    && w->make_options[i].facility == RC_MAKE_MENU_FURNACE) {
                info.category = CAT_BAR;
                level = recipe->reqs[0].level;
            }
        }
        if (info.category == CAT_OTHER && def->equippable) {
            for (int i = 0; i < def->req_count && i < RC_ITEM_MAX_REQUIREMENTS; i++)
                if (def->req_level[i] > level) level = def->req_level[i];
            if (def->equip_slot == EQUIP_WEAPON)
                info.category = name_has(def->name, "bow") ? CAT_RANGED
                              : name_has(def->name, "taff") ? CAT_MAGIC : CAT_MELEE;
            else if (def->equip_slot == EQUIP_AMMO)
                info.category = CAT_RANGED;
            else
                info.category = CAT_ARMOUR;
        } else if (info.category == CAT_OTHER && name_has(def->name, " rune")) {
            info.category = CAT_MAGIC;
        }
    }
    info.tier = (uint8_t)(level < 0 ? 0 : level > 255 ? 255 : level);
    // Main inventory action: eat/drink, a content handler (bury), or equip.
    info.primary_op = OP_NONE;
    for (int op = 0; op < RC_ITEM_ACTION_COUNT && info.primary_op == OP_NONE; op++) {
        RcInteractionDispatchKey key = rc_interaction_dispatch_key_any();
        key.kind = RC_INTERACTION_INVENTORY_ITEM;
        key.op = rc_interaction_op_from_option(op);
        key.definition_id = id;
        if (rc_consumable_action(id, op)
                || (def->inventory_actions[op][0]
                    && rc_interaction_has_specific_world_handler(w, &key)))
            info.primary_op = (int8_t)op;
    }
    if (info.primary_op == OP_NONE && def->equippable && def->equipable_by_player)
        info.primary_op = OP_EQUIP;
    return info;
}

static int npc_option_handled_now(const RcWorld *w, int def_id, int option) {
    RcInteractionDispatchKey key = rc_interaction_dispatch_key_any();
    key.kind = RC_INTERACTION_NPC;
    key.op = rc_interaction_op_from_option(option);
    key.definition_id = def_id;
    return rc_interaction_has_specific_world_handler(w, &key);
}

// Content registers the same handlers in every gym world, so one world's
// tables serve them all.
static void build_item_info(void) {
    const RcWorld *w = item_info_world;
    for (int id = 0; id < RC_MAX_ITEM_DEFS; id++) item_info[id] = classify_item(w, id);
    for (int i = 0; i < w->interaction_handler_count; i++) {
        const RcInteractionDispatchKey *k = &w->interaction_handlers[i].key;
        if (k->kind != RC_INTERACTION_NPC || k->definition_id < 0
                || k->definition_id >= RC_MAX_NPC_DEFS_ENV)
            continue;
        if (npc_option_handled_now(w, k->definition_id, 0)) npc_options[k->definition_id] |= 1;
        if (npc_option_handled_now(w, k->definition_id, 2)) npc_options[k->definition_id] |= 2;
    }
    for (int i = 0; i < RC_ACT_SPELLS; i++) spell_ids[i] = rc_spell_find(spells[i]);
}

static ItemInfo info_of(int id) {
    return id >= 0 && id < RC_MAX_ITEM_DEFS ? item_info[id] : (ItemInfo){CAT_OTHER, 0};
}

static void item_features(int id, int quantity, float *out) {
    memset(out, 0, RC_OBS_ITEM * sizeof(float));
    if (id < 0 || quantity <= 0) return;
    ItemInfo info = info_of(id);
    out[0] = 1.0f;
    out[1 + info.category] = 1.0f;
    out[17] = log_scale(quantity, 10.0);
    out[18] = log_scale(rc_env_net_worth_item_value(id), 12.0);
    out[19] = info.tier / 99.0f;
}

static int is_equipment(int category) {
    return category == CAT_MELEE || category == CAT_RANGED || category == CAT_MAGIC
        || category == CAT_ARMOUR;
}

static int is_resource(int category) {
    return category == CAT_LOGS || category == CAT_ORE || category == CAT_BAR
        || category == CAT_RAW_FOOD;
}

// ---- Object index -------------------------------------------------------------------

static int action_kind(const char *a) {
    static const char *const traversal[] = {
        "Climb-up", "Climb-down", "Climb", "Pull", "Enter", "Exit", "Go-up",
        "Go-down", "Walk-up", "Walk-down", "Cross", "Squeeze-through", "Climb-over",
    };
    if (!a || !a[0]) return -1;
    if (!strcmp(a, "Chop down")) return RC_INTERACT_KIND_TREE;
    if (!strcmp(a, "Mine")) return RC_INTERACT_KIND_ROCK;
    if (!strcmp(a, "Cook")) return RC_INTERACT_KIND_COOK;
    if (!strcmp(a, "Smelt")) return RC_INTERACT_KIND_FURNACE;
    if (!strcmp(a, "Smith")) return RC_INTERACT_KIND_ANVIL;
    if (!strcmp(a, "Bank")) return RC_INTERACT_KIND_BANK;
    if (!strcmp(a, "Pray-at") || !strcmp(a, "Pray")) return RC_INTERACT_KIND_ALTAR;
    for (int i = 0; i < COUNT(traversal); i++)
        if (!strcmp(a, traversal[i])) return RC_INTERACT_KIND_TRAVERSAL;
    return -1;
}

// Kind and usable options (bits 0-2) of an object, or -1 when agents cannot use it.
static int classify_object(const RcWorld *w, int obj_id, int x, int y, int plane,
                           uint64_t key, uint8_t *ops) {
    const RcObjectDef *def = rc_object_def_get(obj_id);
    int kind = -1;
    *ops = 0;
    for (int op = 0; def && op < 3; op++) {
        int k = action_kind(def->actions[op]);
        if (k < 0 || !rc_world_object_option_supported(w, obj_id, x, y, plane, key, op))
            continue;
        if (kind < 0) kind = k;
        *ops |= (uint8_t)(1u << op);
    }
    return kind;
}

int rc_env_index_objects(RcEnv *env) {
    RcWorld *w = env->world;
    item_info_world = w;
    pthread_once(&item_info_once, build_item_info);
    int cap = 256;
    env->objects = malloc((size_t)cap * sizeof(*env->objects));
    env->object_count = 0;
    if (!env->objects) return 0;
    for (int rx = env->layout.min_rx; rx <= env->layout.max_rx; rx++) {
        for (int ry = env->layout.min_ry; ry <= env->layout.max_ry; ry++) {
            int count = 0;
            const RcObjectPlacement *rows = rc_object_region_placements(
                (uint16_t)((rx << 8) | ry), &count);
            for (int i = 0; rows && i < count; i++) {
                const RcObjectPlacement *o = &rows[i];
                uint8_t ops;
                int kind = classify_object(w, (int)o->obj_id, o->x, o->y, o->plane, o->key, &ops);
                if (kind < 0) continue;
                if (env->object_count == cap) {
                    cap *= 2;
                    RcInteractable *grown = realloc(env->objects, (size_t)cap * sizeof(*grown));
                    if (!grown) return 0;
                    env->objects = grown;
                }
                env->objects[env->object_count++] = (RcInteractable){
                    (int)o->obj_id, o->x, o->y, o->plane, o->key, (uint8_t)kind, ops,
                };
            }
        }
    }
    return 1;
}

// ---- Views ---------------------------------------------------------------------------

typedef struct {
    int dist, index;
} Ranked;

static void rank_insert(Ranked *best, int *count, int cap, int dist, int index) {
    if (*count == cap && dist >= best[cap - 1].dist) return;
    int i = *count < cap ? (*count)++ : cap - 1;
    while (i > 0 && best[i - 1].dist > dist) {
        best[i] = best[i - 1];
        i--;
    }
    best[i] = (Ranked){dist, index};
}

// Fires players lit are object states, not placements.
static int fire_candidates(const RcWorld *w, const RcPlayer *p, RcInteractable *out, int cap) {
    int n = 0;
    for (int i = 0; i < w->object_state_count && n < cap; i++) {
        const RcObjectState *s = &w->object_states[i];
        if (!(s->flags & RC_OBJECT_STATE_SPAWNED) || s->active_obj_id < 0
                || s->active_plane != p->plane
                || !rc_skilling_is_cook_object(w, s->active_obj_id)
                || chebyshev(s->active_x, s->active_y, p->x, p->y) > NEAR_RANGE)
            continue;
        out[n++] = (RcInteractable){s->active_obj_id, s->active_x, s->active_y,
                                    s->active_plane, s->placement_key, RC_INTERACT_KIND_COOK, 1};
    }
    return n;
}

static void refresh_view(RcEnv *env, int agent) {
    RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    RcAgentView *v = &env->view[agent];
    memset(v, 0, sizeof(*v));

    Ranked best[RC_OBS_NEAR_GROUND > RC_OBS_NEAR_NPCS ? RC_OBS_NEAR_GROUND : RC_OBS_NEAR_NPCS];
    int n = 0;
    for (int i = 0; i < w->npc_count; i++) {
        const RcNpc *npc = &w->npcs[i];
        if (!npc->active || npc->is_dead || npc->plane != p->plane) continue;
        int d = chebyshev(npc->x, npc->y, p->x, p->y);
        if (d <= NEAR_RANGE) rank_insert(best, &n, RC_OBS_NEAR_NPCS, d, i);
    }
    for (int i = 0; i < n; i++) v->npc_uid[i] = w->npcs[best[i].index].uid;
    v->npc_count = n;

    RcInteractable fires[8];
    int fire_count = fire_candidates(w, p, fires, COUNT(fires));
    Ranked near_objects[RC_OBS_NEAR_OBJECTS];
    n = 0;
    for (int i = 0; i < env->object_count + fire_count; i++) {
        const RcInteractable *o = i < env->object_count ? &env->objects[i]
                                                        : &fires[i - env->object_count];
        if (o->plane != p->plane) continue;
        int d = chebyshev(o->x, o->y, p->x, p->y);
        if (d > NEAR_RANGE) continue;
        if (o->kind == RC_INTERACT_KIND_TREE || o->kind == RC_INTERACT_KIND_ROCK) {
            // A gathering node has an object state only while it is depleted.
            const RcObjectState *state = rc_world_object_state_find_key_const(w, o->key);
            if (state && state->active_obj_id != o->obj_id) continue;
        }
        rank_insert(near_objects, &n, RC_OBS_NEAR_OBJECTS, d, i);
    }
    for (int i = 0; i < n; i++) {
        int idx = near_objects[i].index;
        v->objects[i] = idx < env->object_count ? env->objects[idx]
                                                : fires[idx - env->object_count];
    }
    v->object_count = n;

    n = 0;
    for (int i = 0; i < w->ground_item_count; i++) {
        const RcGroundItem *g = &w->ground_items[i];
        if (!g->active || g->plane != p->plane || !rc_ground_item_visible(g, agent)) continue;
        int d = chebyshev(g->x, g->y, p->x, p->y);
        if (d <= NEAR_RANGE) rank_insert(best, &n, RC_OBS_NEAR_GROUND, d, i);
    }
    for (int i = 0; i < n; i++) {
        v->ground[i] = best[i].index;
        v->ground_uid[i] = w->ground_items[best[i].index].uid;
    }
    v->ground_count = n;

    Ranked near_players[RC_OBS_NEAR_PLAYERS];
    n = 0;
    for (int i = 0; i < w->player_count; i++) {
        const RcPlayer *o = &w->players[i];
        if (i == agent || !o->active || o->plane != p->plane) continue;
        int d = chebyshev(o->x, o->y, p->x, p->y);
        if (d <= NEAR_RANGE) rank_insert(near_players, &n, RC_OBS_NEAR_PLAYERS, d, i);
    }
    for (int i = 0; i < n; i++) v->players[i] = near_players[i].index;
    v->player_count = n;
    for (int i = 0; i < RC_BANK_SIZE && v->bank_count < RC_OBS_BANK_SLOTS; i++)
        if (p->bank[i].item_id >= 0 && p->bank[i].quantity > 0) v->bank_slot[v->bank_count++] = i;
}

void rc_env_refresh_views(RcEnv *env) {
    if (!env || env->view_fresh) return;
    for (int i = 0; i < env->cfg.num_agents; i++) refresh_view(env, i);
    env->view_fresh = true;
}

// ---- Shared predicates (observation and mask) ------------------------------------

static int inventory_count(const RcPlayer *p, int item_id) {
    int n = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        if (p->inventory[i].item_id == item_id) n += p->inventory[i].quantity;
    return n;
}

static int free_slots(const RcPlayer *p) {
    int n = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) n += p->inventory[i].item_id < 0;
    return n;
}

static int can_hold(const RcPlayer *p, int item_id) {
    const RcItemDef *def = rc_item_def_get(item_id);
    return free_slots(p) > 0
        || (def && def->stackable && rc_inv_find(p->inventory, item_id) >= 0);
}

static int has_cookable(const RcWorld *w, const RcPlayer *p) {
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) {
        const RcCookRule *rule = p->inventory[i].item_id >= 0
            ? rc_skilling_cook_rule(w, p->inventory[i].item_id) : NULL;
        if (rule && p->skills.boosted_level[SKILL_COOKING] >= rule->level) return 1;
    }
    return 0;
}

// Whether the acting player could make anything at a furnace or anvil now.
static int can_make_at(const RcWorld *w, int facility) {
    for (int i = 0; i < w->make_option_count; i++)
        if (w->make_options[i].facility == facility
                && rc_recipe_player_can_make(w, rc_recipe_get(w->make_options[i].recipe)))
            return 1;
    return 0;
}

static int object_usable(const RcWorld *w, const RcPlayer *p, const RcInteractable *o) {
    switch (o->kind) {
    case RC_INTERACT_KIND_TREE:
    case RC_INTERACT_KIND_ROCK: {
        const RcGatherRule *rule = rc_skilling_gather_rule(w, o->obj_id);
        return rule && p->skills.boosted_level[rule->skill] >= rule->level
            && rc_skilling_best_tool(w, p, rule->skill) && free_slots(p) > 0;
    }
    case RC_INTERACT_KIND_COOK: return has_cookable(w, p);
    case RC_INTERACT_KIND_FURNACE: return can_make_at(w, RC_MAKE_MENU_FURNACE);
    case RC_INTERACT_KIND_ANVIL:
        return inventory_count(p, HAMMER) > 0 && can_make_at(w, RC_MAKE_MENU_ANVIL);
    default: return 1;
    }
}

static int npc_option_handled(int def_id, int option) {
    return def_id >= 0 && def_id < RC_MAX_NPC_DEFS_ENV
        && (npc_options[def_id] & (option == 0 ? 1 : 2));
}

static int item_primary_op(const RcWorld *w, const RcPlayer *p, int slot) {
    (void)w;
    int id = p->inventory[slot].item_id;
    if (id < 0 || id >= RC_MAX_ITEM_DEFS) return -1;
    int op = item_info[id].primary_op;
    if (op != OP_EQUIP) return op;
    const RcItemDef *def = rc_item_def_get(id);
    for (int i = 0; def && i < def->req_count && i < RC_ITEM_MAX_REQUIREMENTS; i++) {
        int skill = def->req_skill[i];
        if (skill >= 0 && skill < SKILL_COUNT && p->skills.base_level[skill] < def->req_level[i])
            return -1;
    }
    return def ? OP_EQUIP : -1;
}

static int is_staff(const RcPlayer *p) {
    const RcItemDef *def = p->equipment[EQUIP_WEAPON].item_id >= 0
        ? rc_item_def_get(p->equipment[EQUIP_WEAPON].item_id) : NULL;
    return def && name_has(def->name, "taff");
}

// ---- Observation ---------------------------------------------------------------------

static void observe_self(const RcEnv *env, int agent, float *o) {
    const RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    int dungeon = p->x >= 512;
    o[0] = dungeon ? (p->x - 512) / 128.0f : (p->x - 128) / 192.0f;
    o[1] = (p->y - 128) / (dungeon ? 128.0f : 192.0f);
    o[2] = (float)dungeon;
    o[3] = p->plane / 3.0f;
    int wild = rc_wilderness_level_at(p->x, p->y, p->plane);
    o[4] = wild / 10.0f;
    o[5] = wild > 0;
    o[6] = (rc_area_flags_at(p->x, p->y, p->plane) & 1u) != 0;
    o[7] = p->max_hp > 0 ? (float)p->current_hp / p->max_hp : 0.0f;
    o[8] = p->current_hp / 990.0f;
    int prayer_max = p->skills.base_level[SKILL_PRAYER] * 10;
    o[9] = prayer_max > 0 ? (float)p->current_prayer_points / prayer_max : 0.0f;
    o[10] = p->current_prayer_points / 990.0f;
    o[11] = p->run_energy / 10000.0f;
    o[12] = p->running;
    o[13] = p->special_energy / 10000.0f;
    o[14] = p->is_dead;
    o[15] = p->toxin_severity > 0;
    o[16] = p->attack_target >= 0;
    for (int i = 0; i < w->npc_count; i++)
        if (w->npcs[i].active && !w->npcs[i].is_dead && w->npcs[i].target_uid == agent) {
            o[17] = 1.0f;
            break;
        }
    o[18] = clampf(p->attack_timer / 10.0f, 0.0f, 1.0f);
    if (p->attack_style_idx >= 0 && p->attack_style_idx < 4) o[19 + p->attack_style_idx] = 1.0f;
    o[23] = p->auto_retaliate;
    int job = p->skill_action ? p->skill_job.kind : 0;
    o[24 + (job >= 0 && job < 6 ? job : 0)] = 1.0f;
    o[30 + (p->make_menu < 3 ? p->make_menu : 0)] = 1.0f;
    o[33] = p->shop_open >= 0;
    o[34] = p->storage_kind == RC_STORAGE_BANK;
    o[35] = free_slots(p) / 28.0f;
    o[36] = log_scale(inventory_count(p, COINS), 15.0);
    double carried = 0, banked = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        carried += (double)rc_env_net_worth_item_value(p->inventory[i].item_id) * p->inventory[i].quantity;
    for (int i = 0; i < RC_EQUIP_COUNT; i++)
        carried += (double)rc_env_net_worth_item_value(p->equipment[i].item_id) * p->equipment[i].quantity;
    const RcAgentView *view = &env->view[agent];
    for (int i = 0; i < view->bank_count; i++) {
        const RcInvSlot *b = &p->bank[view->bank_slot[i]];
        banked += (double)rc_env_net_worth_item_value(b->item_id) * b->quantity;
    }
    o[37] = log_scale(carried, 15.0);
    o[38] = log_scale(banked, 15.0);
    o[39] = rc_combat_level(&p->skills) / 126.0f;
    int total = 0;
    for (int i = 0; i < RC_OBS_SKILL_COUNT; i++) total += p->skills.base_level[tracked_skills[i]];
    o[40] = total / (99.0f * RC_OBS_SKILL_COUNT);
    o[41] = p->route_idx < p->route_len;
    o[42] = p->interaction.active;
    int active = 0;
    for (int i = 0; i < 32; i++) active += (p->active_prayers >> i) & 1;
    o[43] = active / 5.0f;
    o[44] = (p->active_prayers >> RC_PRAYER_PROTECT_ITEM) & 1;
    o[45] = (p->active_prayers >> RC_PRAYER_PROTECT_FROM_MAGIC) & 1;
    o[46] = (p->active_prayers >> RC_PRAYER_PROTECT_FROM_MISSILES) & 1;
    o[47] = (p->active_prayers >> RC_PRAYER_PROTECT_FROM_MELEE) & 1;
}

static void observe_npcs(const RcEnv *env, int agent, float *o) {
    const RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    const RcAgentView *v = &env->view[agent];
    for (int k = 0; k < v->npc_count; k++, o += RC_OBS_NPC) {
        const RcNpc *npc = rc_npc_resolve_const(w, (RcNpcId)v->npc_uid[k]);
        const RcNpcDef *def = npc ? rc_npc_base_def_for_npc(npc) : NULL;
        if (!npc || !def) continue;
        o[0] = 1.0f;
        o[1] = (npc->x - p->x) / (float)NEAR_RANGE;
        o[2] = (npc->y - p->y) / (float)NEAR_RANGE;
        o[3] = chebyshev(npc->x, npc->y, p->x, p->y) / (float)NEAR_RANGE;
        o[4] = def->hitpoints > 0 ? clampf((float)npc->current_hp / def->hitpoints, 0, 1) : 0;
        o[5] = def->combat_level > 0 ? def->combat_level / 300.0f : 0.0f;
        o[6] = npc->target_uid == agent;
        o[7] = p->attack_target == npc->uid;
        int attackable = 0;
        for (int op = 0; op < RC_NPC_OPTION_COUNT; op++)
            attackable |= rc_npc_def_option_is_attack(def, op);
        o[8] = attackable;
        o[9] = def->id == 239 || def->id == 2054;
        o[10] = rc_skilling_fish_rule(w, def->id, 0) != NULL;
        o[11] = rc_shop_for_npc(w, def->id) >= 0;
        o[12] = clampf(def->max_hit / 50.0f, 0, 1);
        o[13] = def->hunt.range > 0;
        o[14] = npc->target_uid >= 0 && npc->target_uid != agent;
        o[15] = def->size / 5.0f;
    }
}

static void observe_objects(const RcEnv *env, int agent, float *o) {
    const RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    const RcAgentView *v = &env->view[agent];
    for (int k = 0; k < v->object_count; k++, o += RC_OBS_OBJECT) {
        const RcInteractable *obj = &v->objects[k];
        o[0] = 1.0f;
        o[1] = (obj->x - p->x) / (float)NEAR_RANGE;
        o[2] = (obj->y - p->y) / (float)NEAR_RANGE;
        o[3] = chebyshev(obj->x, obj->y, p->x, p->y) / (float)NEAR_RANGE;
        o[4 + obj->kind] = 1.0f;
        const RcGatherRule *rule = rc_skilling_gather_rule(w, obj->obj_id);
        o[12] = rule ? rule->level / 99.0f : 0.0f;
        o[13] = object_usable(w, p, obj);
        o[14] = obj->kind == RC_INTERACT_KIND_COOK && w->skilling
             && obj->obj_id == (int)w->skilling->fire_obj_id;
        o[15] = ((obj->ops >> 1) & 1) + ((obj->ops >> 2) & 1) * 0.5f;
    }
}

static void observe_ground(const RcEnv *env, int agent, float *o) {
    const RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    const RcAgentView *v = &env->view[agent];
    for (int k = 0; k < v->ground_count; k++, o += RC_OBS_GROUND) {
        const RcGroundItem *g = &w->ground_items[v->ground[k]];
        if (!g->active || g->uid != v->ground_uid[k]) continue;
        int cat = info_of(g->item_id).category;
        o[0] = 1.0f;
        o[1] = (g->x - p->x) / (float)NEAR_RANGE;
        o[2] = (g->y - p->y) / (float)NEAR_RANGE;
        o[3] = chebyshev(g->x, g->y, p->x, p->y) / (float)NEAR_RANGE;
        o[4] = g->owner_uid == agent;
        o[5] = log_scale((double)rc_env_net_worth_item_value(g->item_id) * g->quantity, 12.0);
        o[6] = log_scale(g->quantity, 10.0);
        o[7] = cat == CAT_FOOD;
        o[8] = cat == CAT_BONES;
        o[9] = cat == CAT_COINS;
        const RcItemDef *def = rc_item_def_get(g->item_id);
        o[10] = (cat == CAT_MAGIC || cat == CAT_RANGED) && def && def->stackable;  // runes, arrows
        o[11] = is_equipment(cat);
        o[12] = is_resource(cat);
    }
}

static void observe_players(const RcEnv *env, int agent, float *o) {
    const RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    const RcAgentView *v = &env->view[agent];
    for (int k = 0; k < v->player_count; k++, o += RC_OBS_PLAYER) {
        const RcPlayer *other = &w->players[v->players[k]];
        o[0] = 1.0f;
        o[1] = (other->x - p->x) / (float)NEAR_RANGE;
        o[2] = (other->y - p->y) / (float)NEAR_RANGE;
        o[3] = chebyshev(other->x, other->y, p->x, p->y) / (float)NEAR_RANGE;
        o[4] = other->max_hp > 0 ? (float)other->current_hp / other->max_hp : 0.0f;
        o[5] = rc_combat_level(&other->skills) / 126.0f;
        o[6] = other->attack_target >= 0;
        o[7] = other->is_dead;
    }
}

void rc_env_observe(RcEnv *env, int agent, float *obs) {
    if (!env || !obs || agent < 0 || agent >= env->cfg.num_agents) return;
    rc_env_refresh_views(env);
    memset(obs, 0, RC_ENV_OBS_SIZE * sizeof(float));
    RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    RcPlayer *saved = w->player;
    w->player = &w->players[agent];  // recipe checks read the acting player
    observe_self(env, agent, obs + RC_OBS_SELF);

    float *o = obs + RC_OBS_SKILLS;
    for (int i = 0; i < RC_OBS_SKILL_COUNT; i++, o += 3) {
        int s = tracked_skills[i], level = p->skills.base_level[s];
        o[0] = level / 99.0f;
        o[1] = p->skills.boosted_level[s] / 99.0f;
        if (level >= 1 && level < 99) {
            int lo = RC_XP_TABLE[level - 1], hi = RC_XP_TABLE[level];
            o[2] = hi > lo ? clampf((float)(p->skills.xp[s] - lo) / (hi - lo), 0, 1) : 0;
        }
    }
    for (int i = 0; i < 14; i++)
        obs[RC_OBS_BONUSES + i] = clampf(p->equipment_bonuses[i] / 200.0f, -1, 1);
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        item_features(p->inventory[i].item_id, p->inventory[i].quantity,
                      obs + RC_OBS_INVENTORY + i * RC_OBS_ITEM);
    for (int i = 0; i < RC_EQUIP_COUNT; i++) {
        float *e = obs + RC_OBS_EQUIPMENT + i * 3;
        int id = p->equipment[i].item_id;
        if (id < 0 || p->equipment[i].quantity <= 0) continue;
        e[0] = 1.0f;
        e[1] = info_of(id).tier / 99.0f;
        e[2] = log_scale(rc_env_net_worth_item_value(id), 12.0);
    }
    observe_npcs(env, agent, obs + RC_OBS_NPCS);
    observe_objects(env, agent, obs + RC_OBS_OBJECTS);
    observe_ground(env, agent, obs + RC_OBS_GROUND_ITEMS);
    observe_players(env, agent, obs + RC_OBS_PLAYERS);

    o = obs + RC_OBS_LANDMARK;
    for (int i = 0; i < env->layout.landmark_count && i < RC_OBS_LANDMARKS; i++, o += 4) {
        const RcGymLandmark *lm = &env->layout.landmarks[i];
        int walk = rc_env_travel_distance(i, p->x, p->y, p->plane);
        o[0] = clampf((lm->x - p->x) / 64.0f, -1, 1);
        o[1] = clampf((lm->y - p->y) / 64.0f, -1, 1);
        o[2] = walk >= 0 ? clampf(walk / 256.0f, 0, 1) : 1.0f;
        o[3] = walk >= 0;
    }

    o = obs + RC_OBS_MAP;
    for (int dy = RC_OBS_MAP_RADIUS; dy >= -RC_OBS_MAP_RADIUS; dy--)
        for (int dx = -RC_OBS_MAP_RADIUS; dx <= RC_OBS_MAP_RADIUS; dx++)
            *o++ = !rc_tile_blocked(&w->map, p->x + dx, p->y + dy, p->plane);

    o = obs + RC_OBS_SHOP;
    if (p->shop_open >= 0 && p->shop_open < w->shop_count) {
        const RcShopState *shop = &w->shops[p->shop_open];
        int coins = inventory_count(p, COINS);
        for (int k = 0; k < shop->slot_count && k < RC_OBS_SHOP_SLOTS; k++) {
            int price = rc_shop_buy_price(w, p->shop_open, k);
            o[k * 3] = shop->slots[k].stock > 0;
            o[k * 3 + 1] = log_scale(price, 12.0);
            o[k * 3 + 2] = coins >= price;
        }
        o[RC_OBS_SHOP_SLOTS * 3 + (p->shop_open < 3 ? p->shop_open : 2)] = 1.0f;
    } else {
        o[RC_OBS_SHOP_SLOTS * 3 + 3] = 1.0f;
    }

    o = obs + RC_OBS_BANK;
    const RcAgentView *v = &env->view[agent];
    for (int k = 0; k < v->bank_count; k++, o += 8) {
        const RcInvSlot *s = &p->bank[v->bank_slot[k]];
        int cat = info_of(s->item_id).category;
        o[0] = 1.0f;
        o[1] = log_scale(rc_env_net_worth_item_value(s->item_id), 12.0);
        o[2] = log_scale(s->quantity, 10.0);
        o[3] = cat == CAT_FOOD;
        o[4] = is_resource(cat);
        o[5] = is_equipment(cat);
        o[6] = cat == CAT_AXE || cat == CAT_PICKAXE || cat == CAT_FISHING || cat == CAT_TOOL;
        o[7] = !o[3] && !o[4] && !o[5] && !o[6];
    }

    o = obs + RC_OBS_TASK;
    if (env->cfg.task >= 0 && env->cfg.task < 10) o[env->cfg.task] = 1.0f;
    float elapsed = env->episode_ticks > 0 ? (float)w->tick / env->episode_ticks : 0.0f;
    o[10] = clampf(elapsed, 0, 1);
    o[11] = clampf(1.0f - elapsed, 0, 1);
    o[12] = agent / 16.0f;
    o[13] = env->cfg.num_agents / 16.0f;
    o[14] = env->task->death_ends;
    w->player = saved;
}

// ---- Masks ------------------------------------------------------------------------------

static int action_valid(RcEnv *env, int agent, int a, int can_smelt, int can_smith) {
    RcWorld *w = env->world;
    const RcPlayer *p = &w->players[agent];
    const RcAgentView *v = &env->view[agent];
    if (p->is_dead) return a == RC_ACT_NOOP;
    if (a == RC_ACT_NOOP) return 1;
    if (a < RC_ACT_TRAVEL) {
        int dir = (a - RC_ACT_MOVE) / RC_ACT_MOVE_DISTANCES;
        return rc_can_move(&w->map, p->x, p->y, move_dirs[dir][0], move_dirs[dir][1], p->plane);
    }
    if (a < RC_ACT_ATTACK) {
        int i = a - RC_ACT_TRAVEL;
        return i < env->layout.landmark_count
            && rc_env_travel_distance(i, p->x, p->y, p->plane) > 2;
    }
    if (a < RC_ACT_NPC_OP) {
        int k = a - RC_ACT_ATTACK;
        const RcNpc *npc = k < v->npc_count ? rc_npc_resolve_const(w, (RcNpcId)v->npc_uid[k]) : NULL;
        const RcNpcDef *def = npc ? rc_npc_base_def_for_npc(npc) : NULL;
        if (!npc || !def || npc->is_dead || p->attack_target == npc->uid) return 0;
        int attackable = 0;
        for (int op = 0; op < RC_NPC_OPTION_COUNT; op++) attackable |= rc_npc_def_option_is_attack(def, op);
        int multi = (rc_area_flags_at(p->x, p->y, p->plane) & 1u) != 0;
        return attackable && (multi || npc->target_uid < 0 || npc->target_uid == agent);
    }
    if (a < RC_ACT_OBJECT_OP) {
        int k = (a - RC_ACT_NPC_OP) / 2, option = (a - RC_ACT_NPC_OP) % 2 ? 2 : 0;
        const RcNpc *npc = k < v->npc_count ? rc_npc_resolve_const(w, (RcNpcId)v->npc_uid[k]) : NULL;
        const RcNpcDef *def = npc ? rc_npc_base_def_for_npc(npc) : NULL;
        if (!def || !npc_option_handled(def->id, option)) return 0;
        const RcFishRule *fish = rc_skilling_fish_rule(w, def->id, option);
        return !fish || (inventory_count(p, (int)fish->tool) > 0
                         && (!fish->bait || inventory_count(p, (int)fish->bait) > 0)
                         && free_slots(p) > 0
                         && p->skills.boosted_level[SKILL_FISHING]
                                >= fish->catches[fish->catch_count - 1].level);
    }
    if (a < RC_ACT_ITEM) {
        int k = (a - RC_ACT_OBJECT_OP) / 3, option = (a - RC_ACT_OBJECT_OP) % 3;
        if (k >= v->object_count || !(v->objects[k].ops & (1u << option))) return 0;
        const RcInteractable *o = &v->objects[k];
        if (o->kind == RC_INTERACT_KIND_FURNACE) return can_smelt;
        if (o->kind == RC_INTERACT_KIND_ANVIL) return can_smith && inventory_count(p, HAMMER) > 0;
        return object_usable(w, p, o);
    }
    if (a < RC_ACT_DROP) return item_primary_op(w, p, a - RC_ACT_ITEM) >= 0;
    if (a < RC_ACT_LIGHT) return p->inventory[a - RC_ACT_DROP].item_id >= 0;
    if (a < RC_ACT_UNEQUIP) {
        const RcFireRule *rule = rc_skilling_fire_rule(w, p->inventory[a - RC_ACT_LIGHT].item_id);
        return rule && inventory_count(p, TINDERBOX) > 0
            && p->skills.boosted_level[SKILL_FIREMAKING] >= rule->level;
    }
    if (a < RC_ACT_PRAYER) {
        int e = a - RC_ACT_UNEQUIP;
        return p->equipment[e].item_id >= 0 && can_hold(p, p->equipment[e].item_id);
    }
    if (a < RC_ACT_STYLE) {
        int id = prayers[a - RC_ACT_PRAYER];
        return (p->active_prayers & rc_prayer_bit(id)) || rc_prayer_available(w, id) == RC_PRAYER_OK;
    }
    if (a < RC_ACT_RUN)
        return a - RC_ACT_STYLE != p->attack_style_idx
            && rc_player_attack_style_available(&w->players[agent], a - RC_ACT_STYLE);
    if (a < RC_ACT_PICKUP) return p->running || p->run_energy > 0;
    if (a < RC_ACT_MAKE) {
        int k = a - RC_ACT_PICKUP;
        if (k >= v->ground_count) return 0;
        const RcGroundItem *g = &w->ground_items[v->ground[k]];
        return g->active && g->uid == v->ground_uid[k] && can_hold(p, g->item_id);
    }
    if (a < RC_ACT_SHOP_BUY) return rc_skilling_make_option_ready(w, a - RC_ACT_MAKE);
    if (a < RC_ACT_SHOP_SELL) {
        int k = (a - RC_ACT_SHOP_BUY) % RC_OBS_SHOP_SLOTS;
        if (p->shop_open < 0 || p->shop_open >= w->shop_count) return 0;
        const RcShopState *shop = &w->shops[p->shop_open];
        return k < shop->slot_count && shop->slots[k].stock > 0
            && inventory_count(p, COINS) >= rc_shop_buy_price(w, p->shop_open, k)
            && can_hold(p, shop->slots[k].item_id);
    }
    if (a < RC_ACT_DEPOSIT) {
        int id = p->inventory[a - RC_ACT_SHOP_SELL].item_id;
        return p->shop_open >= 0 && id >= 0 && rc_shop_sell_price(w, p->shop_open, id) >= 0;
    }
    int bank_open = p->storage_kind == RC_STORAGE_BANK;
    if (a < RC_ACT_DEPOSIT_ALL) return bank_open && p->inventory[a - RC_ACT_DEPOSIT].item_id >= 0;
    if (a == RC_ACT_DEPOSIT_ALL) return bank_open && free_slots(p) < RC_INVENTORY_SIZE;
    if (a < RC_ACT_CLOSE) {
        int k = a - RC_ACT_WITHDRAW;
        return bank_open && k < v->bank_count && can_hold(p, p->bank[v->bank_slot[k]].item_id);
    }
    if (a == RC_ACT_CLOSE) return bank_open || p->shop_open >= 0 || p->make_menu;
    if (a < RC_ENV_NUM_ACTIONS) {
        int spell = spell_ids[a - RC_ACT_AUTOCAST];
        const RcSpellDef *def = spell >= 0 ? rc_spell_def_get(spell) : NULL;
        return def && is_staff(p) && p->autocast_spell != spell
            && p->skills.boosted_level[SKILL_MAGIC] >= def->level;
    }
    return 0;
}

void rc_env_action_mask(RcEnv *env, int agent, uint8_t *mask) {
    if (!env || !mask || agent < 0 || agent >= env->cfg.num_agents) return;
    rc_env_refresh_views(env);
    RcWorld *w = env->world;
    RcPlayer *saved = w->player;
    w->player = &w->players[agent];  // recipe checks read the acting player
    // Recipe checks are dear: only when a furnace or anvil is in view.
    const RcAgentView *v = &env->view[agent];
    int furnace = 0, anvil = 0;
    for (int k = 0; k < v->object_count; k++) {
        furnace |= v->objects[k].kind == RC_INTERACT_KIND_FURNACE;
        anvil |= v->objects[k].kind == RC_INTERACT_KIND_ANVIL;
    }
    int can_smelt = furnace && can_make_at(w, RC_MAKE_MENU_FURNACE);
    int can_smith = anvil && can_make_at(w, RC_MAKE_MENU_ANVIL);
    memset(mask, 0, RC_ENV_NUM_ACTIONS);
    mask[RC_ACT_NOOP] = 1;
    for (int a = 1; a < RC_ENV_NUM_ACTIONS; a++) {
        // Whole ranges that cannot apply skip their per-action checks.
        if (a == RC_ACT_MAKE && !w->player->make_menu) { a = RC_ACT_SHOP_BUY - 1; continue; }
        if (a == RC_ACT_SHOP_BUY && w->player->shop_open < 0) { a = RC_ACT_DEPOSIT - 1; continue; }
        if (a == RC_ACT_DEPOSIT && w->player->storage_kind != RC_STORAGE_BANK) { a = RC_ACT_CLOSE - 1; continue; }
        mask[a] = (uint8_t)action_valid(env, agent, a, can_smelt, can_smith);
    }
    w->player = saved;
}

// ---- Actions ----------------------------------------------------------------------------

void rc_env_apply_action(RcEnv *env, int agent, int a) {
    RcWorld *w = env->world;
    RcPlayer *p = w->player;
    const RcAgentView *v = &env->view[agent];
    if (a <= RC_ACT_NOOP || a >= RC_ENV_NUM_ACTIONS) return;
    if (a < RC_ACT_TRAVEL) {
        // Walk up to the distance in a straight line, stopping where it is blocked.
        int dir = (a - RC_ACT_MOVE) / RC_ACT_MOVE_DISTANCES;
        int dist = move_distances[(a - RC_ACT_MOVE) % RC_ACT_MOVE_DISTANCES];
        int x = p->x, y = p->y;
        for (int i = 0; i < dist && rc_can_move(&w->map, x, y, move_dirs[dir][0],
                                                move_dirs[dir][1], p->plane); i++) {
            x += move_dirs[dir][0];
            y += move_dirs[dir][1];
        }
        if (x != p->x || y != p->y) (void)rc_player_walk_to(w, x, y);
    } else if (a < RC_ACT_ATTACK) {
        // A hop of up to 12 tiles along the walking path to the landmark (short
        // hops keep the engine's route search small).
        int x, y;
        if (rc_env_travel_waypoint(&w->map, a - RC_ACT_TRAVEL, p->x, p->y, p->plane, 12, &x, &y)
                && (x != p->x || y != p->y))
            (void)rc_player_walk_to(w, x, y);
    } else if (a < RC_ACT_NPC_OP) {
        int k = a - RC_ACT_ATTACK;
        if (k < v->npc_count) (void)rc_player_attack_npc(w, v->npc_uid[k]);
    } else if (a < RC_ACT_OBJECT_OP) {
        int k = (a - RC_ACT_NPC_OP) / 2, option = (a - RC_ACT_NPC_OP) % 2 ? 2 : 0;
        if (k < v->npc_count) rc_player_interact_npc(w, v->npc_uid[k], option);
    } else if (a < RC_ACT_ITEM) {
        int k = (a - RC_ACT_OBJECT_OP) / 3, option = (a - RC_ACT_OBJECT_OP) % 3;
        if (k < v->object_count) {
            const RcInteractable *o = &v->objects[k];
            (void)rc_player_interact_object_placement(w, o->obj_id, o->x, o->y, o->plane,
                                                      o->key, option);
        }
    } else if (a < RC_ACT_DROP) {
        int slot = a - RC_ACT_ITEM, op = item_primary_op(w, p, slot);
        if (op == OP_EQUIP) (void)rc_player_equip(w, slot);
        else if (op >= 0) (void)rc_player_interact_inventory_item(w, slot, op);
    } else if (a < RC_ACT_LIGHT) {
        (void)rc_player_drop_item(w, a - RC_ACT_DROP);
    } else if (a < RC_ACT_UNEQUIP) {
        (void)rc_skilling_start_firemaking(w, a - RC_ACT_LIGHT);
    } else if (a < RC_ACT_PRAYER) {
        (void)rc_player_unequip(w, a - RC_ACT_UNEQUIP);
    } else if (a < RC_ACT_STYLE) {
        (void)rc_player_set_prayer(w, prayers[a - RC_ACT_PRAYER]);
    } else if (a < RC_ACT_RUN) {
        rc_player_set_attack_style(w, a - RC_ACT_STYLE);
    } else if (a < RC_ACT_PICKUP) {
        (void)rc_player_set_running(w, !p->running);
    } else if (a < RC_ACT_MAKE) {
        int k = a - RC_ACT_PICKUP;
        if (k < v->ground_count && w->ground_items[v->ground[k]].uid == v->ground_uid[k])
            (void)rc_player_pickup_item(w, v->ground[k]);
    } else if (a < RC_ACT_SHOP_BUY) {
        (void)rc_skilling_make(w, a - RC_ACT_MAKE, 0);
    } else if (a < RC_ACT_SHOP_BUY10) {
        (void)rc_shop_buy(w, a - RC_ACT_SHOP_BUY, 1);
    } else if (a < RC_ACT_SHOP_SELL) {
        (void)rc_shop_buy(w, a - RC_ACT_SHOP_BUY10, 10);
    } else if (a < RC_ACT_DEPOSIT) {
        int slot = a - RC_ACT_SHOP_SELL, id = p->inventory[slot].item_id;
        if (id >= 0) (void)rc_shop_sell(w, slot, inventory_count(p, id));
    } else if (a < RC_ACT_DEPOSIT_ALL) {
        (void)rc_bank_deposit_slot(w, a - RC_ACT_DEPOSIT, 0);
    } else if (a == RC_ACT_DEPOSIT_ALL) {
        (void)rc_bank_deposit_all(w, false);
    } else if (a < RC_ACT_CLOSE) {
        int k = a - RC_ACT_WITHDRAW;
        if (k < v->bank_count) (void)rc_bank_withdraw_slot(w, v->bank_slot[k], 0);
    } else if (a == RC_ACT_CLOSE) {
        rc_skilling_close_make(p);
        rc_shop_close(p);
        if (p->storage_kind) (void)rc_player_close_storage(w);
    } else {
        int spell = spell_ids[a - RC_ACT_AUTOCAST];
        if (spell >= 0) (void)rc_player_set_autocast_spell(w, spell, false);
    }
}

const char *rc_env_action_name(int a, char *buf, int cap) {
    static const char *const dirs[8] = {"n", "ne", "e", "se", "s", "sw", "w", "nw"};
    if (!buf || cap <= 0) return "";
    if (a == RC_ACT_NOOP) snprintf(buf, cap, "noop");
    else if (a < RC_ACT_TRAVEL)
        snprintf(buf, cap, "move %s %d", dirs[(a - RC_ACT_MOVE) / RC_ACT_MOVE_DISTANCES],
                 move_distances[(a - RC_ACT_MOVE) % RC_ACT_MOVE_DISTANCES]);
    else if (a < RC_ACT_ATTACK) snprintf(buf, cap, "travel %d", a - RC_ACT_TRAVEL);
    else if (a < RC_ACT_NPC_OP) snprintf(buf, cap, "attack npc %d", a - RC_ACT_ATTACK);
    else if (a < RC_ACT_OBJECT_OP)
        snprintf(buf, cap, "npc %d option %d", (a - RC_ACT_NPC_OP) / 2, (a - RC_ACT_NPC_OP) % 2 ? 2 : 0);
    else if (a < RC_ACT_ITEM)
        snprintf(buf, cap, "object %d option %d", (a - RC_ACT_OBJECT_OP) / 3, (a - RC_ACT_OBJECT_OP) % 3);
    else if (a < RC_ACT_DROP) snprintf(buf, cap, "use item %d", a - RC_ACT_ITEM);
    else if (a < RC_ACT_LIGHT) snprintf(buf, cap, "drop %d", a - RC_ACT_DROP);
    else if (a < RC_ACT_UNEQUIP) snprintf(buf, cap, "light %d", a - RC_ACT_LIGHT);
    else if (a < RC_ACT_PRAYER) snprintf(buf, cap, "unequip %d", a - RC_ACT_UNEQUIP);
    else if (a < RC_ACT_STYLE) snprintf(buf, cap, "prayer %d", prayers[a - RC_ACT_PRAYER]);
    else if (a < RC_ACT_RUN) snprintf(buf, cap, "style %d", a - RC_ACT_STYLE);
    else if (a < RC_ACT_PICKUP) snprintf(buf, cap, "toggle run");
    else if (a < RC_ACT_MAKE) snprintf(buf, cap, "pick up %d", a - RC_ACT_PICKUP);
    else if (a < RC_ACT_SHOP_BUY) snprintf(buf, cap, "make %d", a - RC_ACT_MAKE);
    else if (a < RC_ACT_SHOP_BUY10) snprintf(buf, cap, "buy 1 of %d", a - RC_ACT_SHOP_BUY);
    else if (a < RC_ACT_SHOP_SELL) snprintf(buf, cap, "buy 10 of %d", a - RC_ACT_SHOP_BUY10);
    else if (a < RC_ACT_DEPOSIT) snprintf(buf, cap, "sell %d", a - RC_ACT_SHOP_SELL);
    else if (a < RC_ACT_DEPOSIT_ALL) snprintf(buf, cap, "deposit %d", a - RC_ACT_DEPOSIT);
    else if (a == RC_ACT_DEPOSIT_ALL) snprintf(buf, cap, "deposit all");
    else if (a < RC_ACT_CLOSE) snprintf(buf, cap, "withdraw %d", a - RC_ACT_WITHDRAW);
    else if (a == RC_ACT_CLOSE) snprintf(buf, cap, "close");
    else if (a < RC_ENV_NUM_ACTIONS) snprintf(buf, cap, "autocast %s", spells[a - RC_ACT_AUTOCAST]);
    else snprintf(buf, cap, "invalid");
    return buf;
}
