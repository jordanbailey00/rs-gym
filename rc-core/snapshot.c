#include "snapshot.h"

#include "world_state.h"

#include <stdlib.h>
#include <string.h>

struct RcWorldSnapshot {
    RcWorld world;   // the struct as it was; its pointers are rebased on restore
    RcNpc *npcs;
    int player_index;
};

RcWorldSnapshot *rc_world_snapshot(const RcWorld *world) {
    if (!world || world->in_tick || !world->npcs) return NULL;
    RcWorldSnapshot *s = malloc(sizeof(*s));
    if (!s) return NULL;
    memcpy(&s->world, world, sizeof(*world));
    s->npcs = malloc((size_t)world->npc_capacity * sizeof(RcNpc));
    if (!s->npcs || !rc_world_state_clone_dormant(&s->world, world)) {
        free(s->npcs);
        free(s);
        return NULL;
    }
    memcpy(s->npcs, world->npcs, (size_t)world->npc_capacity * sizeof(RcNpc));
    s->player_index = (int)(world->player - world->players);
    return s;
}

int rc_world_restore(RcWorld *world, const RcWorldSnapshot *s) {
    if (!world || !s || world->in_tick || world->npc_capacity != s->world.npc_capacity
            || world->game_data != s->world.game_data)
        return 0;
    RcNpc *npcs = world->npcs;
    rc_world_state_discard_dormant(world);
    memcpy(world, &s->world, sizeof(*world));
    world->npcs = npcs;
    memcpy(npcs, s->npcs, (size_t)world->npc_capacity * sizeof(RcNpc));
    world->player = &world->players[s->player_index];
    // The copy still points at the snapshot's dormant arrays: give the world its own.
    world->dormant_npcs = NULL;
    world->dormant_ground_items = NULL;
    return rc_world_state_clone_dormant(world, &s->world);
}

void rc_world_snapshot_free(RcWorldSnapshot *s) {
    if (!s) return;
    free(s->npcs);
    free(s->world.dormant_npcs);
    free(s->world.dormant_ground_items);
    free(s);
}

size_t rc_world_snapshot_bytes(const RcWorldSnapshot *s) {
    return s ? sizeof(*s) + (size_t)s->world.npc_capacity * sizeof(RcNpc) : 0;
}

// ---- Canonical hash (FNV-1a over explicitly listed fields) ---------------------------------------

typedef struct {
    uint64_t h;
} Hash;

static void mix(Hash *h, int64_t v) {
    uint64_t x = (uint64_t)v;
    for (int i = 0; i < 8; i++) {
        h->h ^= (x >> (i * 8)) & 0xff;
        h->h *= 0x100000001b3ULL;
    }
}

static void mix_slots(Hash *h, const RcInvSlot *slots, int n) {
    for (int i = 0; i < n; i++) {
        mix(h, slots[i].item_id);
        if (slots[i].item_id < 0) continue;
        mix(h, slots[i].quantity);
        mix(h, slots[i].state_id);
    }
}

static void mix_player(Hash *h, const RcPlayer *p) {
    mix(h, p->active);
    if (!p->active) return;
    mix(h, p->x); mix(h, p->y); mix(h, p->plane);
    mix(h, p->current_hp); mix(h, p->max_hp); mix(h, p->is_dead); mix(h, p->respawn_tick);
    mix(h, p->current_prayer_points); mix(h, p->active_prayers);
    mix(h, p->run_energy); mix(h, p->running); mix(h, p->special_energy);
    mix(h, p->attack_target); mix(h, p->attack_timer); mix(h, p->combat_style);
    mix(h, p->attack_style_idx); mix(h, p->autocast_spell); mix(h, p->manual_spell_cast);
    mix(h, p->route_idx); mix(h, p->route_len);
    for (int s = 0; s < SKILL_COUNT; s++) {
        mix(h, p->skills.xp[s]); mix(h, p->skills.xp_hundredths[s]);
        mix(h, p->skills.base_level[s]); mix(h, p->skills.boosted_level[s]);
    }
    mix_slots(h, p->inventory, RC_INVENTORY_SIZE);
    mix_slots(h, p->equipment, RC_EQUIP_COUNT);
    mix_slots(h, p->bank, RC_BANK_SIZE);
    mix(h, p->skill_action); mix(h, p->skill_ready_tick); mix(h, p->skill_job.kind);
    mix(h, p->skill_job.rule); mix(h, p->make_menu); mix(h, p->make_station); mix(h, p->shop_open);
    mix(h, p->storage_kind);
    mix(h, p->trade.stage); mix(h, p->trade.partner); mix(h, p->trade.revision);
    mix(h, p->trade.accepted_revision);
    mix_slots(h, p->trade.offer, p->trade.offer_count);
    mix(h, p->num_pending_hits);
    for (int i = 0; i < p->num_pending_hits && i < RC_MAX_PENDING_HITS; i++) {
        mix(h, p->pending_hits[i].damage);
        mix(h, p->pending_hits[i].apply_tick);
    }
}

static void mix_npc(Hash *h, const RcNpc *n) {
    mix(h, n->active);
    if (!n->active) return;
    mix(h, n->uid); mix(h, n->def_id); mix(h, n->x); mix(h, n->y); mix(h, n->plane);
    mix(h, n->current_hp); mix(h, n->spawn_hp); mix(h, n->is_dead); mix(h, n->target_uid);
    mix(h, n->attack_timer); mix(h, n->death_timer);
    for (int i = 0; i < 6; i++) mix(h, n->stats[i]);
    for (int i = 0; i < RC_MAX_PLAYERS; i++) mix(h, n->player_damage[i]);
    mix(h, n->num_pending_hits);
}

uint64_t rc_world_state_hash(const RcWorld *w) {
    Hash h = {0xcbf29ce484222325ULL};
    if (!w) return h.h;
    mix(&h, w->tick); mix(&h, w->rng_state); mix(&h, w->player_count);
    for (int i = 0; i < w->player_count; i++) mix_player(&h, &w->players[i]);
    mix(&h, w->npc_count);
    for (int i = 0; i < w->npc_count; i++) mix_npc(&h, &w->npcs[i]);
    mix(&h, w->ground_item_count);
    for (int i = 0; i < w->ground_item_count; i++) {
        const RcGroundItem *g = &w->ground_items[i];
        mix(&h, g->active);
        if (!g->active) continue;
        mix(&h, g->item_id); mix(&h, g->quantity); mix(&h, g->state_id); mix(&h, g->x); mix(&h, g->y);
        mix(&h, g->plane); mix(&h, g->owner_uid); mix(&h, g->visibility); mix(&h, g->reveal_timer);
        mix(&h, g->despawn_timer);
    }
    mix(&h, w->object_state_count);
    for (int i = 0; i < w->object_state_count; i++) {
        const RcObjectState *o = &w->object_states[i];
        mix(&h, (int64_t)o->placement_key); mix(&h, o->flags); mix(&h, o->active_obj_id);
        mix(&h, o->active_x); mix(&h, o->active_y); mix(&h, o->revert_tick);
    }
    for (int s = 0; s < w->shop_count; s++)
        for (int i = 0; i < w->shops[s].slot_count; i++) {
            mix(&h, w->shops[s].slots[i].item_id);
            mix(&h, w->shops[s].slots[i].stock);
            mix(&h, w->shops[s].slots[i].next_restock);
        }
    const RcModeBossState *b = &w->mode_boss;
    mix(&h, b->active); mix(&h, b->phase); mix(&h, b->participants); mix(&h, b->max_hp);
    mix(&h, b->feeding_pile); mix(&h, b->moving_to); mix(&h, b->heals_left); mix(&h, b->summons);
    mix(&h, b->hazard_count); mix(&h, b->minion_count); mix(&h, b->kills); mix(&h, b->rotation);
    for (int i = 0; i < b->hazard_count; i++) {
        mix(&h, b->hazards[i].x); mix(&h, b->hazards[i].y); mix(&h, b->hazards[i].land_tick);
    }
    mix(&h, w->trades_completed);
    return h.h;
}
