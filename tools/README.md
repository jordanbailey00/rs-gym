# tools

Python tooling that turns a map spec into runtime data, plus the PufferLib overlay script.

## `build_gym.py`

```bash
python3 tools/build_gym.py [maps/gym_v1.toml]
```

Steps, in order:
1. Compiles the spec with `gymmap` and writes the overlay cache to `build/gym/cache`.
2. Exports object placements, then the gathering-node table (trees and rocks with their placement
   keys, respawn times and depleted stump/empty-rock locs).
3. Exports `data/regions` (terrain, objects, minimaps, collision). Depletable nodes get separate
   meshes so the viewer can swap in the depleted loc.
4. Exports render templates for locs spawned at runtime (`[[spawn_objects]]`: fires).
5. Writes `data/spawns`, the traversal and area-flag tables, `data/regions/gym.cfg` (start, respawn,
   loaded mapsquares, landmarks), the merged drop table and `data/manifest.json`.

## `gymmap/`: the map compiler

- `mapformat.py` reads and writes b237 mapsquare terrain and loc files.
- `compile.py` turns a spec into mapsquares and tables: areas, island terrain, rivers, paths,
  rotated stamps (all planes, heights fitted, their stair rows moved along), locs, scatter, links,
  zones, NPC and item spawns, landmarks.
- `cachewrite.py` writes an OpenRS2-layout cache whose map index holds only the gym; every other
  index links back to `data/source/.../cache`.
- `spatial.py` writes the traversal (stairs, ladders, links) and area-flag (zones) tables.
- `nodes.py` writes the gathering-node table; `drops.py` merges the spec's drop tables.

## Export pipeline (copied from RuneC_v4)

- `cache_pipeline/rc_cache/` reads the cache.
- `cache_pipeline/export_scene_slice.py` writes per-mapsquare terrain, objects, object animations
  and minimaps, plus the catalog and material atlas.
- `cache_pipeline/export_spawn_objects.py` writes the spawned-loc templates.
- `export_collision_tiles.py`, `export_object_placements.py` and `spawn_index.py` write the indexed
  collision, placement and spawn files. Reports go to `build/reports/`.

## `puffer_overlay.sh`

`tools/puffer_overlay.sh /path/to/PufferLib` builds `build/puffer/librs_gym_puffer.a` against that
checkout, symlinks `ocean/rs_gym` and `config/rs_gym.ini` into it, and prints the build command.
