# maps

Map specs (TOML), compiled by `tools/build_mode.py` for the mode that names them:
`compact_v1.toml` (compact_osrs_v1, the v1 benchmark) and `island.toml` (the island draft).

`compact_v1` is a 128x128 island and a 128x64 dungeon. The dungeon has two ways down: a manhole
between the town and the farm into a Varrock sewers stamp (giant rats, goblins) and a trapdoor in
the east into the Edgeville hill giant cave (hill giants at the north end, skeletons west, dark
wizards south). A pipe joins the two; the cave's far pipe leads to Scurrius' lair. The cave's
families hunt players within 8 tiles, so the low tier lives in the sewers, out of their reach.

Coordinates are tiles relative to an area's south-west corner: world tile = area origin × 64 + value.
Tables take an optional `area` (default `surface`). `[[stamp]] source` coordinates are real-world
b237 tiles. Rects are `[x0, y0, x1, y1]`.

| Table | Purpose |
|---|---|
| `[map]` | `name`, `seed`, player `start` (surface) |
| `[[area]]` | `name`, `kind` (`island` or `void`), `origin` and `size` in mapsquares, `ocean_ring` |
| `[materials]` | named floors: `underlay`, `overlay`, `settings` (1 blocks the tile) as map floor ids |
| `[island]` | coast `radius` and `wobble`, beach width, heights, sea depth bands |
| `[[paint]]` | fills a `rect` with a `material` (and an `alt` mix) |
| `[[river]]` | polyline `points`, `width`, blocked `material`; banks slope down |
| `[[path]]` | polyline `points` painted with a material (default `path`), `width` in tiles |
| `[[stamp]]` | copies a real-world rect (`source`, `size`) to `at`, `rotation` in quarter turns clockwise, optional `drop_ids`; its stair and ladder rows move with it |
| `[[loc]]` | one loc: `id`, `at`, optional `type` (default 10), `rotation`, `plane` |
| `[[link]]` | makes a loc's `option` move the player `to` a tile (optionally `to_area`); `stand` sets the tile used to approach it |
| `[[zone]]` | `rect` with `wilderness = [low, high]` (levels rise in bands northward) or `multi = true`, on `planes` |
| `[[scatter]]` | `count` locs from `ids` on open ground inside `rect`, `spacing` tiles apart |
| `[[npc]]` | NPCs from `ids` at a fixed `at`, or `count` of them in `rect` (`placement = "shore"` for fishing spots); optional `wander` |
| `[[item]]` | static ground item: `id`, `quantity`, `at`, `respawn` ticks |
| `[[drops]]` | drop table for `npcs`: `always`, `main` and `tertiary` rows `[item, min, max(, 1-in-N)]` |
| `[[spawn_objects]]` | locs the engine spawns at runtime (`id`, expected `name`, `type`) that need render templates |
| `[[landmark]]` | named places agents can travel to; order is the travel-action order (at most 32) |

Find ids and building bounds with `rc_cache` (`tools/cache_pipeline/rc_cache`), and check a stamp in
the exported minimaps under `data/regions/`.
