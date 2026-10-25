# Changelog

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
