# tools

Python tooling that turns a map spec into runtime data.

## `build_gym.py`

```bash
python3 tools/build_gym.py [maps/gym_v0.toml]
```

It runs these steps in order:
1. Compiles the spec with `gymmap`.
2. Writes the overlay cache to `build/gym/cache`.
3. Exports `data/regions` (terrain, objects, minimaps, collision, object placements).
4. Writes `data/spawns`, an empty `data/defs/area_flags.bin` and `data/manifest.json`.

## `gymmap/`: the map compiler

- `mapformat.py` reads and writes b237 mapsquare terrain and loc files.
- `compile.py` turns a spec into mapsquares:
  - generated island terrain
  - paths
  - stamps of real-world rects, all planes, rotated, with heights fitted to the ground
  - single locs and scattered locs
  - NPC spawns
- `cachewrite.py` writes an OpenRS2-layout cache whose map index (5) holds only the gym. Every
  other index links back to `data/source/.../cache`, so the exporters see the gym as if it were the
  whole world.

## Export pipeline (copied from RuneC_v4)

- `cache_pipeline/rc_cache/` reads the cache.
- `cache_pipeline/export_scene_slice.py` writes the per-mapsquare terrain, objects, object
  animations and minimaps, plus the catalog and the shared material atlas.
- `export_collision_tiles.py` and `export_object_placements.py` write the indexed collision and
  placement files.
- `spawn_index.py` writes the NPC and ground-item spawn indexes.

Reports go to `build/reports/`.
