"""Compile a gym map spec (TOML) into mapsquares and spatial tables built from source-cache content.

A spec names its areas: an island surface and void areas such as a dungeon. Coordinates in a table
are tiles relative to its area's south-west corner (`area` defaults to "surface"); `[[stamp]]
source` coordinates are real b237 tiles.
Order: terrain -> paint -> rivers -> paths -> stamps -> locs -> scatter -> decor -> links, zones,
spawns, items.

Opt-in smooth floors (`[island] coast = "smooth"`, `curve = true` on rivers and paths, `blob = true`
on paint) give coasts, water and roads the client's shaped tile edges; `[island] grass` mixes grass
underlays in noise patches; `[[decor]]` scatters ground decoration; `[replace]` turns stamped
gatherable look-alikes into scenery so resource budgets stay with `[[loc]]` and `[[scatter]]`.
"""

from __future__ import annotations

import math
import random
import tomllib
from collections import deque
from dataclasses import dataclass, field, replace
from pathlib import Path

from export_terrain import calculate_height
from rc_cache import RcCacheStore, read_map_region_file
from rc_cache.definitions import decode_location_definition, decode_npc_definition

from .mapformat import PLANES, PROCEDURAL, SIZE, Loc, Tile, decode_locs, decode_terrain, empty_tiles, encode_locs, encode_terrain
from .shapes import Band, coverage, curve, fit_shape, fractal, noise
from .spatial import (AREA_MULTICOMBAT, AREA_WILDERNESS, AREA_WILDERNESS_LEVEL, TRAVERSAL_OBJECT, Edge, Zone,
                      read_traversal)

OBJECT_CONFIG_GROUP = 6
NPC_CONFIG_GROUP = 9
SCENERY_TYPES = (10, 11)  # locs whose width/length occupy tiles
WANDER_FROM_DEF = 255
DEFAULT_ITEM_RESPAWN = 100
TRAVERSAL_ACTIONS = {"Climb-up", "Climb-down", "Climb", "Pull", "Enter", "Exit", "Go-up", "Go-down",
                     "Walk-up", "Walk-down", "Cross", "Squeeze-through", "Climb-over"}


@dataclass(frozen=True)
class Area:
    name: str
    kind: str  # "island" or "void"
    rx: int
    ry: int
    w: int  # mapsquares
    h: int
    ring: int  # all-water mapsquares around an island

    @property
    def bounds(self) -> tuple[int, int, int, int]:
        return self.rx * SIZE, self.ry * SIZE, (self.rx + self.w) * SIZE, (self.ry + self.h) * SIZE

    def squares(self) -> set[tuple[int, int]]:
        return {(x, y) for x in range(self.rx - self.ring, self.rx + self.w + self.ring)
                for y in range(self.ry - self.ring, self.ry + self.h + self.ring)}


@dataclass
class Gym:
    """Compiled map: encoded mapsquares plus the tables the build step writes beside them."""
    name: str
    mapsquares: dict[tuple[int, int], tuple[bytes, bytes]]
    areas: dict[str, Area]
    start: tuple[int, int, int]  # world x, y, plane
    respawn: tuple[int, int, int]  # where dead players return
    npc_spawns: list[tuple[int, ...]]  # npc_id, x, y, plane, direction, wander, flags
    ground_items: list[tuple[int, ...]]  # item_id, quantity, x, y, plane, flags, respawn_ticks
    edges: list[Edge]
    zones: list[Zone]
    landmarks: list[tuple[str, int, int, int]] = field(default_factory=list)  # name, world x, y, plane


@dataclass(frozen=True)
class Stamp:
    """Where a real-world rect landed: maps source tiles to gym tiles."""
    sx: int
    sy: int
    w: int
    h: int
    dx: int
    dy: int
    r: int

    def contains(self, x: int, y: int) -> bool:
        return self.sx <= x < self.sx + self.w and self.sy <= y < self.sy + self.h

    def tile(self, x: int, y: int) -> tuple[int, int]:
        ti, tj = _rotate_tile(x - self.sx, y - self.sy, self.w, self.h, self.r)
        return self.dx + ti, self.dy + tj


class Source:
    """Read-only view of the real world in the source cache."""

    def __init__(self, store: RcCacheStore):
        self.store = store
        self._terrain: dict[tuple[int, int], list] = {}
        self._heights: dict[tuple[int, int], list] = {}
        self._locs: dict[tuple[int, int], list[Loc]] = {}
        self._def_bytes = store.read_group(2, OBJECT_CONFIG_GROUP)
        self._defs = {}
        self._npc_bytes = store.read_group(2, NPC_CONFIG_GROUP)

    def _square(self, rx: int, ry: int) -> list:
        if (rx, ry) not in self._terrain:
            data = read_map_region_file(self.store, rx, ry, "terrain")
            if data is None:
                raise SystemExit(f"source mapsquare {rx},{ry} has no terrain")
            tiles = decode_terrain(data)
            self._terrain[(rx, ry)] = tiles
            self._heights[(rx, ry)] = _absolute_heights(tiles, rx * SIZE, ry * SIZE)
        return self._terrain[(rx, ry)]

    def tile(self, x: int, y: int, plane: int) -> Tile:
        return self._square(x >> 6, y >> 6)[plane][x & 63][y & 63]

    def height(self, x: int, y: int) -> int:
        """Plane-0 height as an explicit height byte (OSRS height is -byte * 8)."""
        self._square(x >> 6, y >> 6)
        return -self._heights[(x >> 6, y >> 6)][x & 63][y & 63] // 8

    def locs_in(self, x0: int, y0: int, w: int, h: int) -> list[Loc]:
        """World-coordinate locs whose south-west tile lies inside the rect."""
        out = []
        for rx in range(x0 >> 6, (x0 + w - 1 >> 6) + 1):
            for ry in range(y0 >> 6, (y0 + h - 1 >> 6) + 1):
                if (rx, ry) not in self._locs:
                    data = read_map_region_file(self.store, rx, ry, "locations")
                    self._locs[(rx, ry)] = [replace(l, x=rx * SIZE + l.x, y=ry * SIZE + l.y)
                                            for l in decode_locs(data or b"\x00")]
                out += [l for l in self._locs[(rx, ry)] if x0 <= l.x < x0 + w and y0 <= l.y < y0 + h]
        return out

    def loc_def(self, loc_id: int):
        if loc_id not in self._defs:
            if loc_id not in self._def_bytes:
                raise SystemExit(f"loc id {loc_id} is not in the source cache")
            self._defs[loc_id] = decode_location_definition(loc_id, self._def_bytes[loc_id])
        return self._defs[loc_id]

    def npc_def(self, npc_id: int):
        if npc_id not in self._npc_bytes:
            raise SystemExit(f"npc id {npc_id} is not in the source cache")
        return decode_npc_definition(npc_id, self._npc_bytes[npc_id])


def _absolute_heights(tiles: list, base_x: int, base_y: int) -> list:
    """Plane-0 OSRS heights, resolving procedural tiles the way the client does."""
    out = [[0] * SIZE for _ in range(SIZE)]
    for x in range(SIZE):
        for y in range(SIZE):
            t = tiles[0][x][y]
            if t.height == PROCEDURAL:
                out[x][y] = -calculate_height(0xE3B7B + base_x + x, 0x87CCE + base_y + y) * 8
            else:
                out[x][y] = -t.height * 8
    return out


class Canvas:
    """Every exported mapsquare, addressed in world tiles."""

    def __init__(self, squares: set[tuple[int, int]]):
        self.squares = {sq: empty_tiles() for sq in squares}
        self.locs: list[Loc] = []
        self.reserved: list[tuple[int, int, int, int]] = []  # stamp rects, world x0, y0, x1, y1
        self.stamps: list[Stamp] = []

    def inside(self, x: int, y: int) -> bool:
        return (x >> 6, y >> 6) in self.squares

    def tile(self, x: int, y: int, plane: int = 0) -> Tile:
        return self.squares[(x >> 6, y >> 6)][plane][x & 63][y & 63]

    def put(self, x: int, y: int, plane: int, tile: Tile) -> None:
        self.squares[(x >> 6, y >> 6)][plane][x & 63][y & 63] = tile

    def tiles_of(self, squares) -> list[tuple[int, int]]:
        return [(sx * SIZE + x, sy * SIZE + y) for sx, sy in squares for x in range(SIZE) for y in range(SIZE)]


def _rotate_tile(i: int, j: int, w: int, h: int, r: int) -> tuple[int, int]:
    """Quarter turns clockwise of tile (i, j) inside a w x h rect."""
    return ((i, j), (j, w - 1 - i), (w - 1 - i, h - 1 - j), (h - 1 - j, i))[r]


def _rotate_loc(i: int, j: int, fw: int, fh: int, w: int, h: int, r: int) -> tuple[int, int]:
    """New south-west tile of an fw x fh footprint at (i, j) after r clockwise turns."""
    return ((i, j), (j, w - i - fw), (w - i - fw, h - j - fh), (h - j - fh, i))[r]


def _footprint(source: Source, loc: Loc) -> tuple[int, int]:
    d = source.loc_def(loc.id)
    return (d.length, d.width) if loc.rotation & 1 else (d.width, d.length)


def _clamp_height(h: float) -> int:
    h = max(0, min(255, round(h)))
    return 2 if h == 1 else h  # height byte 1 decodes as 0


def _neighbours(x: int, y: int):
    return ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1))


def _paint(t: Tile, mat: dict) -> None:
    t.underlay = mat.get("underlay", t.underlay)
    t.overlay = mat.get("overlay", 0)
    t.shape = 0
    t.rotation = 0
    t.settings = mat.get("settings", 0)


def _distances(seeds: list, passable) -> dict:
    """Breadth-first tile distance from the seeds, over tiles where passable(tile) holds."""
    dist = {s: 0 for s in seeds}
    queue = deque(seeds)
    while queue:
        p = queue.popleft()
        for n in _neighbours(*p):
            if n not in dist and passable(n):
                dist[n] = dist[p] + 1
                queue.append(n)
    return dist


def _island(canvas: Canvas, area: Area, spec: dict, mats: dict, seed: int) -> None:
    rng = random.Random(seed)
    x0, y0, x1, y1 = area.bounds
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    radius = spec["radius"]
    wobble = [(spec["wobble"] / k, k, rng.uniform(0, 2 * math.pi)) for k in range(2, 7)]
    squares = area.squares()
    tiles = canvas.tiles_of(squares)

    land = set()
    for x, y in tiles:
        if x0 <= x < x1 and y0 <= y < y1:
            angle = math.atan2(y - cy, x - cx)
            edge = radius * (1 + sum(a * math.sin(k * angle + p) for a, k, p in wobble))
            if math.hypot(x - cx, y - cy) < edge:
                land.add((x, y))

    in_area = lambda p: (p[0] >> 6, p[1] >> 6) in squares  # noqa: E731
    coast = [p for p in land if any(n not in land for n in _neighbours(*p))]
    dist = _distances(coast, lambda p: p in land)
    shore = [n for p in coast for n in _neighbours(*p) if n not in land and in_area(n)]
    water_dist = _distances(list(set(shore)), lambda p: p not in land and in_area(p))

    sea, beach, ramp, scale = spec["sea_height"], spec["beach_width"], spec["ramp_tiles"], spec["height_scale"]
    depths = spec["water_depths"]  # [max distance, material] pairs, nearest first
    for x, y in tiles:
        t = canvas.tile(x, y)
        if (x, y) in land:
            d = dist[(x, y)]
            hill = (calculate_height(x + 7919, y + 3571) - 10) * scale
            t.height = _clamp_height(sea + 2 + min(1.0, d / ramp) * hill)
            patch = calculate_height(x + 1013, y + 2027) > 40  # smooth blobs of the second grass
            mat = mats["sand"] if d < beach else mats["grass_alt"] if patch else mats["grass"]
        else:
            d = water_dist.get((x, y), 99) + 1
            t.height = sea
            mat = next((mats[name] for limit, name in depths if d <= limit), mats[depths[-1][1]])
        _paint(t, mat)


def _palette(points: list, pairs: list, value) -> dict:
    """Underlay per point from [underlay, weight] pairs: noise quantiles, so weights are shares."""
    vals = sorted(value(*p) for p in points)
    total = sum(w for _, w in pairs)
    cuts, acc = [], 0.0
    for u, w in pairs:
        acc += w / total
        cuts.append((vals[min(len(vals) - 1, int(acc * len(vals)))], u))
    return {p: next((u for cut, u in cuts if value(*p) <= cut), pairs[-1][0]) for p in points}


def _island_smooth(canvas: Canvas, area: Area, spec: dict, mats: dict, seed: int) -> None:
    """A curved coast: edge tiles take shaped sea overlays over sand, the beach is sand underlay
    that blends inland into patches of grass underlays (`grass`), as on real coasts. `sea` and
    `land` circles ([x, y, r], area tiles) cut bays and add headlands."""
    rng = random.Random(seed)
    x0, y0, x1, y1 = area.bounds
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    radius = spec["radius"]
    wobble = [(spec["wobble"] / k, k, rng.uniform(0, 2 * math.pi)) for k in range(2, 7)]
    rough = spec.get("coast_noise", 0.0)
    seas = [(x0 + x, y0 + y, r) for x, y, r in spec.get("sea", [])]
    lands = [(x0 + x, y0 + y, r) for x, y, r in spec.get("land", [])]

    power = spec.get("squareness", 2.0)  # superellipse exponent: 2 is round, higher is squarer

    def depth(px: float, py: float) -> float:  # > 0 on land: tiles inside the coast
        angle = math.atan2(py - cy, px - cx)
        c, s = abs(math.cos(angle)), abs(math.sin(angle))
        reach = radius / max(1e-9, c ** power + s ** power) ** (1 / power)
        d = reach * (1 + sum(a * math.sin(k * angle + p) for a, k, p in wobble)) - math.hypot(px - cx, py - cy)
        d += rough * (fractal(px, py, 14.0, seed) - 0.5) * 2
        for x, y, r in lands:
            d = max(d, r - math.hypot(px - x, py - y))
        for x, y, r in seas:
            d = min(d, math.hypot(px - x, py - y) - r)
        return d

    def on_land(px: float, py: float) -> bool:
        return x0 <= px < x1 and y0 <= py < y1 and depth(px, py) > 0

    squares = area.squares()
    tiles = canvas.tiles_of(squares)
    in_area = lambda p: (p[0] >> 6, p[1] >> 6) in squares  # noqa: E731
    land, edge = set(), {}
    for x, y in tiles:
        if not (x0 - 1 <= x <= x1 and y0 - 1 <= y <= y1) or depth(x + 0.5, y + 0.5) < -2:
            continue
        if depth(x + 0.5, y + 0.5) > 2:
            land.add((x, y))
            continue
        bits = coverage(on_land, x, y)
        fit = fit_shape(tuple(1 - b for b in bits))  # the sea's part of the tile
        if fit is None:
            land.add((x, y))
        elif fit != (0, 0):
            edge[(x, y)] = fit
    coast = [p for p in land if any(n not in land for n in _neighbours(*p))]
    dist = _distances(coast, lambda p: p in land)
    shore = [p for p in edge] + [n for p in coast for n in _neighbours(*p) if n not in land and in_area(n)]
    water_dist = _distances(list(set(shore)), lambda p: p not in land and in_area(p))

    sea, beach, ramp, scale = spec["sea_height"], spec["beach_width"], spec["ramp_tiles"], spec["height_scale"]
    sand = spec.get("sand_underlay", mats["sand"].get("underlay", 48))
    grass_noise = lambda x, y: fractal(x, y, spec.get("grass_scale", 9.0), seed + 7)  # noqa: E731
    grass = _palette(sorted(land), spec["grass"], grass_noise)
    depths = spec["water_depths"]
    for x, y in tiles:
        t = canvas.tile(x, y)
        if (x, y) in land:
            d = dist[(x, y)]
            hill = (calculate_height(x + 7919, y + 3571) - 10) * scale
            t.height = _clamp_height(sea + 2 + min(1.0, d / ramp) * hill)
            shore_band = beach + 2.5 * (noise(x, y, 5.0, seed + 3) - 0.5)
            _paint(t, {"underlay": sand if d < shore_band else grass[(x, y)]})
        elif (x, y) in edge:
            t.height = sea
            _paint(t, {"underlay": sand, **{k: v for k, v in mats[depths[0][1]].items() if k != "underlay"}})
            t.shape, t.rotation = edge[(x, y)]
        else:
            d = water_dist.get((x, y), 99) + 1
            t.height = sea
            _paint(t, next((mats[name] for limit, name in depths if d <= limit), mats[depths[-1][1]]))


def _paint_zones(canvas: Canvas, zones: list, areas: dict, mats: dict) -> None:
    """Repaint open land in a rect (water, beach and paths keep their floors); `alt` adds noise blobs."""
    for zone in zones:
        x0, y0, x1, y1 = _rect(zone, areas)
        palette = _palette([(x, y) for x in range(x0, x1) for y in range(y0, y1)], zone["palette"],
                           lambda x, y: fractal(x, y, 7.0, x0 + 3 * y0)) if "palette" in zone else None
        inside = None
        if zone.get("blob"):  # an ellipse in the rect with a noisy rim
            cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
            inside = lambda px, py: (((px - cx) / rx) ** 2 + ((py - cy) / ry) ** 2  # noqa: E731
                                     < 0.75 + 0.5 * fractal(px, py, 6.0, x0 * 31 + y0))
        for x in range(x0, x1):
            for y in range(y0, y1):
                if not canvas.inside(x, y):
                    continue
                t = canvas.tile(x, y)
                if t.settings & 1 or t.overlay:
                    continue
                mat = mats[zone["material"]] if "material" in zone else {"underlay": palette[(x, y)]}
                if inside:
                    fit = fit_shape(coverage(inside, x, y)) if "overlay" in mat else (
                        (0, 0) if inside(x + 0.5, y + 0.5) else None)
                    if fit is None:
                        continue
                    _paint(t, mat)
                    t.shape, t.rotation = fit if "overlay" in mat else (0, 0)
                    continue
                alt = "alt" in zone and calculate_height(x + 4441, y + 7703) > 40
                _paint(t, mats[zone["alt"]] if alt else mat)


def _void(canvas: Canvas, area: Area, mats: dict) -> None:
    for x, y in canvas.tiles_of(area.squares()):
        t = canvas.tile(x, y)
        _paint(t, mats["void"])
        t.height = 0


def _segment_distance(px, py, ax, ay, bx, by) -> float:
    dx, dy = bx - ax, by - ay
    k = 0.0 if dx == dy == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - k * dx, py - ay - k * dy)


def _polyline_tiles(canvas: Canvas, points: list, half: float) -> set:
    out = set()
    pad = int(half) + 2
    for (ax, ay), (bx, by) in zip(points, points[1:]):
        for x in range(min(ax, bx) - pad, max(ax, bx) + pad + 1):
            for y in range(min(ay, by) - pad, max(ay, by) + pad + 1):
                if canvas.inside(x, y) and _segment_distance(x, y, ax, ay, bx, by) < half:
                    out.add((x, y))
    return out


def _rivers(canvas: Canvas, rivers: list, areas: dict, mats: dict) -> None:
    """Carve rivers and lower the land beside them so banks slope into the water. A `curve` river
    follows a smooth curve through its points with shaped banks over `bank_underlay` (mud)."""
    for k, river in enumerate(rivers):
        area = _area(river, areas)
        pts = [(area.rx * SIZE + x, area.ry * SIZE + y) for x, y in river["points"]]
        level, bank = river.get("height", 2), river.get("bank", 5)
        mat = mats[river.get("material", "shallow")]
        if river.get("curve"):
            band = Band()
            band.add(curve(pts), river["width"] / 2, river.get("wobble", 0.0), 977 + k)
            water = set()
            for x, y in band.tiles():
                if not canvas.inside(x, y) or canvas.tile(x, y).settings & 1:
                    continue  # the sea keeps its own water at the mouth
                fit = fit_shape(band.bits(x, y))
                if fit is None:
                    continue
                t = canvas.tile(x, y)
                _paint(t, mat)
                t.shape, t.rotation = fit
                if fit != (0, 0):
                    t.underlay = river.get("bank_underlay", t.underlay)
                t.height = level
                water.add((x, y))
        else:
            water = {p for p in _polyline_tiles(canvas, pts, river["width"] / 2)
                     if not canvas.tile(*p).settings & 1}  # the sea keeps its own water at the mouth
        for x, y in water:
            t = canvas.tile(x, y)
            if not river.get("curve"):
                _paint(t, mat)
            t.height = level
        near = _distances(list(water), lambda p: canvas.inside(*p) and p not in water
                          and not canvas.tile(*p).settings & 1)
        for (x, y), d in near.items():
            if 0 < d <= bank:
                t = canvas.tile(x, y)
                t.height = _clamp_height(level + (t.height - level) * d / (bank + 1))


def _paths(canvas: Canvas, paths: list, areas: dict, mats: dict) -> None:
    """Paths in spec order. `curve` paths of one material share a band through smooth curves, so
    crossings merge, and edge tiles take shaped overlays; `wobble` varies the width."""
    bands: dict[str, Band] = {}
    for k, path in enumerate(paths):
        area = _area(path, areas)
        name = path.get("material", "path")
        pts = [(area.rx * SIZE + x, area.ry * SIZE + y) for x, y in path["points"]]
        if path.get("curve"):
            band = bands.setdefault(name, Band())
            band.add(curve(pts), path.get("width", 2) / 2, path.get("wobble", 0.0), 31 + k)
            continue
        for x, y in _polyline_tiles(canvas, pts, path.get("width", 2) / 2):
            t = canvas.tile(x, y)
            if not t.settings & 1:  # never pave water
                _paint(t, mats[name])
    for name, band in bands.items():
        mat = mats[name]
        for x, y in sorted(band.tiles()):
            if not canvas.inside(x, y):
                continue
            t = canvas.tile(x, y)
            if t.settings & 1:  # never pave water
                continue
            fit = fit_shape(band.bits(x, y))
            if fit is None or (t.overlay and t.overlay != mat.get("overlay") and fit != (0, 0)):
                continue  # leave another floor's tile unless this one covers it
            t.underlay = mat.get("underlay", t.underlay)
            t.overlay = mat.get("overlay", 0)
            t.shape, t.rotation = fit if t.overlay else (0, 0)


def _stamp(canvas: Canvas, source: Source, stamp: dict, areas: dict, replace_ids: dict | None = None) -> Stamp:
    sx, sy = stamp["source"]
    w, h = stamp["size"]
    r = stamp.get("rotation", 0) % 4
    dx, dy, _ = _at(stamp, areas)
    dw, dh = (w, h) if r % 2 == 0 else (h, w)
    if not (canvas.inside(dx, dy) and canvas.inside(dx + dw - 1, dy + dh - 1)):
        raise SystemExit(f"stamp {stamp['name']!r} falls outside the map")
    placed = Stamp(sx, sy, w, h, dx, dy, r)

    # Fit plane-0 heights: shift the copy so its border matches the ground it lands on.
    border = [(i, j) for i in range(w) for j in range(h) if i in (0, w - 1) or j in (0, h - 1)]
    src_mean = sum(source.height(sx + i, sy + j) for i, j in border) / len(border)
    dst_mean = sum(canvas.tile(*placed.tile(sx + i, sy + j)).height for i, j in border) / len(border)
    offset = stamp.get("height_offset", dst_mean - src_mean)
    blend = stamp.get("blend", 0)  # outer tiles that keep the ground's own underlay, in a noisy band
    ground = {(x, y): canvas.tile(x, y).underlay for x in range(dx, dx + dw) for y in range(dy, dy + dh)
              if blend and min(x - dx, y - dy, dx + dw - 1 - x, dy + dh - 1 - y) < blend}

    area = stamp.get("area", "surface")
    rects = lambda key: [_rect({"area": area, "rect": rect}, areas) for rect in stamp.get(key, [])]  # noqa: E731
    within = lambda x, y, rs: any(a <= x < c and b <= y < d for a, b, c, d in rs)  # noqa: E731
    overlay_swap = {int(k): v for k, v in stamp.get("overlay_swap", {}).items()}
    ground_only = rects("ground_only")  # copied at plane 0 only: no ceiling there
    planes = stamp.get("planes", PLANES)  # copy only the lowest planes: what stands above stays behind
    # clip "land": leave the sea alone, so the coast crops the stamp; clip "water": keep the map's own
    # water where the source has water (a pier keeps its planks and posts, not its square of sea).
    clip = stamp.get("clip")
    sea, wet = set(), set()
    for i in range(w):
        for j in range(h):
            to, below = placed.tile(sx + i, sy + j), source.tile(sx + i, sy + j, 0)
            if clip == "land" and canvas.tile(*to).settings & 1:
                sea.add(to)
            elif clip == "water" and below.settings & 1 and below.overlay:
                wet.add(to)
    for plane in range(planes):
        for i in range(w):
            for j in range(h):
                to = placed.tile(sx + i, sy + j)
                if to in sea or plane == 0 and to in wet or plane and within(*to, ground_only):
                    continue
                src = source.tile(sx + i, sy + j, plane)
                t = replace(src, rotation=(src.rotation + r) & 3 if src.overlay or src.shape else src.rotation)
                t.overlay = overlay_swap.get(t.overlay, t.overlay)
                if plane == 0:
                    t.height = _clamp_height(source.height(sx + i, sy + j) + offset)
                canvas.put(*to, plane, t)

    drop = set(stamp.get("drop_ids", []))
    drop_at = {(_at({"area": area, "at": d[:2]}, areas)[:2], d[2])
               for d in stamp.get("drop_at", [])}  # [x, y, id]: one placement, in area tiles
    drop_walls = rects("drop_walls_in")  # walls and fences cut off by the crop
    swap = {} if stamp.get("keep_resources") else (replace_ids or {})
    canvas.locs = [l for l in canvas.locs if not (dx <= l.x < dx + dw and dy <= l.y < dy + dh)]
    covered = set()
    for loc in source.locs_in(sx, sy, w, h):
        if loc.id in drop or loc.plane >= planes:
            continue
        fw, fh = _footprint(source, loc)
        ni, nj = _rotate_loc(loc.x - sx, loc.y - sy, fw, fh, w, h, r)
        if any((dx + ni + i, dy + nj + j) in sea for i in range(fw) for j in range(fh)):
            continue
        if ((dx + ni, dy + nj), loc.id) in drop_at or loc.type in (0, 1, 2, 3, 9) and within(dx + ni, dy + nj, drop_walls):
            continue
        canvas.locs.append(Loc(swap.get(loc.id, loc.id), loc.plane, dx + ni, dy + nj, loc.type,
                               (loc.rotation + r) & 3))
        if loc.plane == 0 and loc.type != 22:
            covered.update((dx + ni + i, dy + nj + j) for i in range(fw) for j in range(fh))
    for (x, y), underlay in ground.items():
        t = canvas.tile(x, y)
        edge = min(x - dx, y - dy, dx + dw - 1 - x, dy + dh - 1 - y)
        if (x, y) in covered or t.overlay or t.settings & 1 or not underlay:
            continue
        if noise(x, y, 2.5, 5) > (edge + 0.5) / blend:
            t.underlay = underlay
    canvas.reserved.append((dx, dy, dx + dw, dy + dh))
    canvas.stamps.append(placed)
    if _area(stamp, areas).kind == "island":
        _feather(canvas, (dx, dy, dx + dw, dy + dh), stamp.get("feather", 4))
    return placed


def _feather(canvas: Canvas, rect: tuple[int, int, int, int], tiles: int) -> None:
    """Blend the land around a stamp toward the stamp's edge heights."""
    x0, y0, x1, y1 = rect
    for x in range(x0 - tiles, x1 + tiles):
        for y in range(y0 - tiles, y1 + tiles):
            if not canvas.inside(x, y) or (x0 <= x < x1 and y0 <= y < y1):
                continue
            t = canvas.tile(x, y)
            if t.settings & 1 or any(a <= x < c and b <= y < d for a, b, c, d in canvas.reserved):
                continue
            ex, ey = min(max(x, x0), x1 - 1), min(max(y, y0), y1 - 1)
            k = max(abs(x - ex), abs(y - ey)) / (tiles + 1)
            t.height = _clamp_height(canvas.tile(ex, ey).height * (1 - k) + t.height * k)


def _stamp_edges(canvas: Canvas, source: Source, stamps: list[tuple[Stamp, set[int]]],
                 source_edges: list[Edge]) -> list[Edge]:
    """Real stairs, ladders and levers that start and end inside a stamp move with it.

    The source table also holds rows that are not movement (e.g. "Mine" on rocks) and rows whose
    loc is elsewhere; only movement actions next to a stamped loc with the row's id are kept.
    """
    near = {}
    for loc in canvas.locs:
        fw, fh = _footprint(source, loc)
        for i in range(-1, fw + 1):
            for j in range(-1, fh + 1):
                near.setdefault((loc.x + i, loc.y + j, loc.plane), set()).add(loc.id)
    out = []
    for edge in source_edges:
        if edge.kind != TRAVERSAL_OBJECT or edge.flags or edge.action not in TRAVERSAL_ACTIONS:
            continue
        for stamp, dropped in stamps:
            if edge.source_id in dropped or not (stamp.contains(*edge.start[:2]) and stamp.contains(*edge.dest[:2])):
                continue
            moved = replace(edge, start=(*stamp.tile(*edge.start[:2]), edge.start[2]),
                            dest=(*stamp.tile(*edge.dest[:2]), edge.dest[2]))
            if edge.source_id in near.get(moved.start, ()):
                out.append(moved)
    return out


def _occupied(canvas: Canvas, source: Source) -> set[tuple[int, int]]:
    taken = set()
    for loc in canvas.locs:
        if loc.plane == 0 and loc.type in SCENERY_TYPES:
            fw, fh = _footprint(source, loc)
            taken.update((loc.x + i, loc.y + j) for i in range(fw) for j in range(fh))
    return taken


def _weighted(ids: list) -> tuple[list, list]:
    """`ids` as plain ids or [id, weight] pairs."""
    pairs = [(i, 1) if isinstance(i, int) else tuple(i) for i in ids]
    return [i for i, _ in pairs], [w for _, w in pairs]


def _scatter(canvas: Canvas, source: Source, spec: dict, areas: dict, rng: random.Random) -> None:
    """`count` locs in `rect`, `spacing` tiles from other scenery and `margin` tiles from any floor
    overlay (paths, water). `decor` scatters place what fits instead of failing; `cluster = [scale,
    level]` keeps them where a noise field is above `level`, in groves with clearings."""
    taken = _occupied(canvas, source)
    ax0, ay0, ax1, ay1 = _rect(spec, areas)
    spacing, margin = spec.get("spacing", 1), spec.get("margin", 0)
    ids, weights = _weighted(spec["ids"])
    cluster = spec.get("cluster")
    floors = set(spec.get("overlays", []))
    placed, tries = 0, 0
    while placed < spec["count"] and tries < spec["count"] * 200:
        tries += 1
        loc_id = rng.choices(ids, weights)[0]
        x, y = rng.randrange(ax0, ax1), rng.randrange(ay0, ay1)
        if cluster and fractal(x, y, cluster[0], ax0 * 7 + ay0) < cluster[1]:
            continue
        loc = Loc(loc_id, 0, x, y, spec.get("type", 10), rng.randrange(4))
        fw, fh = _footprint(source, loc)
        cells = [(x + i, y + j) for i in range(-spacing, fw + spacing) for j in range(-spacing, fh + spacing)]
        if any(not canvas.inside(cx, cy) or (cx, cy) in taken for cx, cy in cells):
            continue
        if not spec.get("in_stamps") and any(any(a <= cx < c and b <= cy < d for a, b, c, d in canvas.reserved)
                                              for cx, cy in cells):
            continue  # stamps keep their own scenery unless the scatter belongs in one (a field)
        near = [(x + i, y + j) for i in range(-margin, fw + margin) for j in range(-margin, fh + margin)]
        if any(not canvas.inside(*p) or canvas.tile(*p).settings & 1
               or canvas.tile(*p).overlay and canvas.tile(*p).overlay not in floors for p in near):
            continue  # land, not beach, path or water (`overlays` lists floors it may stand on)
        canvas.locs.append(loc)
        taken.update((x + i, y + j) for i in range(fw) for j in range(fh))
        placed += 1
    if placed < spec["count"] and not spec.get("decor"):
        raise SystemExit(f"scatter {spec['ids']} placed {placed}/{spec['count']}; widen its rect")


def _decor(canvas: Canvas, source: Source, spec: dict, areas: dict, rng: random.Random) -> int:
    """Ground decoration (grass tufts, flowers, stones) on open land outside stamps: each free tile
    with an allowed underlay gets one with chance `density`, ids weighted as on real land."""
    area = _area(spec, areas)
    x0, y0, x1, y1 = _rect(spec, areas) if "rect" in spec else area.bounds
    ids, weights = _weighted(spec["ids"])
    kind = spec.get("type", 22)
    allowed = set(spec.get("underlays", [])) or None
    taken = _occupied(canvas, source)
    taken.update((l.x, l.y) for l in canvas.locs if l.plane == 0 and l.type == kind)
    placed = 0
    for x in range(x0, x1):
        for y in range(y0, y1):
            if (x, y) in taken or not canvas.inside(x, y) or rng.random() >= spec["density"]:
                continue
            t = canvas.tile(x, y)
            if t.overlay or t.settings & 1 or (allowed and t.underlay not in allowed):
                continue
            if any(a <= x < c and b <= y < d for a, b, c, d in canvas.reserved):
                continue
            canvas.locs.append(Loc(rng.choices(ids, weights)[0], 0, x, y, kind, rng.randrange(4)))
            taken.add((x, y))
            placed += 1
    return placed


def _floors(canvas: Canvas, floors: list, areas: dict, mats: dict) -> None:
    """Re-floor whole tiles after stamping, e.g. planks laid over a stamped channel."""
    for floor in floors:
        x0, y0, x1, y1 = _rect(floor, areas)
        for x in range(x0, x1):
            for y in range(y0, y1):
                _paint(canvas.tile(x, y), mats[floor["material"]])


def _fences(canvas: Canvas, fences: list, areas: dict) -> None:
    """A wall loc (`id`, e.g. a wooden fence) around the outer edges of the rect's border tiles:
    straight pieces (type 0) along the sides, corner pieces (type 2) at the corners. `gaps` are border
    tiles left open, so animals keep to the pen and people walk in."""
    for fence in fences:
        x0, y0, x1, y1 = _rect(fence, areas)
        area = _area(fence, areas)
        gaps = {(area.rx * SIZE + x, area.ry * SIZE + y) for x, y in fence.get("gaps", [])}
        pieces = {}
        for x in range(x0, x1):
            pieces[(x, y0)] = (0, 3)
            pieces[(x, y1 - 1)] = (0, 1)
        for y in range(y0, y1):
            pieces[(x0, y)] = (0, 0)
            pieces[(x1 - 1, y)] = (0, 2)
        pieces.update({(x0, y1 - 1): (2, 0), (x1 - 1, y1 - 1): (2, 1), (x1 - 1, y0): (2, 2), (x0, y0): (2, 3)})
        for (x, y), (kind, rot) in pieces.items():
            if (x, y) not in gaps and not canvas.tile(x, y).settings & 1:
                canvas.locs.append(Loc(fence["id"], 0, x, y, kind, rot))


def _links(canvas: Canvas, source: Source, links: list, areas: dict) -> list[Edge]:
    """Authored traversals, such as a town ladder down into the dungeon."""
    taken = _occupied(canvas, source)
    edges = []
    for link in links:
        x, y, plane = _at(link, areas)
        loc = next((l for l in canvas.locs if l.id == link["loc"] and (l.x, l.y, l.plane) == (x, y, plane)), None)
        if loc is None:
            raise SystemExit(f"link: loc {link['loc']} is not placed at {link['at']} in {link.get('area', 'surface')}")
        d = source.loc_def(loc.id)
        if link["option"] not in d.actions:
            raise SystemExit(f"link: loc {loc.id} has no {link['option']!r} option ({d.actions})")
        fw, fh = _footprint(source, loc)
        if "stand" in link:
            stand = _at({"area": link.get("area", "surface"), "at": link["stand"]}, areas)[:2]
        else:
            ring = ([(x + i, y - 1) for i in range(fw)] + [(x + fw, y + j) for j in range(fh)]
                    + [(x - 1, y + j) for j in range(fh)] + [(x + i, y + fh) for i in range(fw)])
            stand = next((p for p in ring if canvas.inside(*p) and not canvas.tile(*p, plane).settings & 1
                          and p not in taken), None)
            if stand is None:
                raise SystemExit(f"link: no open tile beside loc {loc.id} at {link['at']}")
        to = _at({"area": link.get("to_area", link.get("area", "surface")), "at": link["to"]}, areas)
        edges.append(Edge(TRAVERSAL_OBJECT, d.actions.index(link["option"]), loc.id, (*stand, plane), to, 0,
                          link["option"], d.name))
    return edges


def _zones(zones: list, areas: dict) -> list[Zone]:
    out = []
    for z in zones:
        x0, y0, x1, y1 = _rect(z, areas)
        planes = z.get("planes", [0, 1, 2, 3])
        rows = []
        if z.get("multi"):
            rows.append(Zone(0, AREA_MULTICOMBAT, (x0, y0, x1 - 1, y1 - 1)))
        if "wilderness" in z:
            lo, hi = z["wilderness"]
            rows.append(Zone(0, AREA_WILDERNESS, (x0, y0, x1 - 1, y1 - 1)))
            band = math.ceil((y1 - y0) / (hi - lo + 1))  # levels rise northward
            for k, level in enumerate(range(lo, hi + 1)):
                b0 = y0 + k * band
                if b0 < y1:
                    rows.append(Zone(0, AREA_WILDERNESS_LEVEL, (x0, b0, x1 - 1, min(y1, b0 + band) - 1), level))
        out += [replace(row, plane=p) for row in rows for p in planes]
    return out


def _npcs(canvas: Canvas, source: Source, groups: list, areas: dict, rng: random.Random) -> list:
    """Place NPC groups: fixed `at` tiles, or random open tiles in `rect` (no water, scenery or stacking).

    `placement = "shore"` instead picks sea or river tiles beside walkable land, for fishing spots.
    """
    blocked = _occupied(canvas, source)
    rows = []

    def water(p):
        return canvas.inside(*p) and canvas.tile(*p).settings & 1 and canvas.tile(*p).overlay

    for group in groups:
        plane = group.get("plane", 0)
        shore = group.get("placement") == "shore"
        if "at" in group:
            x, y, plane = _at(group, areas)
            candidates = [(x, y)]
            count = 1
        else:
            ax0, ay0, ax1, ay1 = _rect(group, areas)
            candidates = None
            count = group["count"]
        placed, tries = 0, 0
        while placed < count and tries < count * 300:
            tries += 1
            npc_id = rng.choice(group["ids"])
            size = max(1, source.npc_def(npc_id).size)
            x, y = candidates[0] if candidates else (rng.randrange(ax0, ax1), rng.randrange(ay0, ay1))
            cells = [(x + i, y + j) for i in range(size) for j in range(size)]
            if any(not canvas.inside(cx, cy) or (cx, cy) in blocked for cx, cy in cells):
                if candidates:
                    break
                continue
            if shore:
                ok = water((x, y)) and any(not canvas.tile(*n).settings & 1 for n in _neighbours(x, y)
                                           if canvas.inside(*n))
            else:
                ok = not any(canvas.tile(cx, cy, plane).settings & 1 for cx, cy in cells)
            if not ok:
                if candidates:
                    break
                continue
            rows.append((npc_id, x, y, plane, rng.randrange(8), group.get("wander", WANDER_FROM_DEF), 0))
            blocked.update(cells)
            placed += 1
        if placed < count:
            where = group.get("at", group.get("rect"))
            raise SystemExit(f"npc group {group['ids']} placed {placed}/{count} at {where}; move or widen it")
    return rows


def _items(items: list, areas: dict) -> list:
    return [(i["id"], i.get("quantity", 1), *_at(i, areas), 0, i.get("respawn", DEFAULT_ITEM_RESPAWN))
            for i in items]


def _area(item: dict, areas: dict) -> Area:
    name = item.get("area", "surface")
    if name not in areas:
        raise SystemExit(f"unknown area {name!r}")
    return areas[name]


def _at(item: dict, areas: dict, key: str = "at") -> tuple[int, int, int]:
    area = _area(item, areas)
    at = item[key]
    plane = at[2] if len(at) > 2 else item.get("plane", 0)
    return area.rx * SIZE + at[0], area.ry * SIZE + at[1], plane


def _rect(item: dict, areas: dict) -> tuple[int, int, int, int]:
    area = _area(item, areas)
    x0, y0, x1, y1 = item["rect"]
    return area.rx * SIZE + x0, area.ry * SIZE + y0, area.rx * SIZE + x1, area.ry * SIZE + y1


def compile_gym(spec_path: Path, source_cache: Path, source_edges_path: Path | None = None) -> Gym:
    spec = tomllib.loads(spec_path.read_text())
    m = spec["map"]
    areas = {a["name"]: Area(a["name"], a["kind"], *a["origin"], *a["size"], a.get("ocean_ring", 0))
             for a in spec["area"]}
    squares = [sq for a in areas.values() for sq in a.squares()]
    if len(squares) != len(set(squares)):
        raise SystemExit("areas overlap")
    canvas = Canvas(set(squares))
    source = Source(RcCacheStore(source_cache))
    mats = spec["materials"]
    rng = random.Random(m["seed"])

    for area in areas.values():
        if area.kind == "island" and spec["island"].get("coast") == "smooth":
            _island_smooth(canvas, area, spec["island"], mats, m["seed"])
        elif area.kind == "island":
            _island(canvas, area, spec["island"], mats, m["seed"])
        elif area.kind == "void":
            _void(canvas, area, mats)
        else:
            raise SystemExit(f"area {area.name}: unknown kind {area.kind!r}")
    _paint_zones(canvas, spec.get("paint", []), areas, mats)
    _rivers(canvas, spec.get("river", []), areas, mats)
    _paths(canvas, spec.get("path", []), areas, mats)
    replace_ids = {int(k): v for k, v in spec.get("replace", {}).items()}
    stamps = [(_stamp(canvas, source, s, areas, replace_ids), set(s.get("drop_ids", [])))
              for s in spec.get("stamp", [])]
    for loc in spec.get("loc", []):
        x, y, plane = _at(loc, areas)
        canvas.locs.append(Loc(loc["id"], plane, x, y, loc.get("type", 10), loc.get("rotation", 0)))
    _floors(canvas, spec.get("floor", []), areas, mats)
    for a in spec["area"]:  # a void area's empty tiles (rock between tunnels) can take one solid floor
        if a["kind"] == "void" and "fill_underlay" in a:
            for x, y in canvas.tiles_of(areas[a["name"]].squares()):
                t = canvas.tile(x, y)
                if not t.underlay and not t.overlay:
                    t.underlay = a["fill_underlay"]
                    if not any(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in canvas.reserved):
                        t.height = a.get("fill_height", t.height)  # stamped rock keeps its heights
    _fences(canvas, spec.get("fence", []), areas)
    for scatter in spec.get("scatter", []):
        _scatter(canvas, source, scatter, areas, rng)
    for decor in spec.get("decor", []):
        _decor(canvas, source, decor, areas, rng)

    for loc in canvas.locs:
        if not canvas.inside(loc.x, loc.y):
            raise SystemExit(f"loc {loc} falls outside the map")
        if not source.loc_def(loc.id).complete:
            raise SystemExit(f"loc id {loc.id} does not decode cleanly; the minimap exporter would fail")
    start = _at({"area": m.get("start_area", "surface"), "at": m["start"]}, areas)
    respawn = _at({"area": m.get("respawn_area", "surface"), "at": m.get("respawn", m["start"])}, areas)
    for name, tile in (("start", start), ("respawn", respawn)):
        if canvas.tile(tile[0], tile[1]).settings & 1 or tile[:2] in _occupied(canvas, source):
            raise SystemExit(f"{name} tile {tile} is blocked")

    edges = _links(canvas, source, spec.get("link", []), areas)
    if source_edges_path is not None:
        edges += _stamp_edges(canvas, source, stamps, read_traversal(source_edges_path))
    zones = _zones(spec.get("zone", []), areas)
    npc_spawns = _npcs(canvas, source, spec.get("npc", []), areas, rng)
    ground_items = _items(spec.get("item", []), areas)

    mapsquares = {}
    for (rx, ry), tiles in canvas.squares.items():
        locs = [replace(l, x=l.x - rx * SIZE, y=l.y - ry * SIZE) for l in canvas.locs
                if l.x >> 6 == rx and l.y >> 6 == ry]
        mapsquares[(rx, ry)] = (encode_terrain(tiles), encode_locs(locs))
    landmarks = [(lm["name"], *_at(lm, areas)) for lm in spec.get("landmark", [])]
    if len(landmarks) > 32:  # RC_GYM_MAX_LANDMARKS
        raise SystemExit(f"{len(landmarks)} landmarks: the engine keeps at most 24")
    return Gym(m["name"], mapsquares, areas, start, respawn, npc_spawns, ground_items, edges, zones,
               landmarks)
