"""Compile a gym map spec (TOML) into mapsquares built from source-cache content.

Spec coordinates are tiles relative to the playable area's south-west corner.
Pipeline: island terrain -> paths -> stamps (real-world rects, rotated) -> locs -> scatter.
"""

from __future__ import annotations

import math
import random
import tomllib
from collections import deque
from dataclasses import dataclass, replace
from pathlib import Path

from export_terrain import calculate_height
from rc_cache import RcCacheStore, read_map_region_file
from rc_cache.definitions import decode_location_definition, decode_npc_definition

from .mapformat import PLANES, PROCEDURAL, SIZE, Loc, Tile, decode_locs, decode_terrain, encode_locs, encode_terrain

OBJECT_CONFIG_GROUP = 6
NPC_CONFIG_GROUP = 9
SCENERY_TYPES = (10, 11)  # locs whose width/length occupy tiles
WANDER_FROM_DEF = 255


@dataclass
class Gym:
    """Compiled map: encoded mapsquares plus what the build step writes beside them."""
    name: str
    mapsquares: dict[tuple[int, int], tuple[bytes, bytes]]
    area: tuple[int, int, int, int]  # playable world tiles x0, y0, x1, y1 (exclusive)
    start: tuple[int, int, int]  # world x, y, plane
    npc_spawns: list[tuple[int, int, int, int, int, int, int]]  # npc_id, x, y, plane, direction, wander, flags


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
    """Every exported tile (playable area plus ocean ring), addressed in world tiles."""

    def __init__(self, x0: int, y0: int, w: int, h: int):
        self.x0, self.y0, self.w, self.h = x0, y0, w, h
        self.tiles = [[[Tile() for _ in range(h)] for _ in range(w)] for _ in range(PLANES)]
        self.locs: list[Loc] = []
        self.reserved: list[tuple[int, int, int, int]] = []  # stamp rects, world x0, y0, x1, y1

    def inside(self, x: int, y: int) -> bool:
        return self.x0 <= x < self.x0 + self.w and self.y0 <= y < self.y0 + self.h

    def tile(self, x: int, y: int, plane: int = 0) -> Tile:
        return self.tiles[plane][x - self.x0][y - self.y0]


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


def _island(canvas: Canvas, area: tuple[int, int, int, int], spec: dict, mats: dict, seed: int) -> None:
    rng = random.Random(seed)
    x0, y0, x1, y1 = area
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    radius = spec["radius"]
    wobble = [(spec["wobble"] / k, k, rng.uniform(0, 2 * math.pi)) for k in range(2, 7)]

    land = set()
    for x in range(x0, x1):
        for y in range(y0, y1):
            angle = math.atan2(y - cy, x - cx)
            edge = radius * (1 + sum(a * math.sin(k * angle + p) for a, k, p in wobble))
            if math.hypot(x - cx, y - cy) < edge:
                land.add((x, y))

    # Distance (in tiles) from the coast, over land and water separately.
    dist = {}
    queue = deque()
    for x in range(canvas.x0, canvas.x0 + canvas.w):
        for y in range(canvas.y0, canvas.y0 + canvas.h):
            if (x, y) in land and any(n not in land for n in _neighbours(x, y)):
                dist[(x, y)] = 0
                queue.append((x, y))
    water_dist = {}
    wq = deque()
    for x, y in list(queue):
        for n in _neighbours(x, y):
            if n not in land and canvas.inside(*n) and n not in water_dist:
                water_dist[n] = 1
                wq.append(n)
    for q, d, inside_land in ((queue, dist, True), (wq, water_dist, False)):
        while q:
            x, y = q.popleft()
            for n in _neighbours(x, y):
                if canvas.inside(*n) and ((n in land) == inside_land) and n not in d:
                    d[n] = d[(x, y)] + 1
                    q.append(n)

    sea, beach, ramp, scale = spec["sea_height"], spec["beach_width"], spec["ramp_tiles"], spec["height_scale"]
    depths = spec["water_depths"]  # [max distance, overlay] pairs, nearest first
    for x in range(canvas.x0, canvas.x0 + canvas.w):
        for y in range(canvas.y0, canvas.y0 + canvas.h):
            t = canvas.tile(x, y)
            if (x, y) in land:
                d = dist[(x, y)]
                hill = (calculate_height(x + 7919, y + 3571) - 10) * scale
                t.height = _clamp_height(sea + 2 + min(1.0, d / ramp) * hill)
                patch = calculate_height(x + 1013, y + 2027) > 40  # smooth blobs of the second grass
                mat = mats["sand"] if d < beach else mats["grass_alt"] if patch else mats["grass"]
            else:
                d = water_dist.get((x, y), 99)
                t.height = sea
                mat = next((mats[name] for limit, name in depths if d <= limit), mats[depths[-1][1]])
            _paint(t, mat)


def _neighbours(x: int, y: int):
    return ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1))


def _paint(t: Tile, mat: dict) -> None:
    t.underlay = mat.get("underlay", t.underlay)
    t.overlay = mat.get("overlay", 0)
    t.shape = 0
    t.rotation = 0
    t.settings = mat.get("settings", 0)


def _paths(canvas: Canvas, paths: list, origin: tuple[int, int], mats: dict) -> None:
    for path in paths:
        mat, half = mats[path.get("material", "path")], path.get("width", 2) / 2
        pts = [(origin[0] + px, origin[1] + py) for px, py in path["points"]]
        for (ax, ay), (bx, by) in zip(pts, pts[1:]):
            for x in range(min(ax, bx) - 3, max(ax, bx) + 4):
                for y in range(min(ay, by) - 3, max(ay, by) + 4):
                    if canvas.inside(x, y) and _segment_distance(x, y, ax, ay, bx, by) < half:
                        t = canvas.tile(x, y)
                        if not t.settings & 1:  # never pave water
                            _paint(t, mat)


def _segment_distance(px, py, ax, ay, bx, by) -> float:
    dx, dy = bx - ax, by - ay
    k = 0.0 if dx == dy == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - k * dx, py - ay - k * dy)


def _stamp(canvas: Canvas, source: Source, stamp: dict, origin: tuple[int, int]) -> None:
    sx, sy = stamp["source"]
    w, h = stamp["size"]
    r = stamp.get("rotation", 0) % 4
    dx, dy = origin[0] + stamp["at"][0], origin[1] + stamp["at"][1]
    dw, dh = (w, h) if r % 2 == 0 else (h, w)
    if not (canvas.inside(dx, dy) and canvas.inside(dx + dw - 1, dy + dh - 1)):
        raise SystemExit(f"stamp {stamp['name']!r} falls outside the map")

    # Fit plane-0 heights: shift the copy so its border matches the ground it lands on.
    border = [(i, j) for i in range(w) for j in range(h) if i in (0, w - 1) or j in (0, h - 1)]
    src_mean = sum(source.height(sx + i, sy + j) for i, j in border) / len(border)
    dst_mean = sum(canvas.tile(dx + ti, dy + tj).height
                   for ti, tj in (_rotate_tile(i, j, w, h, r) for i, j in border)) / len(border)
    offset = dst_mean - src_mean

    for plane in range(PLANES):
        for i in range(w):
            for j in range(h):
                src = source.tile(sx + i, sy + j, plane)
                ti, tj = _rotate_tile(i, j, w, h, r)
                t = replace(src, rotation=(src.rotation + r) & 3 if src.overlay or src.shape else src.rotation)
                if plane == 0:
                    t.height = _clamp_height(source.height(sx + i, sy + j) + offset)
                canvas.tiles[plane][dx + ti - canvas.x0][dy + tj - canvas.y0] = t

    drop = set(stamp.get("drop_ids", []))
    canvas.locs = [l for l in canvas.locs if not (dx <= l.x < dx + dw and dy <= l.y < dy + dh)]
    for loc in source.locs_in(sx, sy, w, h):
        if loc.id in drop:
            continue
        fw, fh = _footprint(source, loc)
        ni, nj = _rotate_loc(loc.x - sx, loc.y - sy, fw, fh, w, h, r)
        canvas.locs.append(Loc(loc.id, loc.plane, dx + ni, dy + nj, loc.type, (loc.rotation + r) & 3))
    canvas.reserved.append((dx, dy, dx + dw, dy + dh))
    _feather(canvas, (dx, dy, dx + dw, dy + dh), stamp.get("feather", 4))


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


def _occupied(canvas: Canvas, source: Source) -> set[tuple[int, int]]:
    taken = set()
    for loc in canvas.locs:
        if loc.plane == 0 and loc.type in SCENERY_TYPES:
            fw, fh = _footprint(source, loc)
            taken.update((loc.x + i, loc.y + j) for i in range(fw) for j in range(fh))
    return taken


def _scatter(canvas: Canvas, source: Source, spec: dict, origin: tuple[int, int], rng: random.Random) -> None:
    taken = _occupied(canvas, source)
    ax0, ay0, ax1, ay1 = spec["area"]
    spacing = spec.get("spacing", 1)
    placed, tries = 0, 0
    while placed < spec["count"] and tries < spec["count"] * 200:
        tries += 1
        loc_id = rng.choice(spec["ids"])
        x, y = origin[0] + rng.randrange(ax0, ax1), origin[1] + rng.randrange(ay0, ay1)
        loc = Loc(loc_id, 0, x, y, spec.get("type", 10), rng.randrange(4))
        fw, fh = _footprint(source, loc)
        cells = [(x + i, y + j) for i in range(-spacing, fw + spacing) for j in range(-spacing, fh + spacing)]
        if any(not canvas.inside(cx, cy) or (cx, cy) in taken for cx, cy in cells):
            continue
        if any(any(a <= cx < c and b <= cy < d for a, b, c, d in canvas.reserved) for cx, cy in cells):
            continue
        under = [canvas.tile(x + i, y + j) for i in range(fw) for j in range(fh)]
        if any(t.settings & 1 or t.overlay for t in under):  # land, not beach, path or water
            continue
        canvas.locs.append(loc)
        taken.update((x + i, y + j) for i in range(fw) for j in range(fh))
        placed += 1
    if placed < spec["count"]:
        raise SystemExit(f"scatter {spec['ids']} placed {placed}/{spec['count']}; widen its area")


def _npcs(canvas: Canvas, source: Source, groups: list, origin: tuple[int, int], rng: random.Random) -> list:
    """Place NPC groups on open land: no water, no blocked tiles, no scenery, no stacking."""
    blocked = _occupied(canvas, source)
    rows = []
    for group in groups:
        ax0, ay0, ax1, ay1 = group["area"]
        placed, tries = 0, 0
        while placed < group["count"] and tries < group["count"] * 200:
            tries += 1
            npc_id = rng.choice(group["ids"])
            size = max(1, source.npc_def(npc_id).size)
            x, y = origin[0] + rng.randrange(ax0, ax1), origin[1] + rng.randrange(ay0, ay1)
            cells = [(x + i, y + j) for i in range(size) for j in range(size)]
            if any(not canvas.inside(cx, cy) or (cx, cy) in blocked or canvas.tile(cx, cy).settings & 1
                   for cx, cy in cells):
                continue
            rows.append((npc_id, x, y, 0, rng.randrange(8), group.get("wander", WANDER_FROM_DEF), 0))
            blocked.update(cells)
            placed += 1
        if placed < group["count"]:
            raise SystemExit(f"npc group {group['ids']} placed {placed}/{group['count']}; widen its area")
    return rows


def compile_gym(spec_path: Path, source_cache: Path) -> Gym:
    spec = tomllib.loads(spec_path.read_text())
    m = spec["map"]
    (rx0, ry0), (sw, sh), ring = m["origin"], m["size"], m["ocean_ring"]
    area = (rx0 * SIZE, ry0 * SIZE, (rx0 + sw) * SIZE, (ry0 + sh) * SIZE)
    origin = area[:2]
    canvas = Canvas((rx0 - ring) * SIZE, (ry0 - ring) * SIZE, (sw + 2 * ring) * SIZE, (sh + 2 * ring) * SIZE)
    source = Source(RcCacheStore(source_cache))
    mats = spec["materials"]
    rng = random.Random(m["seed"])

    _island(canvas, area, spec["island"], mats, m["seed"])
    _paths(canvas, spec.get("path", []), origin, mats)
    for stamp in spec.get("stamp", []):
        _stamp(canvas, source, stamp, origin)
    for loc in spec.get("loc", []):
        canvas.locs.append(Loc(loc["id"], loc.get("plane", 0), origin[0] + loc["at"][0], origin[1] + loc["at"][1],
                               loc.get("type", 10), loc.get("rotation", 0)))
    for scatter in spec.get("scatter", []):
        _scatter(canvas, source, scatter, origin, rng)

    for loc in canvas.locs:
        if not canvas.inside(loc.x, loc.y):
            raise SystemExit(f"loc {loc} falls outside the map")
        if not source.loc_def(loc.id).complete:
            raise SystemExit(f"loc id {loc.id} does not decode cleanly; the minimap exporter would fail")
    sx, sy = m["start"]
    start = (origin[0] + sx, origin[1] + sy, 0)
    if canvas.tile(start[0], start[1]).settings & 1 or start[:2] in _occupied(canvas, source):
        raise SystemExit(f"start tile {m['start']} is blocked")
    npc_spawns = _npcs(canvas, source, spec.get("npc", []), origin, rng)

    mapsquares = {}
    for rx in range(rx0 - ring, rx0 + sw + ring):
        for ry in range(ry0 - ring, ry0 + sh + ring):
            bx, by = rx * SIZE - canvas.x0, ry * SIZE - canvas.y0
            tiles = [[canvas.tiles[p][bx + x][by:by + SIZE] for x in range(SIZE)] for p in range(PLANES)]
            locs = [replace(l, x=l.x - rx * SIZE, y=l.y - ry * SIZE) for l in canvas.locs
                    if l.x >> 6 == rx and l.y >> 6 == ry]
            mapsquares[(rx, ry)] = (encode_terrain(tiles), encode_locs(locs))
    return Gym(m["name"], mapsquares, area, start, npc_spawns)
