# rc-core

The headless C11 game engine. It owns the tick loop, movement and pathfinding, combat (melee, ranged
and magic), prayer, items, equipment, consumables, loot, banking, skilling, shops, death, objects,
NPCs and collision. It has no rendering and no gym-specific tables; those live in `rc-content`.

Copied from RuneC_v4 at `338f2e6`; RS-gym keeps its own copy (changes in `CHANGELOG.md`).

## Use

```c
RcGymLayout layout;
rc_gym_layout_load(RC_GYM_LAYOUT_PATH, &layout);          // data/regions/gym.cfg
RcWorldConfig cfg = rc_gym_world_config(&layout, 4);      // the gym, 4 players
RcWorld *w = rc_world_create_config(&cfg);
rc_world_install_content(w, rc_content_register_all);
for (int i = 0; i < 4; i++) {
    rc_world_select_player(w, i);                         // commands act for the selected player
    rc_world_relocate_player(w, layout.start_x + i, layout.start_y, 0);  // loads the gym
}
rc_world_select_player(w, 0);
rc_player_walk_to(w, layout.start_x + 8, layout.start_y); // queued for the next tick
rc_world_tick(w);
```

For RL use `rc-env`, which wraps all of this.

## Gym additions

- Several players: `players[RC_MAX_PLAYERS]`, with `world->player` the acting player (the selected
  one outside a tick, each in turn during per-player phases, an NPC's target while it runs).
  `players.h` has the helpers.
- `layout.c`: the gym layout file and `rc_gym_world_config`.
- `skilling.c`: skilling jobs (gather, fish, firemaking, cook, furnace/anvil make) using content rules.
- `shops.c`: live shops (shared stock, stock-based prices, restocking) on top of the shops.bin loader.
- `death.c`: keep-3 items, gravestones and wilderness drops, timed respawn.
- `rc_world_reset_seeded`: reset as a freshly created world with a new seed.
- `rc_object_placements_freeze`: makes the shared placement cache read-only for multi-threaded use.

## Data

Paths resolve under `RUNEC_DATA_ROOT` (default `data`). Definitions come from `data/defs`;
`tools/build_gym.py` generates the gym's spatial files (collision, placements, spawns, zones,
traversals, gathering nodes, drops, `regions/gym.cfg`). Startup checks `data/manifest.json` for
`"format": "runec-data-manifest-v1"` and `"data_version": "v1"`.

## Limits

- At most 32 mapsquares stay loaded (`RC_MAX_REGIONS`); the gym uses 24.
- Routes search a 128×128 window around the mover, so one click reaches at most 64 tiles.
- Object states (depleted nodes, fires, doors) are capped at 512 per world, ground items at 512.
- Thread safety: worlds can step on different threads once the placement cache is frozen (rc-env
  does this); data loading itself is single-threaded.
