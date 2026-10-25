#!/usr/bin/env python3
"""Reusable mapsquare selections for cache-derived region exports."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from cache_pipeline.rc_cache import RcCacheStore, find_all_map_region_files

REGION_SIZE = 64
MAX_REGION_COORD = 255
DEFAULT_NAMED_RADIUS = 2

VARROCK_CENTER = (3213, 3428)
EDGEVILLE_CENTER = (3093, 3493)

_AROUND_PATTERN = re.compile(r"around:(\d+),(\d+),r=(\d+)")


@dataclass(frozen=True)
class RegionSet:
    """A deterministic mapsquare selection and filesystem-safe output name."""

    name: str
    regions: tuple[tuple[int, int], ...]


def around_tile(x: int, y: int, radius: int) -> list[tuple[int, int]]:
    """Return a row-major square of mapsquares around one world tile."""
    max_tile = (MAX_REGION_COORD + 1) * REGION_SIZE - 1
    if not 0 <= x <= max_tile or not 0 <= y <= max_tile:
        raise ValueError(f"tile coordinates out of range: {x},{y}")
    if not 0 <= radius <= MAX_REGION_COORD:
        raise ValueError(f"region radius out of range: {radius}")

    center_x = x // REGION_SIZE
    center_y = y // REGION_SIZE
    min_x = max(0, center_x - radius)
    max_x = min(MAX_REGION_COORD, center_x + radius)
    min_y = max(0, center_y - radius)
    max_y = min(MAX_REGION_COORD, center_y + radius)
    return [
        (region_x, region_y)
        for region_y in range(min_y, max_y + 1)
        for region_x in range(min_x, max_x + 1)
    ]


def varrock() -> list[tuple[int, int]]:
    """Return the existing 5x5 Varrock development region set."""
    return around_tile(*VARROCK_CENTER, DEFAULT_NAMED_RADIUS)


def edgeville() -> list[tuple[int, int]]:
    """Return a 5x5 development region set centered on Edgeville."""
    return around_tile(*EDGEVILLE_CENTER, DEFAULT_NAMED_RADIUS)


def full_from_cache(cache_root: Path) -> list[tuple[int, int]]:
    """Return every mapsquare represented by the cache's map index."""
    entries = find_all_map_region_files(RcCacheStore(cache_root))
    regions = {(entry.region_x, entry.region_y) for entry in entries.values()}
    return sorted(regions, key=lambda region: (region[1], region[0]))


def resolve(spec: str, cache_root: Path | None = None) -> RegionSet:
    """Resolve a named, radius-based, or full-cache region-set expression."""
    normalized = spec.strip().lower()
    if normalized == "varrock":
        return RegionSet("varrock", tuple(varrock()))
    if normalized == "edgeville":
        return RegionSet("edgeville", tuple(edgeville()))
    if normalized == "full":
        if cache_root is None:
            raise ValueError("region set 'full' requires RUNEC_B237_CACHE")
        return RegionSet("full", tuple(full_from_cache(cache_root)))

    match = _AROUND_PATTERN.fullmatch(normalized)
    if match:
        x, y, radius = (int(value) for value in match.groups())
        return RegionSet(
            f"around_{x}_{y}_r{radius}",
            tuple(around_tile(x, y, radius)),
        )

    raise ValueError(
        "unknown region set; use varrock, edgeville, "
        "around:<x>,<y>,r=<radius>, or full"
    )


def format_regions(regions: tuple[tuple[int, int], ...] | list[tuple[int, int]]) -> list[str]:
    """Format mapsquares for cache exporter command arguments."""
    return [f"{region_x},{region_y}" for region_x, region_y in regions]
