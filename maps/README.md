# maps

Gym map specs (TOML), compiled by `tools/build_gym.py`. `gym_v0.toml` is the v0 island.

Spec coordinates are tiles relative to the playable area's south-west corner:
world tile = origin × 64 + value. `[[stamp]] source` coordinates are real-world b237 tiles.

| Table | Purpose |
|---|---|
| `[map]` | `origin` and `size` in mapsquares, `ocean_ring` width, `seed`, `start` tile |
| `[materials]` | named floors: `underlay`, `overlay`, `settings` (1 blocks the tile) as map floor ids |
| `[island]` | coast radius and wobble, beach width, heights, sea depth bands |
| `[[path]]` | polyline `points` painted with a material (default `path`), `width` in tiles |
| `[[stamp]]` | copies a real-world rect (`source`, `size`) to `at`, with `rotation` in quarter turns clockwise and optional `drop_ids` |
| `[[loc]]` | one loc: `id`, `at`, optional `type` (default 10), `rotation`, `plane` |
| `[[scatter]]` | `count` locs picked from `ids`, placed on open grass inside `area`, `spacing` tiles apart |
| `[[npc]]` | `count` NPCs picked from `ids`, placed on open tiles inside `area`; optional `wander` |

Find ids and building bounds with `rc_cache` (`tools/cache_pipeline/rc_cache`), and check a stamp in
the exported minimaps under `data/regions/`.
