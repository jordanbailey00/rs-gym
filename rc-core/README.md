# rc-core

The headless C11 game engine. It owns the tick loop, movement and pathfinding, combat (melee, ranged
and magic), prayer, items, equipment, consumables, loot, banking, objects, NPCs and collision. It
has no rendering and no OSRS-specific scripts; those live in `rc-content`.

Copied from RuneC_v4 at `338f2e6`. RS-gym keeps its own copy.

## Use

```c
RcWorldConfig cfg = rc_preset_base_only();   // then set subsystems and data paths
RcWorld *w = rc_world_create_config(&cfg);
rc_world_install_content(w, rc_content_register_all);
rc_world_relocate_player(w, 222, 222, 0);    // the gym start; needed after create and reset
rc_player_walk_to(w, 230, 222);
rc_world_tick(w);
```

`tests/test_gym_world.c` shows the full gym configuration. It matches `viewer_world_config` in
`rc-viewer/viewer.c`.

## Data

Paths resolve under `RUNEC_DATA_ROOT` (default `data`). Definitions come from `data/defs`.
`tools/build_gym.py` generates the gym's spatial files:

- `data/regions/world.collision-tiles.indexed.bin`
- `data/regions/world.object-placements.indexed.bin`
- `data/spawns/world.npc-spawns.indexed.bin`
- `data/spawns/world.ground-items.indexed.bin`
- `data/defs/area_flags.bin`

Startup also checks `data/manifest.json` for `"format": "runec-data-manifest-v1"` and
`"data_version": "v1"`.

## Limits that matter for the gym

- It simulates at most a 5×5-mapsquare window around the player and re-activates it on every
  mapsquare crossing.
- A world holds one player, so there is no trading or PvP.
- Skilling only depletes nodes, shops cannot buy or sell, and death has no item loss or respawn point.
- `rc_world_reset` restores the same seed and the default position (3213, 3428). Relocate after reset.
- The shared collision and object page caches are not thread-safe. Run worlds on one thread.
