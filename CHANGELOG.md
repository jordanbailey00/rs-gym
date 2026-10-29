# Changelog

## 2026-10-25: v1 implementation

**What.** All six v1 milestones: a dense island plus dungeon, up to 16 players per world, six
skills, shops, death rules, two bosses, and an RL interface with a PufferLib 5.0 binding, replays
and a benchmark. Not yet tested by hand in the viewer or trained.

**Why.** v1 is meant to be a complete small game that single- and multi-agent RL methods can be
compared on, fast enough for hour-long experiments.

**World (`maps/gym_v1.toml`, `tools/gymmap`).**
- Spec v1: named areas (island and void), rivers, paint rects, stamps in any area, traversal
  links, zones (wilderness bands, multi-combat), fixed and shoreline NPC placement, static ground
  items, gym drop tables, spawned-loc templates and named landmarks.
- New compiler modules: `spatial.py` (traversal and area-flag tables), `nodes.py` (gathering
  nodes, now with stump and empty-rock replacements), `drops.py` (merges gym drop tables into the
  source table).
- Real stair and ladder rows move with each stamp; spec links add the dungeon entrances.
- `build_gym.py` writes `data/regions/gym.cfg` (start, respawn, the loaded mapsquare rect, landmarks)
  and the spawned-loc templates (`tools/cache_pipeline/export_spawn_objects.py`, copied from
  RuneC_v4 and changed to read the spec). `export_objects.py` gives depletable nodes their stump or
  empty-rock replacement mesh.

**Engine (`rc-core`).**
- Players: a `players[16]` array with `world->player` as the acting player; per-player ticks,
  interactions, command queues, banks, NPC targeting, hit sources and loot credit. The whole gym
  stays loaded (`pin_area`). `layout.c` reads `gym.cfg`; `rc_gym_world_config` is the one gym config
  for the viewer, tests and rc-env.
- `skilling.c`: per-player skilling jobs (gather, fish, firemaking, cook, make) with OSRS success
  curves; furnace/anvil products come from recipes.bin.
- `shops.c`: live shops with shared stock, stock-based prices and restocking.
- `death.c`: keep-3 (Protect Item), gravestones, wilderness drops, timed respawn.
- New player commands for making, lighting fires and shop buy/sell, so viewer and agent input queue
  for the next tick like every other command.
- `rc_world_reset_seeded` resets as a new world (RNG and NPC ids start over) for exact replays.
- Fixes found on the way:
  - Deaths are checked before prayer drain and regen, so Protect Item counts and regen cannot
    revive a killed player.
  - A handler's own result no longer replaces the chat message its game code published.
- Speed and threads:
  - Route searches use a 128×128 window (OSRS-sized) instead of 512×512.
  - Collision lookups remember the last map region.
  - The shared object-placement cache can be frozen for safe reads from many threads.
  - The combat-rejection log line only prints with `RC_DEBUG_COMBAT`.

**Content (`rc-content`).** `skilling.c` (tools, rates, fishing, logs, cooking, smithing products
and their handlers); `gym.c` (three shops, bone burying, death rules).

**RL (`rc-env`, new).** The C API, observation and action layouts, masks, ten task presets,
per-landmark travel fields, replays, `rs-gym-bench`, `rs-gym-rollout`, and the PufferLib binding with
`tools/puffer_overlay.sh`.

**Viewer.**
- The default player is a fresh gym player with the starter kit (dev 99s and bank:
  `RUNEC_DEV_VALIDATION=1`).
- A furnace/anvil make menu in the chatbox and a shop window in the bank panel.
- Skilling animations and Bury in the item menu.
- `--replay FILE`, with keys 1–9 to follow an agent; other players are drawn.

**Decisions.**
- Agents act through a flat 453-action masked space with entity lists, not raw tile clicks: walk
  hops and landmark travel keep every route short, which keeps stepping fast and learnable.
- Every route the engine searches stays OSRS-faithful (same BFS, smaller window).
- Shops, death and skilling rules are content tables; the engine holds the mechanics.
- I did not build against or link anything into an external PufferLib checkout; the overlay script
  does that when you run it.

**Verified.**
- `ctest`: 8 suites. Compiler tests (15), world, multiplayer, skilling (chop, mine, fish, light,
  cook, smelt, smith), rules (shops, death, burying), bosses (dragonfire with and without the
  shield, KBD drop, Chaos Elemental teleport and disarm), env (masked random play, determinism
  across envs and resets, scripted woodcutting reward, multi-agent episodes, boss death,
  record-and-replay), PufferLib binding (stand-in header).
- ThreadSanitizer is clean across all tasks with 4 threads.
- Viewer smoke run and a rendered replay frame.

**Known gaps.** See `TODO.md`.

## 2026-10-24: v1 spec

**What.** v0 is done and kept on branch `v0` (`ea600a0`). `SPEC.md` now defines v1 and v2, and
`TODO.md` is organised by v1 milestone.

**Why.** This is the agreed scope.
- v1 is a complete small game for single- and multi-agent RL, meant to become the main environment
  for new RL experiments.
- Trading, PvP, more monsters and encounters, and additional skills (still to be chosen) are v2.
- Quests, farming and minigames are not planned.

**Decisions.**
- The RL interface and several players per world are part of v1, so v1 can be used for
  experiments on its own.
- The multi-player engine change comes before the new systems, so skilling, shops and death are
  written for many players from the start.

## 2026-10-24: v0 island gym

**What.** New repo. A map compiler builds a 192×192 island out of b237 cache content; the copied
RuneC exporters turn it into runtime data; the copied engine simulates it and the copied viewer
shows it.

**Why.** Full OSRS is too large for agents to explore. A small, dense map keeps the interesting
systems close together. RS-gym is self-contained: it copies what it needs and never calls RuneC.

**Copied from RuneC_v4 at `338f2e6`:**
- `rc-core/`, `rc-content/`, `rc-viewer/`, `lib/raylib/`.
- The export closure: `tools/cache_pipeline/` (`rc_cache`, `export_scene_slice`, `export_terrain`,
  `export_objects`, `export_minimap`, `export_models`, `export_textures`, `export_collision_map`,
  `export_collision_map_modern`, `modern_cache_reader`, `source_inputs`), plus `tools/`
  `export_collision_tiles`, `export_object_placements`, `collision_tile_index`,
  `object_placement_index`, `region_sets`, `source_paths`, `spawn_index`.
- Not copied: RuneC's tests, guard scripts, docs and authored `content/` tree.

**Changed in the copies:**
- `rc-content/combat/magic.c`: includes `npc_spell_targets.inc` from `rc-content/combat/`, where it was
  moved from RuneC's `content/combat/`. It was the only include outside the three C components.
- `rc-viewer/viewer.c`, `dev_validation.{c,h}`:
  - Defaults now start at the gym (world origin 64,64; start 222,222 via `RS_GYM_START_X/Y`).
  - A failed start relocation exits with an error instead of leaving the player in the void.
  - The dev panel's real-world boss teleports and encounter tables are replaced by one Start
    teleport.
  - The Varrock bank combat dummy now spawns beside the gym start.
- `tools/export_collision_tiles.py`, `tools/export_object_placements.py`: reports go to
  `build/reports/` instead of the source tree.
- Removed `rc_cache/smoke.py`, `rc-core/AGENT.md` and the nested `rc-content` READMEs.
- `CMakeLists.txt` was rewritten to build only the three components and the two gym tests.

**New:**
- `tools/gymmap/`:
  - `mapformat.py` encodes and decodes b237 terrain and loc files.
  - `cachewrite.py` writes an overlay cache: the map index is replaced and every other index links
    to the source cache.
  - `compile.py` handles the spec: island terrain, paths, rotated stamps of real-world rects with
    height fitting, loc and scatter placement, NPC spawn placement.
- `tools/build_gym.py` runs compile → overlay cache → exporters → spawn files → empty area flags →
  `manifest.json`.
- `maps/gym_v0.toml`, `tests/test_gymmap.py`, `tests/test_gym_world.c`, and the docs.

**Data (local, git-ignored):**
- Copied the b237 cache to `data/source/`.
- Copied defs, models, sprites, fonts, ui and anims from RuneC_v4's installed
  `data-v2026.10.22-b237` release. Left out the legacy `collision_tiles.bin`, the Varrock NPC model
  bundles, `npcs.anims`, `player.anims` and `encounter_effects.atlas`, which nothing in RS-gym reads.
- Gym spatial data is generated.

**Verified:**
- All 2,869 real mapsquares round-trip through the encoder (4.88M locs).
- 10 Python tests and the C gym-world test pass under `ctest`.
- `RUNEC_VIEWER_SMOKE=1` passes with 26 NPCs.
- A rendered frame shows the island, buildings, NPCs and UI.

**Known gaps:**
- Skilling, shops, death rules, stairs and ladders, area flags and multi-player are not in v0 (see
  SPEC and TODO).
- `data/` cannot yet be rebuilt from inside RS-gym.
