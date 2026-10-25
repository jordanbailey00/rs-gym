#include "world_state.h"
#include "items.h"

#include "npc.h"
#include "combat.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct RcDormantNpcState {
    uint64_t spawn_key;
    RcTick saved_tick;
    RcNpc npc;
};

struct RcDormantGroundItemState {
    uint64_t key;
    RcTick saved_tick;
    bool static_spawn;
    RcGroundItem item;
};

enum {
    RC_MAX_DORMANT_NPC_STATES = RC_MAX_NPCS,
    RC_MAX_DORMANT_GROUND_ITEM_STATES = RC_MAX_GROUND_ITEMS * 8,
};

static int reserve_npcs(RcWorld *world, int needed) {
    if (needed < 0 || needed > RC_MAX_DORMANT_NPC_STATES) return 0;
    if (needed <= world->dormant_npc_capacity) return 1;
    int capacity = world->dormant_npc_capacity > 0
                 ? world->dormant_npc_capacity : 16;
    while (capacity < needed) {
        if (capacity > INT_MAX / 2) return 0;
        capacity *= 2;
    }
    void *states = realloc(world->dormant_npcs,
                           (size_t)capacity * sizeof(*world->dormant_npcs));
    if (!states) return 0;
    world->dormant_npcs = states;
    world->dormant_npc_capacity = capacity;
    return 1;
}

static int reserve_ground_items(RcWorld *world, int needed) {
    if (needed < 0 || needed > RC_MAX_DORMANT_GROUND_ITEM_STATES) return 0;
    if (needed <= world->dormant_ground_item_capacity) return 1;
    int capacity = world->dormant_ground_item_capacity > 0
                 ? world->dormant_ground_item_capacity : 16;
    while (capacity < needed) {
        if (capacity > INT_MAX / 2) return 0;
        capacity *= 2;
    }
    void *states = realloc(
        world->dormant_ground_items,
        (size_t)capacity * sizeof(*world->dormant_ground_items));
    if (!states) return 0;
    world->dormant_ground_items = states;
    world->dormant_ground_item_capacity = capacity;
    return 1;
}

static int npc_lower_bound(const RcWorld *world, uint64_t key, bool *found) {
    int lo = 0;
    int hi = world->dormant_npc_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (world->dormant_npcs[mid].spawn_key < key) lo = mid + 1;
        else hi = mid;
    }
    if (found) {
        *found = lo < world->dormant_npc_count
              && world->dormant_npcs[lo].spawn_key == key;
    }
    return lo;
}

static void remove_npc_state(RcWorld *world, int index) {
    if (index < 0 || index >= world->dormant_npc_count) return;
    memmove(&world->dormant_npcs[index], &world->dormant_npcs[index + 1],
            (size_t)(world->dormant_npc_count - index - 1)
                * sizeof(*world->dormant_npcs));
    world->dormant_npc_count--;
}

static struct RcDormantNpcState npc_state_from_active(
    const RcWorld *world, const RcNpc *npc) {
    return (struct RcDormantNpcState){
        .spawn_key = npc->spawn_key,
        .saved_tick = world->tick,
        .npc = *npc,
    };
}

int rc_world_state_save_npcs(RcWorld *world) {
    if (!world) return -1;
    if (!reserve_npcs(world, world->dormant_npc_count + world->npc_count))
        return -1;

    int saved = 0;
    for (int i = 0; i < world->npc_count; i++) {
        const RcNpc *npc = &world->npcs[i];
        if (!npc->active || npc->spawn_key == 0) continue;
        bool found = false;
        int at = npc_lower_bound(world, npc->spawn_key, &found);
        struct RcDormantNpcState state = npc_state_from_active(world, npc);
        if (found) {
            world->dormant_npcs[at] = state;
        } else {
            memmove(&world->dormant_npcs[at + 1],
                    &world->dormant_npcs[at],
                    (size_t)(world->dormant_npc_count - at)
                        * sizeof(*world->dormant_npcs));
            world->dormant_npcs[at] = state;
            world->dormant_npc_count++;
        }
        saved++;
    }
    return saved;
}

static void reconcile_npc_elapsed(RcWorld *world, RcNpc *npc,
                                  RcTick elapsed) {
    if (!npc || !npc->is_dead || elapsed == 0) return;
    RcTick death = npc->death_timer > 0 ? (RcTick)npc->death_timer : 0;
    if (elapsed <= death) {
        npc->death_timer -= (int)elapsed;
        return;
    }
    elapsed -= death;
    npc->death_timer = 0;
    if (!npc->respawns) return;
    RcTick respawn = npc->respawn_timer > 0
                   ? (RcTick)npc->respawn_timer : 0;
    if (elapsed < respawn) {
        npc->respawn_timer -= (int)elapsed;
        return;
    }
    rc_npc_reset_life(world, npc);
}

static void apply_npc_state(RcWorld *world, RcNpc *npc,
                            const struct RcDormantNpcState *state) {
    *npc = state->npc;
    RcTick elapsed = world->tick >= state->saved_tick
                   ? world->tick - state->saved_tick : 0;
    reconcile_npc_elapsed(world, npc, elapsed);
}

int rc_world_state_restore_npcs(RcWorld *world) {
    if (!world) return -1;
    int restored = 0;
    for (int i = 0; i < world->npc_count; i++) {
        RcNpc *npc = &world->npcs[i];
        if (!npc->active || npc->spawn_key == 0) continue;
        bool found = false;
        int at = npc_lower_bound(world, npc->spawn_key, &found);
        if (!found) continue;
        apply_npc_state(world, npc, &world->dormant_npcs[at]);
        remove_npc_state(world, at);
        restored++;
    }
    return restored;
}

static void clear_player_combat(RcNpc *npc) {
    npc->target_uid = -1;
    if (npc->facing_entity == 0) npc->facing_entity = -1;
    if (npc->route_mode == RC_NPC_ROUTE_CHASE)
        rc_npc_route_clear(npc, RC_MOVEMENT_NONE);
    npc->combat.active = false;
    npc->combat.flags = 0;
    npc->combat.distance_to_target = -1;
    npc->combat.attack_prepared = false;
    npc->combat.target = (RcCombatTargetRef){0};
    npc->combat.attacker_count = npc->combat.under_attack_timer = 0;
    npc->combat.primary_attacker = (RcCombatActorRef){RC_COMBAT_ACTOR_NONE, -1};
}

void rc_world_state_clear_player_combat(RcWorld *world) {
    if (!world) return;
    for (int i = 0; i < world->npc_count; i++)
        clear_player_combat(&world->npcs[i]);
    for (int i = 0; i < world->dormant_npc_count; i++)
        clear_player_combat(&world->dormant_npcs[i].npc);
}

static int ground_compare(bool static_a, uint64_t key_a,
                          bool static_b, uint64_t key_b) {
    if (static_a != static_b) return static_a ? -1 : 1;
    return key_a < key_b ? -1 : key_a > key_b;
}

static int ground_lower_bound(const RcWorld *world, bool static_spawn,
                              uint64_t key, bool *found) {
    int lo = 0;
    int hi = world->dormant_ground_item_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        const struct RcDormantGroundItemState *state =
            &world->dormant_ground_items[mid];
        if (ground_compare(state->static_spawn, state->key,
                           static_spawn, key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (found) {
        *found = lo < world->dormant_ground_item_count
              && ground_compare(
                    world->dormant_ground_items[lo].static_spawn,
                    world->dormant_ground_items[lo].key,
                    static_spawn, key) == 0;
    }
    return lo;
}

static void remove_ground_state(RcWorld *world, int index) {
    if (index < 0 || index >= world->dormant_ground_item_count) return;
    memmove(&world->dormant_ground_items[index],
            &world->dormant_ground_items[index + 1],
            (size_t)(world->dormant_ground_item_count - index - 1)
                * sizeof(*world->dormant_ground_items));
    world->dormant_ground_item_count--;
}

static int upsert_ground_state(RcWorld *world, bool static_spawn,
                               uint64_t key, const RcGroundItem *item) {
    bool found = false;
    int at = ground_lower_bound(world, static_spawn, key, &found);
    struct RcDormantGroundItemState state = {
        .key = key,
        .saved_tick = world->tick,
        .static_spawn = static_spawn,
        .item = *item,
    };
    if (found) {
        world->dormant_ground_items[at] = state;
        return 1;
    }
    memmove(&world->dormant_ground_items[at + 1],
            &world->dormant_ground_items[at],
            (size_t)(world->dormant_ground_item_count - at)
                * sizeof(*world->dormant_ground_items));
    world->dormant_ground_items[at] = state;
    world->dormant_ground_item_count++;
    return 1;
}

static int ground_item_is_default(const RcGroundItem *item) {
    return item->active && item->static_spawn && item->spawn_key != 0
        && item->quantity == item->spawn_quantity
        && item->owner_uid == RC_GROUND_OWNER_NONE
        && item->original_owner_uid == RC_GROUND_OWNER_NONE
        && item->visibility == RC_GROUND_VIS_PUBLIC
        && item->reveal_timer == 0 && item->despawn_timer == 0;
}

static int point_in_area(int x, int y, int plane, const RcTileRect *bounds,
                         int min_plane, int max_plane) {
    return rc_tile_rect_contains(bounds, x, y)
        && plane >= min_plane && plane <= max_plane;
}

int rc_world_state_save_ground_items(RcWorld *world,
                                     int min_x, int min_y,
                                     int max_x, int max_y,
                                     int min_plane, int max_plane) {
    RcTileRect bounds;
    if (!world || !rc_tile_rect_make(min_x, min_y, max_x, max_y, &bounds)
            || !rc_plane_valid(min_plane) || !rc_plane_valid(max_plane)
            || min_plane > max_plane) {
        return -1;
    }
    if (!reserve_ground_items(
            world, world->dormant_ground_item_count
                 + world->ground_item_count)) {
        return -1;
    }

    int saved = 0;
    for (int i = 0; i < world->ground_item_count; i++) {
        RcGroundItem *item = &world->ground_items[i];
        if (item->static_spawn && item->spawn_key != 0) {
            bool found = false;
            int at = ground_lower_bound(world, true, item->spawn_key, &found);
            if (ground_item_is_default(item)) {
                if (found) remove_ground_state(world, at);
            } else {
                upsert_ground_state(world, true, item->spawn_key, item);
                saved++;
            }
            continue;
        }
        if (!item->active || point_in_area(item->x, item->y, item->plane,
                                           &bounds,
                                           min_plane, max_plane)) {
            continue;
        }
        RcGroundItem state = *item;
        state.version++;
        if (state.version <= 0) state.version = 1;
        upsert_ground_state(world, false, (uint64_t)(uint32_t)item->uid,
                            &state);
        item->active = false;
        item->quantity = 0;
        item->version = state.version;
        saved++;
    }
    return saved;
}

static RcTick elapsed_ground_ticks(
    const RcWorld *world, const struct RcDormantGroundItemState *state) {
    RcTick start = state->saved_tick;
    if (state->item.timer_start_tick > start)
        start = state->item.timer_start_tick;
    return world->tick > start ? world->tick - start : 0;
}

static void apply_static_ground_state(
    RcWorld *world, RcGroundItem *item,
    const struct RcDormantGroundItemState *state) {
    int version = item->version + 1;
    RcGroundItem restored = state->item;
    rc_ground_item_advance(&restored, elapsed_ground_ticks(world, state));
    item->quantity = restored.quantity;
    item->state_id = restored.state_id;
    item->owner_uid = restored.owner_uid;
    item->original_owner_uid = restored.original_owner_uid;
    item->reveal_timer = restored.reveal_timer;
    item->despawn_timer = restored.despawn_timer;
    item->respawn_timer = restored.respawn_timer;
    item->arrival_timer = restored.arrival_timer;
    item->visibility = restored.visibility;
    item->active = restored.active;
    item->version = version > 0 ? version : 1;
}

static int inactive_ground_slot(RcWorld *world) {
    for (int i = 0; i < world->ground_item_count; i++) {
        if (!world->ground_items[i].active
                && !world->ground_items[i].static_spawn) {
            return i;
        }
    }
    if (world->ground_item_count >= RC_MAX_GROUND_ITEMS) return -1;
    return world->ground_item_count++;
}

int rc_world_state_restore_ground_items(RcWorld *world,
                                        int min_x, int min_y,
                                        int max_x, int max_y,
                                        int min_plane, int max_plane) {
    RcTileRect bounds;
    if (!world || !rc_tile_rect_make(min_x, min_y, max_x, max_y, &bounds)
            || !rc_plane_valid(min_plane) || !rc_plane_valid(max_plane)
            || min_plane > max_plane) {
        return -1;
    }
    int restored = 0;
    for (int i = 0; i < world->ground_item_count; i++) {
        RcGroundItem *item = &world->ground_items[i];
        if (!item->active || !item->static_spawn || item->spawn_key == 0)
            continue;
        bool found = false;
        int at = ground_lower_bound(world, true, item->spawn_key, &found);
        if (!found) continue;
        apply_static_ground_state(
            world, item, &world->dormant_ground_items[at]);
        remove_ground_state(world, at);
        restored++;
    }

    for (int i = 0; i < world->dormant_ground_item_count;) {
        struct RcDormantGroundItemState *state =
            &world->dormant_ground_items[i];
        if (state->static_spawn || !point_in_area(
                state->item.x, state->item.y, state->item.plane,
                &bounds, min_plane, max_plane)) {
            i++;
            continue;
        }
        RcGroundItem restored_item = state->item;
        rc_ground_item_advance(
            &restored_item, elapsed_ground_ticks(world, state));
        if (!restored_item.active) {
            remove_ground_state(world, i);
            world->streaming_telemetry.expired_ground_items++;
            continue;
        }
        int slot = inactive_ground_slot(world);
        if (slot < 0) {
            return -1;
        }
        world->ground_items[slot] = restored_item;
        world->ground_items[slot].active = true;
        world->ground_items[slot].static_spawn = false;
        world->ground_items[slot].spawn_key = 0;
        world->ground_items[slot].spawn_quantity = 0;
        if (world->next_ground_item_uid <= state->item.uid)
            world->next_ground_item_uid = state->item.uid + 1;
        remove_ground_state(world, i);
        restored++;
    }
    return restored;
}

void rc_world_state_destroy(RcWorld *world) {
    if (!world) return;
    free(world->dormant_npcs);
    free(world->dormant_ground_items);
    world->dormant_npcs = NULL;
    world->dormant_ground_items = NULL;
    world->dormant_npc_count = 0;
    world->dormant_ground_item_count = 0;
    world->dormant_npc_capacity = 0;
    world->dormant_ground_item_capacity = 0;
}

int rc_world_state_clone_dormant(RcWorld *dst, const RcWorld *src) {
    if (!dst || !src) return 0;
    dst->dormant_npcs = NULL;
    dst->dormant_ground_items = NULL;
    dst->dormant_npc_count = src->dormant_npc_count;
    dst->dormant_ground_item_count = src->dormant_ground_item_count;
    dst->dormant_npc_capacity = src->dormant_npc_count;
    dst->dormant_ground_item_capacity = src->dormant_ground_item_count;
    if (src->dormant_npc_count > 0) {
        dst->dormant_npcs = malloc(
            (size_t)src->dormant_npc_count * sizeof(*dst->dormant_npcs));
        if (!dst->dormant_npcs) goto fail;
        memcpy(dst->dormant_npcs, src->dormant_npcs,
               (size_t)src->dormant_npc_count * sizeof(*dst->dormant_npcs));
    }
    if (src->dormant_ground_item_count > 0) {
        dst->dormant_ground_items = malloc(
            (size_t)src->dormant_ground_item_count
                * sizeof(*dst->dormant_ground_items));
        if (!dst->dormant_ground_items) goto fail;
        memcpy(dst->dormant_ground_items, src->dormant_ground_items,
               (size_t)src->dormant_ground_item_count
                * sizeof(*dst->dormant_ground_items));
    }
    return 1;

fail:
    rc_world_state_discard_dormant(dst);
    return 0;
}

void rc_world_state_discard_dormant(RcWorld *world) {
    if (!world) return;
    free(world->dormant_npcs);
    free(world->dormant_ground_items);
    world->dormant_npcs = NULL;
    world->dormant_ground_items = NULL;
    world->dormant_npc_count = 0;
    world->dormant_ground_item_count = 0;
    world->dormant_npc_capacity = 0;
    world->dormant_ground_item_capacity = 0;
}
