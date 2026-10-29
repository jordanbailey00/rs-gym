#ifndef RC_PLAYERS_H
#define RC_PLAYERS_H

// Several players per world. `world->player` is the player the engine is acting for; these
// helpers address the others. A player's uid in combat actor refs, NPC targets and ground-item
// owners is its slot index.

#include "types.h"

// Player in slot `idx`, or NULL if the slot is empty.
static inline RcPlayer *rc_world_player_at(RcWorld *world, int idx) {
    return world && idx >= 0 && idx < world->player_count && world->players[idx].active
        ? &world->players[idx] : NULL;
}

static inline int rc_player_slot(const RcWorld *world, const RcPlayer *player) {
    return (int)(player - world->players);
}

// RcPendingHit.source_idx for a hit landed by player `idx` (players encode as -16..-31).
#define RC_HIT_SOURCE_PLAYER_BASE (-16)
static inline int rc_hit_source_player(int idx) { return RC_HIT_SOURCE_PLAYER_BASE - idx; }
static inline int rc_hit_source_player_slot(int source) {
    return source <= RC_HIT_SOURCE_PLAYER_BASE && source > RC_HIT_SOURCE_PLAYER_BASE - RC_MAX_PLAYERS
        ? RC_HIT_SOURCE_PLAYER_BASE - source : -1;
}

// Makes slot `idx` the acting player; returns the previous slot, or -1 if `idx` is not a player.
int rc_world_select_player(RcWorld *world, int idx);

#endif
