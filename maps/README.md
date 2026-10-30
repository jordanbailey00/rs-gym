# maps

Map specs (TOML), compiled by `tools/build_mode.py` for the mode that names them:
`compact_v1.toml` (compact_osrs_v1, the v1 benchmark) and `island.toml` (the island draft).

`compact_v1` is a 128x128 island and a 192x64 dungeon, composed from real b237 areas so it reads as
one place. The village is Rimmington's houses round the well (one has the range) with the Draynor
bank, the Lumbridge church, general store and smithy along its square; Fred's farm (chicken pen,
flax field, pond) and a fenced cow pasture lie south; the Lumbridge river bridge crosses to the
woods in the north; the Rimmington mine sits on the hill east of the village. The east is darker
ground with dead trees: a graveyard round the trapdoor, the dark wizards' stone circle round the
chaos altar, the outer resources and the Catherby pier for lobsters. The essence quarry and the
henge-ringed air and death altars are in the south-east.

The dungeon is the Varrock sewers with the moss cave west of them, one connected stamp. The
village manhole's ladder lands in the main channel (giant rats, goblins); a plank crossing leads
to the west rooms and the long pipe to the moss cave, where the east trapdoor's ladder comes down
among the hill giants; two gates (two-way links on the sewers' own doors) close off the north rooms
(dark wizards, then skeletons past the pillared hall); the broken bars at the channel's east end
lead into Scurrius' arena. The aggressive families sit behind gates or far from the basic tier.

Coordinates are tiles relative to an area's south-west corner: world tile = area origin × 64 + value.
Tables take an optional `area` (default `surface`). `[[stamp]] source` coordinates are real-world
b237 tiles. Rects are `[x0, y0, x1, y1]`.

| Table | Purpose |
|---|---|
| `[map]` | `name`, `seed`, player `start` (surface) |
| `[[area]]` | `name`, `kind` (`island` or `void`), `origin` and `size` in mapsquares, `ocean_ring`; a void area's `fill_underlay` and `fill_height` floor its empty tiles (rock between tunnels) |
| `[materials]` | named floors: `underlay`, `overlay`, `settings` (1 blocks the tile) as map floor ids |
| `[island]` | coast `radius` and `wobble`, beach width, heights, sea depth bands. `coast = "smooth"`: `squareness` (superellipse exponent), `coast_noise`, `sea`/`land` circles, shaped edge tiles, a `sand_underlay` beach and a `grass` underlay mix in noise patches |
| `[replace]` | loc id -> loc id for stamped locs, e.g. gatherable trees and ore rocks to option-less twins, so resource budgets stay with `[[loc]]` and `[[scatter]]` |
| `[[paint]]` | fills a `rect` with a `material` (and an `alt` mix) or an underlay `palette`; `blob = true` paints a noisy ellipse with shaped edges |
| `[[river]]` | polyline `points`, `width`, blocked `material`; banks slope down. `curve = true`: a smooth curve with shaped banks over `bank_underlay`, `wobble` varies the width |
| `[[path]]` | polyline `points` painted with a material (default `path`), `width` in tiles; `curve = true` paths of one material share a smooth band with shaped edges |
| `[[stamp]]` | copies a real-world rect (`source`, `size`) to `at`, `rotation` in quarter turns clockwise; its stair and ladder rows move with it. Options: `drop_ids`, `drop_at` ([x, y, id]), `drop_walls_in` (rects), `blend` (outer tiles keep the ground's underlay), `clip` (`land`: the coast crops it; `water`: the map keeps its own sea), `planes` (copy the lowest N), `ground_only` (rects copied at plane 0 only), `overlay_swap`, `keep_resources` |
| `[[floor]]` | re-floors a `rect` with a `material` after stamping (planks over a channel) |
| `[[loc]]` | one loc: `id`, `at`, optional `type` (default 10), `rotation`, `plane` |
| `[[fence]]` | a wall loc `id` round a `rect` (type 0 sides, type 2 corners) with `gaps` |
| `[[link]]` | makes a loc's `option` move the player `to` a tile (optionally `to_area`); `stand` sets the tile used to approach it. Two links on one door with opposite `stand` tiles make a two-way gate that stays shut |
| `[[zone]]` | `rect` with `wilderness = [low, high]` (levels rise in bands northward) or `multi = true`, on `planes` |
| `[[scatter]]` | `count` locs from `ids` (plain or [id, weight]) on open ground inside `rect`, `spacing` tiles apart; `margin` from floors, `cluster = [scale, level]` groves, `decor` best effort, `overlays` it may stand on, `in_stamps` |
| `[[decor]]` | ground decoration: each free tile with an allowed underlay gets one of `ids` with chance `density` (type 22 by default) |
| `[[npc]]` | NPCs from `ids` at a fixed `at`, or `count` of them in `rect` (`placement = "shore"` for fishing spots); optional `wander` |
| `[[item]]` | static ground item: `id`, `quantity`, `at`, `respawn` ticks |
| `[[drops]]` | drop table for `npcs`: `always`, `main` and `tertiary` rows `[item, min, max(, 1-in-N)]` |
| `[[spawn_objects]]` | locs the engine spawns at runtime (`id`, expected `name`, `type`) that need render templates |
| `[[landmark]]` | named places agents can travel to; order is the travel-action order (at most 32) |

Find ids and building bounds with `rc_cache` (`tools/cache_pipeline/rc_cache`), and check a stamp in
the exported minimaps under `data/regions/`.
