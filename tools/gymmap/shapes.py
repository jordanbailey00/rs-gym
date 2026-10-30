"""Smooth floors for the map compiler: curves, value noise and OSRS overlay shapes.

The client draws a tile's overlay over part of the tile when the tile has a shape (12 shapes, each
in four rotations), the way real roads, rivers and coasts get diagonal and rounded edges. A floor
here is a region given as a point test in continuous tile coordinates (tile x, y spans x..x+1 and
y..y+1, y north). Each edge tile samples the region on a 6x6 grid and takes the shape whose
overlay triangles, as the terrain exporter draws them, cover the same samples best.
"""

from __future__ import annotations

import math
from functools import lru_cache

from export_terrain import TILE_SHAPE_PATHS, TILE_SHAPE_POINTS, _rotate_scene_tile_point

GRID = 6
SUBSAMPLES = [((c + 0.5) / GRID, 1 - (r + 0.5) / GRID) for r in range(GRID) for c in range(GRID)]
SAMPLES = GRID * GRID
# Tile points of the client's shapes as (x, y) fractions (y north): corners, edge midpoints, inner.
_POINT_XY = {1: (0, 0), 2: (0.5, 0), 3: (1, 0), 4: (1, 0.5), 5: (1, 1), 6: (0.5, 1), 7: (0, 1), 8: (0, 0.5),
             9: (0.5, 0.25), 10: (0.75, 0.5), 11: (0.5, 0.75), 12: (0.25, 0.5),
             13: (0.25, 0.25), 14: (0.75, 0.25), 15: (0.75, 0.75), 16: (0.25, 0.75)}


def _overlay_triangles(shape: int, rotation: int) -> list:
    """The overlay part of map shape `shape` (1..11) in `rotation`, as the terrain exporter emits it."""
    scene = shape + 1
    points = [_POINT_XY[_rotate_scene_tile_point(p, rotation)] for p in TILE_SHAPE_POINTS[scene]]
    path = TILE_SHAPE_PATHS[scene]
    out = []
    for i in range(0, len(path), 4):
        kind, *idx = path[i:i + 4]
        idx = [(v - rotation) & 3 if v < 4 else v for v in idx]
        if kind == 1:
            out.append(tuple(points[v] for v in idx))
    return out


def _in_triangle(px: float, py: float, tri) -> bool:
    (ax, ay), (bx, by), (cx, cy) = tri
    d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by)
    d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy)
    d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay)
    return not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0))


# (shape, rotation, mask): which samples each shaped tile covers with its overlay.
_MASKS = [(s, r, tuple(1 if any(_in_triangle(x, y, t) for t in _overlay_triangles(s, r)) else 0
                       for x, y in SUBSAMPLES))
          for s in range(1, 12) for r in range(4)]


def _rows(bits: tuple) -> list[int]:
    return [sum(bits[r * GRID:(r + 1) * GRID]) for r in range(GRID)]


def _cols(bits: tuple) -> list[int]:
    return [sum(bits[c::GRID]) for c in range(GRID)]


def _straight(counts: list[int]) -> bool:
    """Each row (or column) is all in or all out, give or take one sample: a straight edge across."""
    return all(min(v, GRID - v) <= 1 for v in counts)


def _ragged(counts: list[int]) -> int:
    return sum(min(v, GRID - v) for v in counts)


# Shapes whose overlay edge runs straight across the tile: a slanted shape repeated along a straight
# edge reads as a sawtooth, so straight edges only use these.
_ACROSS = [m for m in _MASKS if _straight(_rows(m[2]))]
_ALONG = [m for m in _MASKS if _straight(_cols(m[2]))]


@lru_cache(maxsize=None)
def fit_shape(bits: tuple) -> tuple[int, int] | None:
    """(shape, rotation) for a tile whose samples inside the region are `bits`; None to leave the
    tile without this floor. Full tiles are shape 0."""
    n = sum(bits)
    best, err = None, n
    if SAMPLES - n < err:
        best, err = (0, 0), SAMPLES - n
    rows, cols = _rows(bits), _cols(bits)
    if _straight(rows) and _ragged(rows) < _ragged(cols):
        candidates = _ACROSS
    elif _straight(cols) and _ragged(cols) < _ragged(rows):
        candidates = _ALONG
    else:
        candidates = _MASKS
    for s, r, mask in candidates:
        d = sum(a != b for a, b in zip(mask, bits))
        if d < err:
            best, err = (s, r), d
    return best


def coverage(inside, x: int, y: int) -> tuple:
    """Which of tile (x, y)'s 16 samples fall inside the region `inside(px, py)`."""
    return tuple(1 if inside(x + dx, y + dy) else 0 for dx, dy in SUBSAMPLES)


def _hash(ix: int, iy: int, seed: int) -> float:
    h = (ix * 374761393 + iy * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def noise(x: float, y: float, scale: float, seed: int = 0) -> float:
    """Smooth value noise in 0..1 with features about `scale` tiles across."""
    fx, fy = x / scale, y / scale
    ix, iy = math.floor(fx), math.floor(fy)
    tx, ty = fx - ix, fy - iy
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    a = _hash(ix, iy, seed) + (_hash(ix + 1, iy, seed) - _hash(ix, iy, seed)) * tx
    b = _hash(ix, iy + 1, seed) + (_hash(ix + 1, iy + 1, seed) - _hash(ix, iy + 1, seed)) * tx
    return a + (b - a) * ty


def fractal(x: float, y: float, scale: float, seed: int = 0, octaves: int = 3) -> float:
    total, amp, norm = 0.0, 1.0, 0.0
    for k in range(octaves):
        total += amp * noise(x, y, scale / (2 ** k), seed + 101 * k)
        norm += amp
        amp *= 0.5
    return total / norm


def curve(points: list, step: float = 0.25) -> list[tuple[float, float]]:
    """A Catmull-Rom curve through tile centres, sampled about every `step` tiles."""
    pts = [(x + 0.5, y + 0.5) for x, y in points]
    if len(pts) == 1:
        return pts
    ext = [pts[0]] + pts + [pts[-1]]
    out = []
    for i in range(1, len(ext) - 2):
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        n = max(1, int(math.dist(p1, p2) / step))
        for k in range(n):
            t = k / n
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2
                                    + (-a + 3 * b - 3 * c + d) * t3)
                             for a, b, c, d in zip(p0, p1, p2, p3)))
    out.append(pts[-1])
    return out


class Band:
    """The tiles within a half-width of one or more curves; `half` may vary with position."""

    def __init__(self):
        self.cells: dict[tuple[int, int], list] = {}
        self.reach = 0.0

    def add(self, samples: list, half, wobble: float = 0.0, seed: int = 0) -> None:
        for k, (x, y) in enumerate(samples):
            h = half(k / max(1, len(samples) - 1)) if callable(half) else half
            if wobble:
                h += wobble * (noise(x, y, 6.0, seed) - 0.5) * 2
            self.cells.setdefault((math.floor(x), math.floor(y)), []).append((x, y, h))
            self.reach = max(self.reach, h)

    def near(self, tx: int, ty: int) -> list:
        """Curve samples that can reach tile (tx, ty)."""
        r = math.ceil(self.reach) + 1
        return [p for i in range(tx - r, tx + r + 1) for j in range(ty - r, ty + r + 1)
                for p in self.cells.get((i, j), ())]

    def bits(self, tx: int, ty: int) -> tuple:
        pts = self.near(tx, ty)
        return tuple(1 if any((tx + dx - x) ** 2 + (ty + dy - y) ** 2 < h * h for x, y, h in pts) else 0
                     for dx, dy in SUBSAMPLES)

    def tiles(self) -> set[tuple[int, int]]:
        r = math.ceil(self.reach) + 1
        out = set()
        for (i, j) in self.cells:
            out.update((i + a, j + b) for a in range(-r, r + 1) for b in range(-r, r + 1))
        return out
