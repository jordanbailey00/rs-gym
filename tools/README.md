# tools

Python tooling that turns a map spec into runtime data, plus the PufferLib overlay script.

## `build_mode.py`

```bash
python3 tools/build_mode.py [mode]        # default compact_osrs_v1
```

Steps, in order (output in `data/modes/<mode>/`):
1. Compiles the mode's map spec with `gymmap` and writes the overlay cache to
   `build/modes/<mode>/cache`.
2. Exports object placements; compiles the mode's rules with `modec` (when it has
   `content.toml`); writes the gathering-node table.
3. Exports `regions/` (terrain, objects, minimaps, collision). Depletable nodes get separate
   meshes so the viewer can swap in the depleted loc.
4. Exports render templates for locs spawned at runtime (`[[spawn_objects]]`: fires).
5. Copies the mode's `combat_visuals.tsv` and exports skeletal animation (`models/skeletal.bin`)
   for its NPCs and the spot animations that table names.
6. Writes the roof bitmaps, spawns, traversal, area-flag and drop tables and `gym.cfg` (start,
   respawn, loaded mapsquares, landmarks), then runs `rs-gym-validate` for the manifest and
   override ledger.

## `gymmap/`: the map compiler

- `mapformat.py` reads and writes b237 mapsquare terrain and loc files.
- `compile.py` turns a spec into mapsquares and tables: areas, island terrain, rivers, paths,
  rotated stamps (heights fitted, their stair rows moved along, decorative swaps, coast clipping,
  blended edges), locs, fences, scatter, ground decoration, links, zones, NPC and item spawns,
  landmarks.
- `shapes.py` gives smooth floors their edges: curves, value noise, and the client's 12 overlay
  shapes fitted to each edge tile from a 4x4 sample of the region.
- `cachewrite.py` writes an OpenRS2-layout cache whose map index holds only the gym; every other
  index links back to `data/source/.../cache`.
- `spatial.py` writes the traversal (stairs, ladders, links) and area-flag (zones) tables.
- `nodes.py` writes the gathering-node table; `drops.py` merges the spec's drop tables.

## Export pipeline (copied from RuneC_v4)

- `cache_pipeline/rc_cache/` reads the cache.
- `cache_pipeline/export_scene_slice.py` writes per-mapsquare terrain, objects, object animations
  and minimaps, plus the catalog and material atlas.
- `cache_pipeline/export_spawn_objects.py` writes the spawned-loc templates.
- `cache_pipeline/export_skeletal.py` writes skeletal ("maya") animation: per-vertex bone weights
  for each skeletal NPC and spot-animation model, and per-tick bone matrices for every sequence
  on their skeletons (decoding in `rc_cache/skeletal.py`).
- `export_collision_tiles.py`, `export_object_placements.py` and `spawn_index.py` write the indexed
  collision, placement and spawn files. Reports go to `build/reports/`.

## `puffer_overlay.sh`

`tools/puffer_overlay.sh /path/to/PufferLib` builds `build/puffer/librs_gym_puffer.a` against that
checkout, symlinks `ocean/rs_gym` and `config/rs_gym.ini` into it, and prints the build command.
