#ifndef RC_SNAPSHOT_H
#define RC_SNAPSHOT_H

// World snapshots and canonical state hashes.
//
// A snapshot is a deep copy of a world's mutable state: players, NPCs (live and dormant), ground
// items, object states, collision, shops, encounter and boss state, timers, every RNG stream and
// the tick. Restoring it into the same world (same game data and NPC capacity) resumes exactly
// where it was taken: stepping both with the same actions gives the same states. Content hooks
// and handlers are code, identical for every world of a process, and are not part of it.
//
// The hash covers canonical gameplay fields (never addresses or padding), so identical seeds and
// action traces give identical hashes.

#include "types.h"

#include <stddef.h>
#include <stdint.h>

typedef struct RcWorldSnapshot RcWorldSnapshot;

RcWorldSnapshot *rc_world_snapshot(const RcWorld *world);
int rc_world_restore(RcWorld *world, const RcWorldSnapshot *snapshot);
void rc_world_snapshot_free(RcWorldSnapshot *snapshot);
size_t rc_world_snapshot_bytes(const RcWorldSnapshot *snapshot);

uint64_t rc_world_state_hash(const RcWorld *world);

#endif
