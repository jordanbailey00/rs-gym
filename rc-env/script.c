#include "script.h"

#include "../rc-core/api.h"
#include "../rc-core/items.h"
#include "../rc-core/npc.h"
#include "../rc-core/object_runtime.h"
#include "../rc-core/objects.h"
#include "../rc-core/players.h"
#include "../rc-core/shops.h"
#include "../rc-core/skilling.h"
#include "../rc-core/skills.h"
#include "../rc-core/storage.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

enum { BANK_BOOTH = 10355 };

static const int foods[] = {379, 333, 315, 2142, 2140};  // lobster, trout, shrimps, meat, chicken
static const int bones[] = {526, 532};

// ---- Scheduler ------------------------------------------------------------------------------------

static ScRun *g_run;  // the run being stepped (runs are single-threaded)

static void trampoline(void) {
    ScAgent *a = &g_run->agents[g_run->current];
    a->plan(a, a->arg);
    a->done = 1;
}

void sc_init(ScRun *run, RcWorld *world, int agents, FILE *log) {
    memset(run, 0, sizeof(*run));
    run->world = world;
    run->count = agents;
    run->log = log;
    for (int i = 0; i < agents; i++) {
        run->agents[i].run = run;
        run->agents[i].index = i;
    }
}

void sc_start(ScRun *run, int agent, ScPlan plan, void *arg) {
    ScAgent *a = &run->agents[agent];
    a->plan = plan;
    a->arg = arg;
    a->stack = malloc(SC_STACK_SIZE);
    getcontext(&a->ctx);
    a->ctx.uc_stack.ss_sp = a->stack;
    a->ctx.uc_stack.ss_size = SC_STACK_SIZE;
    a->ctx.uc_link = &run->main;
    makecontext(&a->ctx, trampoline, 0);
    a->started = 1;
}

int sc_run(ScRun *run, RcTick until) {
    RcWorld *w = run->world;
    RcTick start = w->tick;
    while (w->tick < until) {
        int alive = 0;
        for (int i = 0; i < run->count; i++) {
            ScAgent *a = &run->agents[i];
            if (!a->started || a->done) continue;
            alive++;
            int dead = w->players[i].is_dead;
            if (dead && !a->was_dead) a->deaths++;
            a->was_dead = dead;
            if (a->wake_tick > w->tick) continue;
            run->current = i;
            g_run = run;
            rc_world_select_player(w, i);
            swapcontext(&run->main, &a->ctx);
        }
        if (!alive) break;
        rc_world_tick(w);
    }
    rc_world_select_player(w, 0);
    return (int)(w->tick - start);
}

void sc_free(ScRun *run) {
    for (int i = 0; i < run->count; i++) {
        free(run->agents[i].stack);
        run->agents[i].stack = NULL;
    }
}

RcWorld *sc_world(ScAgent *a) {
    return a->run->world;
}

RcPlayer *sc_player(ScAgent *a) {
    rc_world_select_player(a->run->world, a->index);
    return a->run->world->player;
}

void sc_wait(ScAgent *a, int ticks) {
    a->wake_tick = a->run->world->tick + (ticks > 0 ? ticks : 1);
    swapcontext(&a->ctx, &a->run->main);
    rc_world_select_player(a->run->world, a->index);
}

void sc_note(ScAgent *a, const char *fmt, ...) {
    if (!a->run->log) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(a->run->log, "t%u a%d ", (unsigned)a->run->world->tick, a->index);
    vfprintf(a->run->log, fmt, ap);
    fputc('\n', a->run->log);
    va_end(ap);
}

void sc_fail(ScAgent *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->failure, sizeof(a->failure), fmt, ap);
    va_end(ap);
    sc_note(a, "FAILED: %s", a->failure);
    a->failed = 1;
    a->done = 1;
    swapcontext(&a->ctx, &a->run->main);  // never resumed
}

// ---- Movement -------------------------------------------------------------------------------------

int sc_near(ScAgent *a, int x, int y, int tol) {
    const RcPlayer *p = sc_player(a);
    return abs(p->x - x) <= tol && abs(p->y - y) <= tol;
}

static int max_i(int a, int b) { return a > b ? a : b; }

// Clicks toward the target in steps the route search can cover, until within `tol` tiles.
int sc_walk(ScAgent *a, int x, int y, int tol) {
    RcWorld *w = sc_world(a);
    int stuck = 0, best = 1 << 30;
    for (int click = 0; click < 64; click++) {
        RcPlayer *p = sc_player(a);
        if (p->is_dead) return 0;
        int dx = x - p->x, dy = y - p->y, dist = max_i(abs(dx), abs(dy));
        if (dist <= tol) return 1;
        if (dist < best) {
            best = dist;
            stuck = 0;
        } else if (++stuck >= 4) {
            return 0;
        }
        int step = dist > 40 ? 40 : dist;
        int tx = p->x + dx * step / dist, ty = p->y + dy * step / dist;
        if (stuck) {  // sidestep around whatever stopped the last click
            tx += (click & 1 ? 1 : -1) * stuck * 3;
            ty += (click & 2 ? 1 : -1) * stuck * 3;
        }
        (void)rc_player_set_running(w, p->run_energy > 1500);
        if (!rc_player_walk_to(w, tx, ty)) {
            sc_wait(a, 1);
            continue;
        }
        int lx = -1, ly = -1, still = 0;
        for (int t = 0; t < 80 && still < 2; t++) {
            sc_wait(a, 1);
            p = sc_player(a);
            still = p->x == lx && p->y == ly ? still + 1 : 0;
            lx = p->x;
            ly = p->y;
            if (abs(x - p->x) <= tol && abs(y - p->y) <= tol) return 1;
        }
    }
    return 0;
}

// ---- Locs -----------------------------------------------------------------------------------------

int sc_find_loc(ScAgent *a, const int *ids, int n, int radius, int *ox, int *oy, int *oid) {
    const RcWorld *w = sc_world(a);
    const RcPlayer *p = sc_player(a);
    int best = 1 << 30, found = 0;
    for (int dx = -radius; dx <= radius; dx++) {
        for (int dy = -radius; dy <= radius; dy++) {
            int x = p->x + dx, y = p->y + dy, d = max_i(abs(dx), abs(dy));
            if (d >= best) continue;
            RcObjectPlacement rows[8];
            int count = rc_object_placements_at(x, y, p->plane, rows, 8);
            for (int i = 0; i < count; i++) {
                for (int k = 0; k < n; k++) {
                    RcObjectPlacement current;
                    if ((int)rows[i].obj_id != ids[k]
                            || !rc_world_object_current_placement(w, ids[k], x, y, p->plane,
                                                                  0, &current))
                        continue;
                    best = d;
                    *ox = x;
                    *oy = y;
                    if (oid) *oid = ids[k];
                    found = 1;
                }
            }
        }
    }
    return found;
}

int sc_loc_option(ScAgent *a, int id, int x, int y, const char *label) {
    const RcWorld *w = sc_world(a);
    int plane = sc_player(a)->plane;
    for (int op = 0; op < RC_OBJECT_ACTIONS; op++)
        if (rc_world_object_option_supported(w, id, x, y, plane, 0, op)
                && !strcmp(rc_world_object_option_label(w, id, x, y, plane, 0, op), label))
            return op;
    return -1;
}

// Waits for a started interaction to be walked to and handled.
static int await_interaction(ScAgent *a, int timeout) {
    sc_wait(a, 1);
    for (int t = 0; t < timeout; t++) {
        const RcPlayer *p = sc_player(a);
        if (p->is_dead) return 0;
        if (!p->interaction.active) return p->interaction.last_failure == 0;
        sc_wait(a, 1);
    }
    return 0;
}

int sc_use_loc(ScAgent *a, int id, int x, int y, const char *label) {
    int op = sc_loc_option(a, id, x, y, label);
    if (op < 0) return 0;
    RcPlayer *p = sc_player(a);
    if (!rc_player_interact_object_at(sc_world(a), id, x, y, p->plane, op)) return 0;
    return await_interaction(a, 120);
}

int sc_use_item_on_loc(ScAgent *a, int item, int id, int x, int y) {
    int slot = sc_slot(a, item);
    RcPlayer *p = sc_player(a);
    if (slot < 0 || !rc_player_use_inventory_item_on_object(sc_world(a), slot, id, x, y, p->plane))
        return 0;
    return await_interaction(a, 120);
}

// ---- NPCs -----------------------------------------------------------------------------------------

static int engaged_by_other(const RcWorld *w, const RcNpc *n, int me) {
    for (int i = 0; i < RC_MAX_PLAYERS; i++)
        if (i != me && w->players[i].active && w->players[i].attack_target == n->uid) return 1;
    return 0;
}

const RcNpc *sc_find_npc(ScAgent *a, int def_id, int radius, int unengaged) {
    const RcWorld *w = sc_world(a);
    const RcPlayer *p = sc_player(a);
    int count = 0, best = 1 << 30;
    const RcNpc *npcs = rc_get_npcs(w, &count), *found = NULL;
    for (int i = 0; i < count; i++) {
        const RcNpc *n = &npcs[i];
        if (!n->active || n->is_dead || n->plane != p->plane) continue;
        const RcNpcDef *d = rc_npc_def_for_npc(w, n);
        if (!d || d->id != def_id) continue;
        int dist = max_i(abs(n->x - p->x), abs(n->y - p->y));
        if (dist > radius || dist >= best) continue;
        if (unengaged && engaged_by_other(w, n, a->index)) continue;
        best = dist;
        found = n;
    }
    return found;
}

int sc_talk(ScAgent *a, const RcNpc *npc, const char *option) {
    RcWorld *w = sc_world(a);
    const RcNpcDef *d = npc ? rc_npc_def_for_npc(w, npc) : NULL;
    for (int o = 0; d && o < RC_NPC_OPTION_COUNT; o++) {
        if (strcmp(d->options[o], option)) continue;
        rc_player_interact_npc(w, npc->uid, o);
        return await_interaction(a, 120);
    }
    return 0;
}

// ---- Inventory ------------------------------------------------------------------------------------

int sc_count(ScAgent *a, int item) {
    const RcPlayer *p = sc_player(a);
    int n = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        if (p->inventory[i].item_id == item) n += p->inventory[i].quantity;
    return n;
}

int sc_slot(ScAgent *a, int item) {
    return rc_inv_find(sc_player(a)->inventory, item);
}

int sc_free_slots(ScAgent *a) {
    const RcPlayer *p = sc_player(a);
    int n = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++) n += p->inventory[i].item_id < 0;
    return n;
}

int sc_equip(ScAgent *a, int item) {
    int slot = sc_slot(a, item);
    if (slot < 0) return sc_player(a)->equipment[rc_item_def_get(item)
        ? rc_item_def_get(item)->equip_slot : 0].item_id == item;
    (void)rc_player_equip(sc_world(a), slot);
    sc_wait(a, 1);
    const RcItemDef *d = rc_item_def_get(item);
    return d && d->equip_slot >= 0 && sc_player(a)->equipment[d->equip_slot].item_id == item;
}

int sc_eat(ScAgent *a) {
    for (int i = 0; i < COUNT(foods); i++) {
        int slot = sc_slot(a, foods[i]);
        if (slot < 0) continue;
        (void)rc_player_eat(sc_world(a), slot);
        sc_wait(a, 1);
        return 1;
    }
    return 0;
}

int sc_bury_all(ScAgent *a) {
    int before = 0, after = 0;
    for (int b = 0; b < COUNT(bones); b++) {
        before += sc_count(a, bones[b]);
        for (int tries = 0; tries < 64; tries++) {
            int slot = sc_slot(a, bones[b]);
            if (slot < 0 || !rc_player_interact_inventory_item(sc_world(a), slot, 0)) break;
            sc_wait(a, 2);
        }
        after += sc_count(a, bones[b]);
    }
    return before - after;
}

int sc_drop_all(ScAgent *a, int item) {
    int dropped = 0, slot;
    while ((slot = sc_slot(a, item)) >= 0 && dropped < RC_INVENTORY_SIZE) {
        (void)rc_player_drop_item(sc_world(a), slot);
        sc_wait(a, 1);
        dropped++;
    }
    return dropped;
}

int sc_pickup(ScAgent *a, const int *items, int n, int radius) {
    RcWorld *w = sc_world(a);
    int taken = 0, waited = 0;
    for (int round = 0; round < 32; round++) {
        const RcPlayer *p = sc_player(a);
        int best = -1, best_d = 1 << 30;
        for (int i = 0; i < w->ground_item_count; i++) {
            const RcGroundItem *g = &w->ground_items[i];
            if (!g->active || g->arrival_timer || g->plane != p->plane
                    || !rc_ground_item_visible(g, p->index))
                continue;
            int want = 0;
            for (int k = 0; k < n; k++) want |= g->item_id == items[k];
            int d = max_i(abs(g->x - p->x), abs(g->y - p->y));
            if (want && d <= radius && d < best_d) best = i, best_d = d;
        }
        if (best < 0 && round == 0 && waited < 10) {  // a kill's drop lands after its death
            sc_wait(a, 1);
            waited++;
            round--;
            continue;
        }
        if (best < 0) return taken;
        const RcItemDef *def = rc_item_def_get(w->ground_items[best].item_id);
        if (sc_free_slots(a) == 0 && !(def && def->stackable && sc_slot(a, def->id) >= 0)) return taken;
        int uid = w->ground_items[best].uid;
        (void)rc_player_pickup_item(w, best);
        for (int t = 0; t < 40; t++) {
            sc_wait(a, 1);
            if (!w->ground_items[best].active || w->ground_items[best].uid != uid) break;
        }
        if (w->ground_items[best].active && w->ground_items[best].uid == uid) return taken;
        taken++;
    }
    return taken;
}

// ---- Bank -------------------------------------------------------------------------------------------

int sc_bank_open(ScAgent *a) {
    int ids[] = {BANK_BOOTH}, x, y;
    if (sc_player(a)->storage_kind) return 1;
    if (!sc_find_loc(a, ids, 1, 24, &x, &y, NULL)) return 0;
    if (!sc_use_loc(a, BANK_BOOTH, x, y, "Bank")) return 0;
    for (int t = 0; t < 20 && !sc_player(a)->storage_kind; t++) sc_wait(a, 1);
    return sc_player(a)->storage_kind != 0;
}

int sc_bank_count(ScAgent *a, int item) {
    const RcPlayer *p = sc_player(a);
    int n = 0;
    for (int i = 0; i < RC_BANK_SIZE; i++)
        if (p->bank[i].item_id == item) n += p->bank[i].quantity;
    return n;
}

int sc_withdraw(ScAgent *a, int item, int qty) {
    const RcPlayer *p = sc_player(a);
    for (int i = 0; i < RC_BANK_SIZE; i++) {
        if (p->bank[i].item_id != item) continue;
        int before = sc_count(a, item);
        (void)rc_bank_withdraw_slot(sc_world(a), i, qty);
        sc_wait(a, 1);
        return sc_count(a, item) - before;
    }
    return 0;
}

int sc_deposit(ScAgent *a, int item, int qty) {
    int slot = sc_slot(a, item);
    if (slot < 0) return 0;
    int before = sc_count(a, item);
    (void)rc_bank_deposit_slot(sc_world(a), slot, qty);
    sc_wait(a, 1);
    return before - sc_count(a, item);
}

int sc_deposit_all_except(ScAgent *a, const int *keep, int n) {
    int moved = 0;
    for (int pass = 0; pass < RC_INVENTORY_SIZE; pass++) {
        const RcPlayer *p = sc_player(a);
        int slot = -1;
        for (int i = 0; i < RC_INVENTORY_SIZE && slot < 0; i++) {
            int item = p->inventory[i].item_id, kept = 0;
            for (int k = 0; k < n; k++) kept |= item == keep[k];
            if (item >= 0 && !kept) slot = i;
        }
        if (slot < 0) break;
        (void)rc_bank_deposit_slot(sc_world(a), slot, 1 << 30);
        sc_wait(a, 1);
        moved++;
    }
    return moved;
}

void sc_bank_close(ScAgent *a) {
    (void)rc_player_close_storage(sc_world(a));
    sc_wait(a, 1);
}

// ---- Shops ------------------------------------------------------------------------------------------

int sc_shop_open(ScAgent *a, int npc_def) {
    const RcNpc *n = sc_find_npc(a, npc_def, 30, 0);
    if (!n || !sc_talk(a, n, "Trade")) return 0;
    return sc_player(a)->shop_open >= 0;
}

int sc_buy(ScAgent *a, int item, int qty) {
    RcWorld *w = sc_world(a);
    const RcPlayer *p = sc_player(a);
    if (p->shop_open < 0) return 0;
    const RcShopState *s = &w->shops[p->shop_open];
    for (int i = 0; i < s->slot_count; i++) {
        if (s->slots[i].item_id != item) continue;
        int before = sc_count(a, item);
        (void)rc_shop_buy(w, i, qty);
        sc_wait(a, 1);
        return sc_count(a, item) - before;
    }
    return 0;
}

int sc_sell(ScAgent *a, int item, int qty) {
    int slot = sc_slot(a, item);
    if (slot < 0 || sc_player(a)->shop_open < 0) return 0;
    int before = sc_count(a, item);
    (void)rc_shop_sell(sc_world(a), slot, qty);
    sc_wait(a, 1);
    return before - sc_count(a, item);
}

void sc_shop_close(ScAgent *a) {
    rc_shop_close(sc_player(a));
}

// ---- Production -------------------------------------------------------------------------------------

int sc_make(ScAgent *a, int item, int count) {
    RcWorld *w = sc_world(a);
    int option = -1;
    for (int i = 0; i < w->make_option_count && option < 0; i++) {
        const RcRecipe *r = rc_recipe_get(w->make_options[i].recipe);
        if (r && (int)r->output_item == item && rc_skilling_make_option_ready(w, i)) option = i;
    }
    if (option < 0) return 0;
    int before = sc_count(a, item);
    if (!rc_skilling_make(w, option, count)) return 0;
    sc_wait(a, 1);
    for (int t = 0; t < 600; t++) {
        const RcPlayer *p = sc_player(a);
        if (p->is_dead) break;
        if (p->skill_job.kind == RC_SKILL_JOB_NONE && !p->skill_action) break;
        sc_wait(a, 1);
    }
    rc_skilling_close_make(sc_player(a));
    return sc_count(a, item) - before;
}

int sc_use_items(ScAgent *a, int item_a, int item_b) {
    int sa = sc_slot(a, item_a), sb = sc_slot(a, item_b);
    if (sa < 0 || sb < 0) return 0;
    if (!rc_player_use_inventory_item_on_inventory_item(sc_world(a), sa, sb)) return 0;
    sc_wait(a, 1);
    for (int t = 0; t < 5 && !sc_player(a)->make_menu; t++) sc_wait(a, 1);
    return sc_player(a)->make_menu != 0;
}

// ---- Gathering --------------------------------------------------------------------------------------

int sc_gather(ScAgent *a, const int *locs, int n, const char *option, int product, int want,
              int radius) {
    int idle = 0, failed = 0;
    while (sc_count(a, product) < want && sc_free_slots(a) > 0) {
        const RcPlayer *p = sc_player(a);
        if (p->is_dead) return 0;
        if (p->skill_job.kind == RC_SKILL_JOB_GATHER) {
            sc_wait(a, 1);
            continue;
        }
        int x, y, id;
        if (!sc_find_loc(a, locs, n, radius, &x, &y, &id)) {
            if (++idle > 120) return 0;  // everything depleted: wait for a respawn
            sc_wait(a, 2);
            continue;
        }
        idle = 0;
        if (!sc_use_loc(a, id, x, y, option) && ++failed > 20) return 0;
    }
    return sc_count(a, product) >= want;
}

int sc_fish(ScAgent *a, int spot_def, const char *option, int product, int want, int radius) {
    int idle = 0;
    while (sc_count(a, product) < want && sc_free_slots(a) > 0) {
        const RcPlayer *p = sc_player(a);
        if (p->is_dead) return 0;
        if (p->skill_job.kind == RC_SKILL_JOB_FISH) {
            sc_wait(a, 1);
            continue;
        }
        const RcNpc *spot = sc_find_npc(a, spot_def, radius, 0);
        if (!spot || !sc_talk(a, spot, option)) {
            if (++idle > 60) return 0;
            sc_wait(a, 2);
            continue;
        }
        idle = 0;
    }
    return sc_count(a, product) >= want;
}

// ---- Combat -----------------------------------------------------------------------------------------

int sc_kill(ScAgent *a, int def_id, int radius, int eat_below) {
    RcWorld *w = sc_world(a);
    const RcNpc *n = sc_find_npc(a, def_id, radius, 1);
    if (!n) return 0;
    int uid = n->uid;
    if (!rc_player_attack_npc(w, uid)) return 0;
    for (int t = 0; t < 600; t++) {
        sc_wait(a, 1);
        const RcPlayer *p = sc_player(a);
        if (p->is_dead) return -1;
        const RcNpc *target = rc_npc_resolve_const(w, (RcNpcId)uid);
        if (!target || target->is_dead || !target->active) return 1;
        if (p->current_hp < eat_below && !sc_eat(a)) {
            // out of food: keep fighting; the plan decides when to retreat
        }
        if (p->attack_target != uid) (void)rc_player_attack_npc(w, uid);
    }
    return 0;
}
