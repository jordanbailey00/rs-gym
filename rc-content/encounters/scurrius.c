// Scurrius, a gym mode's shared boss (SPEC.md, "Scurrius as the shared advanced encounter";
// parameters in modes/<mode>/scurrius.toml through rc-core/mode.h). The fight follows the OSRS boss
// as RuneC's encounter notes describe it: tail swipes in melee, bolts of electricity and flying fur
// at range, a walk to a food pile to eat at 80% and every so often after, a final phase in the
// centre below 30% with faster ranged and magic attacks and more frequent falling bricks, bricks
// telegraphed on each player's tile and on random tiles, and screeches that summon giant rats.
// The mode adds group scaling over a monotonic participant set, kill credit by damage share, a
// seeded loot rotation among qualified players, respawn after a kill and an abandoned-fight reset.
//
// Runs as the world's mode tick (after the tick's damage and deaths resolve) and through the NPC
// combat hooks for the boss's style, range and attack speed.
#include "../content.h"
#include "../../rc-core/combat_hit.h"
#include "../../rc-core/interaction.h"
#include "../../rc-core/items.h"
#include "../../rc-core/mode.h"
#include "../../rc-core/npc.h"
#include "../../rc-core/objects.h"
#include "../../rc-core/pathfinding.h"
#include "../../rc-core/rng.h"

#include <string.h>

enum { CENTRE = 3, FOOD_PILE_A = 14205, FOOD_PILE_B = 14206 };

static const RcModeBoss *boss_rules(void) {
    const RcModeRules *m = rc_mode_active();
    return m && m->has_boss ? &m->boss : NULL;
}

static int in_arena(const RcModeBoss *b, int x, int y, int plane) {
    return plane == 0 && x >= b->rect[0] && x <= b->rect[2] && y >= b->rect[1] && y <= b->rect[3];
}

static RcNpc *find_boss(RcWorld *w, const RcModeBoss *b) {
    RcModeBossState *s = &w->mode_boss;
    RcNpc *npc = s->boss_uid >= 0 ? rc_npc_resolve(w, (RcNpcId)s->boss_uid) : NULL;
    const RcNpcDef *d = npc ? rc_npc_def_for_npc(w, npc) : NULL;
    if (npc && npc->active && d && d->id == b->npc) return npc;
    for (int i = 0; i < w->npc_count; i++) {
        d = w->npcs[i].active ? rc_npc_def_for_npc(w, &w->npcs[i]) : NULL;
        if (d && d->id == b->npc) {
            s->boss_uid = w->npcs[i].uid;
            return &w->npcs[i];
        }
    }
    s->boss_uid = -1;
    return NULL;
}

static int is_boss(const RcWorld *w, const RcNpc *npc) {
    const RcModeBoss *b = boss_rules();
    const RcNpcDef *d = b && npc ? rc_npc_def_for_npc(w, npc) : NULL;
    return d && d->id == b->npc;
}

// Living players inside the arena, as a slot mask.
static uint16_t occupants(const RcWorld *w, const RcModeBoss *b) {
    uint16_t mask = 0;
    for (int i = 0; i < w->player_count; i++) {
        const RcPlayer *p = &w->players[i];
        if (p->active && !p->is_dead && p->current_hp > 0 && in_arena(b, p->x, p->y, p->plane))
            mask |= (uint16_t)(1u << i);
    }
    return mask;
}

static int count_bits(uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

static void prune_minions(RcWorld *w) {
    RcModeBossState *s = &w->mode_boss;
    int n = 0;
    for (int i = 0; i < s->minion_count; i++) {
        const RcNpc *rat = rc_npc_resolve_const(w, (RcNpcId)s->minions[i]);
        if (rat && rat->active && !rat->is_dead) s->minions[n++] = s->minions[i];
    }
    s->minion_count = n;
}

// A random walkable arena tile (seeded), or 0.
static int random_tile(RcWorld *w, const RcModeBoss *b, int *x, int *y) {
    for (int tries = 0; tries < 24; tries++) {
        int tx = b->rect[0] + rc_rng_range(&w->rng_state, b->rect[2] - b->rect[0]);
        int ty = b->rect[1] + rc_rng_range(&w->rng_state, b->rect[3] - b->rect[1]);
        if (!rc_tile_blocked(&w->map, tx, ty, 0)) {
            *x = tx;
            *y = ty;
            return 1;
        }
    }
    return 0;
}

static void walk_boss(RcWorld *w, RcNpc *boss, int x, int y) {
    RcRouteTarget target = {.x = x, .y = y, .width = 1, .height = 1, .min_distance = 0,
                            .max_distance = 1};
    boss->owner_controlled = true;  // no attacks or chasing while it walks
    (void)rc_npc_route_request(w, boss, &target, RC_NPC_ROUTE_SCRIPTED, true);
}

static int boss_beside(const RcNpc *boss, int size, int x, int y) {
    int dx = x < boss->x ? boss->x - x : x > boss->x + size - 1 ? x - (boss->x + size - 1) : 0;
    int dy = y < boss->y ? boss->y - y : y > boss->y + size - 1 ? y - (boss->y + size - 1) : 0;
    return dx <= 1 && dy <= 1;
}

static void clear_fight(RcWorld *w) {
    RcModeBossState *s = &w->mode_boss;
    s->active = false;
    s->phase = 1;
    s->participants = 0;
    s->max_hp = s->base_hp;
    memset(s->damage, 0, sizeof(s->damage));
    s->empty_since = 0;
    s->next_bricks = s->next_summon = s->next_feed = 0;
    s->moving_to = s->feeding_pile = -1;
    s->heals_left = 0;
    s->hazard_count = 0;
}

// No living player has been inside for reset_ticks: the boss, its rats and its bricks start over.
static void reset_fight(RcWorld *w, RcNpc *boss) {
    RcModeBossState *s = &w->mode_boss;
    for (int i = 0; i < s->minion_count; i++) (void)rc_npc_remove(w, (RcNpcId)s->minions[i]);
    s->minion_count = 0;
    boss->owner_controlled = false;
    rc_npc_reset_life(w, boss);
    clear_fight(w);
}

static void grant_loot(RcWorld *w, const RcModeBoss *b, const RcNpc *boss, int owner) {
    RcGroundGrant grants[12];
    int n = 0;
    for (int i = 0; i < b->always_count; i++) {
        const RcModeDropRow *r = &b->always[i];
        int qty = r->max > r->min ? r->min + rc_rng_range(&w->rng_state, r->max - r->min) : r->min;
        grants[n++] = (RcGroundGrant){.item_id = r->item, .quantity = qty};
    }
    if (b->bundle_count > 0) {
        const RcModeStack *pick = &b->bundle[rc_rng_range(&w->rng_state, b->bundle_count - 1)];
        grants[n++] = (RcGroundGrant){.item_id = pick->item, .quantity = pick->qty};
    }
    for (int i = 0; i < b->extra_count; i++)
        if (b->extra[i].one_in <= 1 || rc_rng_range(&w->rng_state, b->extra[i].one_in - 1) == 0)
            grants[n++] = (RcGroundGrant){.item_id = b->extra[i].item, .quantity = b->extra[i].qty};
    const RcModeRules *m = rc_mode_active();
    int private_ticks = m->private_ticks, public_ticks = m->public_ticks;
    RcGroundPolicy policy = {.reveal_ticks = private_ticks,
                             .lifetime_ticks = private_ticks + public_ticks,
                             .untradeable_lifetime_ticks = private_ticks + public_ticks};
    (void)rc_ground_grant(w, grants, n, boss->x + 1, boss->y + 1, boss->plane, owner, policy);
}

// A kill resolves once, from the tick's final state: players alive inside with at least
// qualify_pct of the fight's actual damage share the kill credit, and the one bundle goes to the
// next of them in the seeded rotation. With nobody qualified, the largest contributor (ties broken
// by the seeded generator) owns the loot without kill credit.
static void resolve_kill(RcWorld *w, const RcModeBoss *b, RcNpc *boss) {
    RcModeBossState *s = &w->mode_boss;
    uint32_t total = 0;
    for (int i = 0; i < RC_MAX_PLAYERS; i++) total += boss->player_damage[i];
    uint16_t inside = occupants(w, b), qualified = 0;
    for (int i = 0; i < w->player_count && total; i++)
        if ((inside >> i & 1) && (uint64_t)boss->player_damage[i] * 100 >= (uint64_t)total * b->qualify_pct)
            qualified |= (uint16_t)(1u << i);
    int owner = -1;
    if (qualified) {
        int n = count_bits(qualified), pick = (int)(s->rotation++ % (uint32_t)n);
        for (int i = 0; i < w->player_count; i++)
            if ((qualified >> i & 1) && pick-- == 0) owner = i;
    } else {
        int best = 0, ties = 0;
        for (int i = 0; i < w->player_count; i++) {
            if (boss->player_damage[i] > best) best = boss->player_damage[i], ties = 1, owner = i;
            else if (best && boss->player_damage[i] == best) ties++;
        }
        if (ties > 1) {
            int pick = rc_rng_range(&w->rng_state, ties - 1);
            for (int i = 0; i < w->player_count; i++)
                if (boss->player_damage[i] == best && pick-- == 0) owner = i;
        }
    }
    if (owner >= 0) grant_loot(w, b, boss, owner);
    s->kills++;
    s->last_kill_tick = w->tick;
    s->last_owner = owner;
    s->last_qualified = qualified;
    clear_fight(w);
}

static void land_bricks(RcWorld *w, const RcModeBoss *b, const RcNpc *boss) {
    RcModeBossState *s = &w->mode_boss;
    int kept = 0;
    for (int i = 0; i < s->hazard_count; i++) {
        RcModeHazard h = s->hazards[i];
        if (h.land_tick > w->tick) {
            s->hazards[kept++] = h;
            continue;
        }
        for (int p = 0; p < w->player_count; p++) {
            RcPlayer *pl = &w->players[p];
            if (!pl->active || pl->is_dead || pl->x != h.x || pl->y != h.y || pl->plane != 0) continue;
            int damage = rc_rng_range(&w->rng_state, b->hazard_max_hit);
            // Typeless: protection prayers do not block falling bricks.
            rc_queue_hit_meta(pl->pending_hits, &pl->num_pending_hits, damage, 0, COMBAT_NONE,
                              boss->uid, 0, w->tick, 0, b->hazard_max_hit);
        }
    }
    s->hazard_count = kept;
}

static void add_hazard(RcWorld *w, int x, int y, int warning) {
    RcModeBossState *s = &w->mode_boss;
    for (int i = 0; i < s->hazard_count; i++)
        if (s->hazards[i].x == x && s->hazards[i].y == y) return;
    if (s->hazard_count < RC_MODE_BOSS_HAZARDS)
        s->hazards[s->hazard_count++] = (RcModeHazard){(int16_t)x, (int16_t)y,
                                                        w->tick + (RcTick)warning};
}

// One volley: a brick on each living player's tile inside, and some on random tiles.
static void drop_bricks(RcWorld *w, const RcModeBoss *b, uint16_t inside) {
    for (int p = 0; p < w->player_count; p++)
        if (inside >> p & 1) add_hazard(w, w->players[p].x, w->players[p].y, b->hazard_warning_ticks);
    int extra = b->hazard_random_tiles * (w->mode_boss.phase >= 3 ? 2 : 1), x, y;
    for (int i = 0; i < extra; i++)
        if (random_tile(w, b, &x, &y)) add_hazard(w, x, y, b->hazard_warning_ticks);
}

static void summon_rats(RcWorld *w, const RcModeBoss *b) {
    RcModeBossState *s = &w->mode_boss;
    int def_idx = rc_npc_def_find(b->minion), x, y;
    int room = b->max_minions - s->minion_count;
    if (room > RC_MODE_BOSS_MINIONS - s->minion_count) room = RC_MODE_BOSS_MINIONS - s->minion_count;
    for (int i = 0; def_idx >= 0 && i < b->minions_per_summon && i < room; i++) {
        if (!random_tile(w, b, &x, &y)) continue;
        RcNpcSpawnConfig cfg = {.wander_range = 3};
        RcNpcSpawnResult r = rc_npc_spawn_ex(w, def_idx, x, y, 0, &cfg);
        if (r.status == RC_NPC_SPAWN_CREATED) s->minions[s->minion_count++] = r.uid;
    }
}

static int food_pile_nearest(const RcModeBoss *b, const RcNpc *boss) {
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < b->food_count; i++) {
        int dx = b->food[i][0] - boss->x, dy = b->food[i][1] - boss->y;
        int d = dx * dx + dy * dy;
        if (d < best_d) best_d = d, best = i;
    }
    return best;
}

static void scurrius_tick(RcWorld *w) {
    const RcModeBoss *b = boss_rules();
    if (!b) return;
    RcModeBossState *s = &w->mode_boss;
    if (!s->base_hp) {
        s->base_hp = s->max_hp = b->hp;
        s->moving_to = s->feeding_pile = -1;
        s->last_owner = -1;
        s->boss_uid = -1;
        s->phase = 1;
    }
    prune_minions(w);
    RcNpc *boss = find_boss(w, b);
    if (!boss) return;
    if (boss->is_dead || boss->current_hp <= 0) {
        boss->owner_controlled = false;
        if (s->active) resolve_kill(w, b, boss);
        s->hazard_count = 0;
        return;
    }
    uint16_t inside = occupants(w, b);
    if (!s->active) {
        if (!inside) return;
        clear_fight(w);
        s->active = true;
        s->next_bricks = w->tick + (RcTick)b->hazard_interval;
    }
    // Scaling: each new participant adds the same absolute HP to the maximum and the current.
    uint16_t joined = inside & (uint16_t)~s->participants;
    for (int p = 0; p < w->player_count; p++) {
        if (!(joined >> p & 1)) continue;
        if (s->participants) {
            int add = s->base_hp * b->group_scale_pct / 100;
            s->max_hp += add;
            boss->current_hp += add;
        }
        s->participants |= (uint16_t)(1u << p);
    }
    boss->spawn_hp = s->max_hp;
    if (!inside) {
        if (!s->empty_since) s->empty_since = w->tick;
        else if (w->tick - s->empty_since >= (RcTick)b->reset_ticks) {
            reset_fight(w, boss);
            return;
        }
    } else {
        s->empty_since = 0;
    }
    land_bricks(w, b, boss);

    int pct = s->max_hp ? boss->current_hp * 100 / s->max_hp : 100;
    if (s->phase < 2 && pct < b->phase2_pct) {
        s->phase = 2;
        s->next_feed = w->tick;  // the first feeding starts on crossing the threshold
        s->next_summon = w->tick + 2;
    }
    if (s->phase < 3 && pct < b->phase3_pct) {
        s->phase = 3;
        s->feeding_pile = -1;
        s->heals_left = 0;
        s->moving_to = CENTRE;
        walk_boss(w, boss, b->centre[0], b->centre[1]);
    }
    // Walking to a food pile or the centre.
    if (s->moving_to >= 0) {
        int tx = s->moving_to == CENTRE ? b->centre[0] : b->food[s->moving_to][0];
        int ty = s->moving_to == CENTRE ? b->centre[1] : b->food[s->moving_to][1];
        const RcNpcDef *d = rc_npc_def_for_npc(w, boss);
        int size = d && d->size > 0 ? d->size : 1;
        if (boss_beside(boss, size, tx, ty) || boss->route_status == RC_ROUTE_FAILED
                || boss->movement_result == RC_MOVEMENT_ARRIVED
                || boss->movement_result == RC_MOVEMENT_NO_ROUTE) {
            boss->owner_controlled = false;
            if (s->moving_to != CENTRE) {
                s->feeding_pile = s->moving_to;
                s->heals_left = (uint8_t)b->feed_heals;
                s->next_heal = w->tick + (RcTick)b->feed_ticks;
            }
            s->moving_to = -1;
        } else if (boss->route_mode != RC_NPC_ROUTE_SCRIPTED) {
            walk_boss(w, boss, tx, ty);
        }
    } else if (s->phase == 2 && s->feeding_pile < 0 && b->food_count && w->tick >= s->next_feed) {
        // Usually the nearest pile, sometimes another.
        int pile = rc_rng_range(&w->rng_state, 3) ? food_pile_nearest(b, boss)
                                                  : rc_rng_range(&w->rng_state, b->food_count - 1);
        s->moving_to = (int8_t)pile;
        walk_boss(w, boss, b->food[pile][0], b->food[pile][1]);
    }
    // Eating: each heal restores feed_heal per living player inside.
    if (s->feeding_pile >= 0 && w->tick >= s->next_heal) {
        int heal = b->feed_heal * count_bits(inside);
        boss->current_hp = boss->current_hp + heal > s->max_hp ? s->max_hp : boss->current_hp + heal;
        s->next_heal = w->tick + (RcTick)b->feed_ticks;
        if (--s->heals_left == 0) {
            s->feeding_pile = -1;
            s->next_feed = w->tick + (RcTick)b->feed_interval;
        }
    }
    if (inside && w->tick >= s->next_bricks) {
        drop_bricks(w, b, inside);
        int interval = s->phase >= 3 ? b->hazard_interval / 2 : b->hazard_interval;
        s->next_bricks = w->tick + (RcTick)(interval > 1 ? interval : 1);
    }
    if (inside && s->phase >= 2 && w->tick >= s->next_summon) {
        summon_rats(w, b);
        s->next_summon = w->tick + (RcTick)b->summon_interval;
    }
}

// ---- Combat hooks: the boss's style, reach and cadence; other NPCs pass through ----------------

static RcCombatStyle (*base_select_style)(RcWorld *, RcNpc *, const RcPlayer *, RcCombatStyle);
static int (*base_attack_range)(const RcWorld *, const RcNpc *, RcCombatStyle, int);
static int (*base_attack_speed)(RcWorld *, RcNpc *, int);

static RcCombatStyle select_style(RcWorld *w, RcNpc *npc, const RcPlayer *p, RcCombatStyle style) {
    if (!is_boss(w, npc))
        return base_select_style ? base_select_style(w, npc, p, style) : style;
    const RcModeBossState *s = &w->mode_boss;
    // At a food pile and in the final phase: bolts of electricity or flying fur.
    if (s->phase >= 3 || s->feeding_pile >= 0)
        return rc_rng_range(&w->rng_state, 1) ? COMBAT_MAGIC : COMBAT_RANGED;
    int roll = rc_rng_range(&w->rng_state, 11);  // tail swipe 10 : bolts 1 : fur 1
    return roll < 10 ? COMBAT_MELEE_CRUSH : roll == 10 ? COMBAT_MAGIC : COMBAT_RANGED;
}

static int attack_range(const RcWorld *w, const RcNpc *npc, RcCombatStyle style, int range) {
    if (is_boss(w, npc) && w->mode_boss.feeding_pile >= 0 && style != COMBAT_MELEE_CRUSH)
        return 20;  // it eats in place and attacks from the pile
    return base_attack_range ? base_attack_range(w, npc, style, range) : range;
}

static int attack_speed(RcWorld *w, RcNpc *npc, int speed) {
    const RcModeBoss *b = boss_rules();
    if (is_boss(w, npc)) return w->mode_boss.phase >= 3 ? b->final_attack_speed : b->attack_speed;
    return base_attack_speed ? base_attack_speed(w, npc, speed) : speed;
}

static RcInteractionHandlerResult food_pile(RcWorld *world, RcPlayer *player,
                                            const RcPendingInteraction *pending, void *ctx) {
    (void)world;
    (void)player;
    (void)pending;
    (void)ctx;
    return rc_interaction_result_message("The rats have spoiled it; you can't eat this.");
}

int rc_content_scurrius_register(RcWorld *world) {
    if (!world) return 0;
    if (!boss_rules() || !(world->enabled & RC_SUB_COMBAT)) return 1;
    world->mode_boss.boss_uid = -1;
    world->mode_boss.last_owner = -1;
    world->mode_tick = scurrius_tick;
    // Chain onto whatever regular-NPC hooks are installed (identical for every world).
    if (world->combat_hooks.select_npc_style != select_style) {
        base_select_style = world->combat_hooks.select_npc_style;
        world->combat_hooks.select_npc_style = select_style;
    }
    if (world->combat_hooks.npc_attack_range != attack_range) {
        base_attack_range = world->combat_hooks.npc_attack_range;
        world->combat_hooks.npc_attack_range = attack_range;
    }
    if (world->combat_hooks.modify_npc_attack_speed != attack_speed) {
        base_attack_speed = world->combat_hooks.modify_npc_attack_speed;
        world->combat_hooks.modify_npc_attack_speed = attack_speed;
    }
    // Players cannot eat from the lair's food piles in this mode.
    int ok = 1;
    for (int id = FOOD_PILE_A; id <= FOOD_PILE_B; id++) {
        const RcObjectDef *def = rc_object_def_get(id);
        for (int o = 0; def && o < RC_OBJECT_ACTIONS; o++) {
            if (strcmp(def->actions[o], "Eat")) continue;
            RcInteractionDispatchKey key = rc_interaction_dispatch_key_any();
            key.kind = RC_INTERACTION_OBJECT;
            key.definition_id = id;
            key.op = rc_interaction_op_from_option(o);
            ok &= rc_interaction_register_world_handler(world, &key, food_pile, NULL);
        }
    }
    return ok;
}
