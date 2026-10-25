#!/usr/bin/env python3
"""Export OSRS-style per-mapsquare minimap rasters from b237."""

from __future__ import annotations

import argparse
import math
import struct
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from PIL import Image

from export_terrain import (
    FloorDef,
    RegionTerrain,
    apply_light,
    decode_floor_definitions_modern,
    hsl16_to_rgb,
    hsl_encode,
    load_texture_average_colors_modern,
    parse_terrain_full,
    visual_source_plane,
)
from rc_cache import (
    CONFIG_OBJECT,
    INDEX_CONFIGS,
    INDEX_SPRITES,
    RcCacheStore,
    decode_location_definition,
    find_group_id,
    iter_location_placements,
    load_sprite_group,
    read_map_region_file,
)
from source_inputs import B237_CACHE, require_path


SCENE_SIZE = 512
TILE_PIXELS = 4
REGION_TILES = 64
SCENE_BORDER_TILES = (SCENE_SIZE // TILE_PIXELS - REGION_TILES) // 2
MAPSQUARE_CATALOG_MAGIC = 0x3151534D

# Client minimap overlay masks. Cache paths are zero-based, so rendering adds
# one before selecting the corresponding 4x4 shape.
OVERLAY_SHAPES = (
    (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
    (1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    (1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1),
    (1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0),
    (0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1),
    (0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    (1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1),
    (1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0),
    (0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0),
    (1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1),
    (1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0),
    (0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1),
    (0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1),
)

OVERLAY_ROTATIONS = (
    (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15),
    (12, 8, 4, 0, 13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3),
    (15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0),
    (3, 7, 11, 15, 2, 6, 10, 14, 1, 5, 9, 13, 0, 4, 8, 12),
)

WALL_COLOR = (238, 238, 238, 255)
DOOR_COLOR = (238, 0, 0, 255)


def _palette_rgb(hsl16: int) -> tuple[int, int, int, int]:
    red, green, blue = hsl16_to_rgb(hsl16)

    def gamma(channel: int) -> int:
        return max(0, min(255, int(math.pow(channel / 256.0, 0.8) * 256.0)))

    return gamma(red), gamma(green), gamma(blue), 255


def _underlay_color(
    terrain: RegionTerrain,
    plane: int,
    tile_x: int,
    tile_y: int,
    underlays: dict[int, FloorDef],
) -> tuple[int, int, int, int]:
    if terrain.underlay_ids[plane][tile_x][tile_y] <= 0:
        return 0, 0, 0, 255
    hue = saturation = lightness = multiplier = count = 0
    for sample_x in range(max(0, tile_x - 5), min(REGION_TILES, tile_x + 6)):
        for sample_y in range(max(0, tile_y - 5), min(REGION_TILES, tile_y + 6)):
            underlay_id = terrain.underlay_ids[plane][sample_x][sample_y]
            definition = underlays.get(underlay_id - 1) if underlay_id > 0 else None
            if definition is None:
                continue
            hue += definition.blend_hue
            saturation += definition.saturation
            lightness += definition.luminance
            multiplier += definition.blend_hue_multiplier
            count += 1
    if count == 0 or multiplier == 0:
        return 0, 0, 0, 255
    packed = hsl_encode(
        (hue * 256) // multiplier,
        saturation // count,
        lightness // count,
    )
    return _palette_rgb(apply_light(packed, 96))


def _overlay_color(
    overlay_id: int,
    overlays: dict[int, FloorDef],
    texture_colors: dict[int, int],
) -> tuple[int, int, int, int]:
    definition = overlays.get(overlay_id - 1)
    if definition is None or definition.rgb == 0xFF00FF:
        return 0, 0, 0, 255
    if definition.secondary_rgb != -1:
        packed = hsl_encode(
            definition.secondary_hue,
            definition.secondary_saturation,
            definition.secondary_lightness,
        )
    elif definition.texture >= 0:
        packed = texture_colors.get(definition.texture, -1)
        if packed < 0:
            return 0, 0, 0, 255
    else:
        packed = hsl_encode(
            definition.hue,
            definition.saturation,
            definition.lightness,
        )
    return _palette_rgb(apply_light(packed, 96))


def _tile_origin(tile_x: int, tile_y: int) -> tuple[int, int]:
    return (
        (SCENE_BORDER_TILES + tile_x) * TILE_PIXELS,
        (SCENE_BORDER_TILES + REGION_TILES - 1 - tile_y) * TILE_PIXELS,
    )


def _draw_floor_tile(
    pixels,
    terrain: RegionTerrain,
    plane: int,
    tile_x: int,
    tile_y: int,
    underlays: dict[int, FloorDef],
    overlays: dict[int, FloorDef],
    texture_colors: dict[int, int],
) -> None:
    underlay = _underlay_color(terrain, plane, tile_x, tile_y, underlays)
    overlay_id = terrain.overlay_ids[plane][tile_x][tile_y] & 0xFFFF
    pixel_x, pixel_y = _tile_origin(tile_x, tile_y)
    if overlay_id == 0:
        for dy in range(TILE_PIXELS):
            for dx in range(TILE_PIXELS):
                pixels[pixel_x + dx, pixel_y + dy] = underlay
        return

    overlay = _overlay_color(overlay_id, overlays, texture_colors)
    scene_shape = terrain.shapes[plane][tile_x][tile_y] + 1
    rotation = terrain.rotations[plane][tile_x][tile_y] & 3
    if not 0 <= scene_shape < len(OVERLAY_SHAPES):
        raise ValueError(
            f"unsupported overlay shape {scene_shape} at {tile_x},{tile_y}"
        )
    mask = OVERLAY_SHAPES[scene_shape]
    rotated = OVERLAY_ROTATIONS[rotation]
    for index in range(TILE_PIXELS * TILE_PIXELS):
        dx = index % TILE_PIXELS
        dy = index // TILE_PIXELS
        pixels[pixel_x + dx, pixel_y + dy] = (
            overlay if mask[rotated[index]] else underlay
        )


def _visible_floor_planes(
    terrain: RegionTerrain, tile_x: int, tile_y: int, target_plane: int
) -> list[int]:
    source_plane = visual_source_plane(terrain, tile_x, tile_y, target_plane)
    result = []
    if terrain.settings[source_plane][tile_x][tile_y] & 0x18 == 0:
        result.append(source_plane)
    upper = target_plane + 1
    if upper < 4 and terrain.settings[upper][tile_x][tile_y] & 0x8:
        result.append(upper)
    return result


def _placement_visible(terrain: RegionTerrain, placement, target_plane: int) -> bool:
    tile_x = placement.local_x
    tile_y = placement.local_y
    display_plane = placement.plane
    if display_plane > 0 and terrain.settings[1][tile_x][tile_y] & 0x2:
        display_plane -= 1
    if display_plane == target_plane:
        return terrain.settings[target_plane][tile_x][tile_y] & 0x18 == 0
    return (
        placement.plane == target_plane + 1
        and terrain.settings[target_plane + 1][tile_x][tile_y] & 0x8 != 0
    )


def _draw_wall(pixels, placement, color: tuple[int, int, int, int]) -> None:
    left, top = _tile_origin(placement.local_x, placement.local_y)

    def horizontal(y: int) -> None:
        for x in range(TILE_PIXELS):
            pixels[left + x, top + y] = color

    def vertical(x: int) -> None:
        for y in range(TILE_PIXELS):
            pixels[left + x, top + y] = color

    rotation = placement.rotation
    shape = placement.shape
    if shape in (0, 2):
        (vertical, horizontal, vertical, horizontal)[rotation](
            (0, 0, TILE_PIXELS - 1, TILE_PIXELS - 1)[rotation]
        )
    if shape == 2:
        (horizontal, vertical, horizontal, vertical)[rotation](
            (0, TILE_PIXELS - 1, TILE_PIXELS - 1, 0)[rotation]
        )
    elif shape == 3:
        dx, dy = ((0, 0), (3, 0), (3, 3), (0, 3))[rotation]
        pixels[left + dx, top + dy] = color
    elif shape == 9:
        for offset in range(TILE_PIXELS):
            y = TILE_PIXELS - 1 - offset if rotation in (0, 2) else offset
            pixels[left + offset, top + y] = color


def _draw_map_scene(image: Image.Image, placement, definition, sprite) -> None:
    sprite_image = Image.frombytes("RGBA", (sprite.width, sprite.height), sprite.pixels)
    left = (SCENE_BORDER_TILES + placement.local_x) * TILE_PIXELS
    top = (
        SCENE_BORDER_TILES + REGION_TILES - placement.local_y - definition.length
    ) * TILE_PIXELS
    left += (definition.width * TILE_PIXELS - sprite.width) // 2
    top += (definition.length * TILE_PIXELS - sprite.height) // 2
    image.alpha_composite(sprite_image, (left, top))


def export_minimap(
    cache_dir: Path,
    region_x: int,
    region_y: int,
    output: Path,
    target_plane: int = 0,
) -> None:
    """Export one mapsquare in the client's centered 512px scene format."""
    store = RcCacheStore(cache_dir)
    terrain_data = read_map_region_file(store, region_x, region_y, "terrain")
    location_data = read_map_region_file(store, region_x, region_y, "locations")
    if terrain_data is None or location_data is None:
        raise ValueError(f"missing map data for region {region_x},{region_y}")

    terrain = parse_terrain_full(terrain_data, region_x * 64, region_y * 64)
    underlays, overlays = decode_floor_definitions_modern(store)
    texture_colors = load_texture_average_colors_modern(store)
    image = Image.new("RGBA", (SCENE_SIZE, SCENE_SIZE), (0, 0, 0, 255))
    pixels = image.load()
    for tile_y in range(REGION_TILES):
        for tile_x in range(REGION_TILES):
            for plane in _visible_floor_planes(
                terrain, tile_x, tile_y, target_plane
            ):
                _draw_floor_tile(
                    pixels, terrain, plane, tile_x, tile_y,
                    underlays, overlays, texture_colors,
                )

    placements = [
        placement
        for placement in iter_location_placements(
            location_data, region_x, region_y
        )
        if _placement_visible(terrain, placement, target_plane)
    ]
    object_files = store.read_group(INDEX_CONFIGS, CONFIG_OBJECT)
    definitions = {}
    for object_id in sorted({placement.object_id for placement in placements}):
        data = object_files.get(object_id)
        if data is None:
            raise ValueError(f"missing object definition {object_id}")
        definition = decode_location_definition(object_id, data)
        if not definition.complete:
            raise ValueError(
                f"object definition {object_id} has unsupported opcode "
                f"{definition.unknown_opcode}"
            )
        definitions[object_id] = definition

    sprite_manifest = store.read_index_manifest(INDEX_SPRITES)
    map_scene_group = find_group_id(sprite_manifest, "mapscene")
    if map_scene_group is None:
        raise ValueError("cache sprite group 'mapscene' is missing")
    map_scenes = load_sprite_group(store, map_scene_group)
    for placement in placements:
        definition = definitions[placement.object_id]
        uses_map_scene = (
            definition.map_scene_id >= 0
            and placement.shape in (0, 1, 2, 3, 9, 10, 11, 22)
        )
        if uses_map_scene:
            if definition.map_scene_id >= len(map_scenes):
                raise ValueError(
                    f"object {placement.object_id} references missing "
                    f"map-scene sprite {definition.map_scene_id}"
                )
            _draw_map_scene(
                image, placement, definition, map_scenes[definition.map_scene_id]
            )
        elif placement.shape in (0, 2, 3, 9):
            is_door = definition.wall_or_door > 0 or (
                definition.wall_or_door < 0 and any(definition.actions)
            )
            _draw_wall(pixels, placement, DOOR_COLOR if is_door else WALL_COLOR)

    output.parent.mkdir(parents=True, exist_ok=True)
    image.convert("RGB").save(output, format="PNG", optimize=False)


def minimap_path(output_dir: Path, region_x: int, region_y: int,
                 plane: int) -> Path:
    return output_dir / f"{region_x}_{region_y}.p{plane}.minimap.png"


def export_minimap_split(
    cache_dir: Path,
    regions: list[tuple[int, int]],
    output_dir: Path,
    planes: list[int] | tuple[int, ...] = (0, 1, 2, 3),
) -> list[Path]:
    if not cache_dir.is_dir():
        raise FileNotFoundError(f"modern cache directory not found: {cache_dir}")
    if not regions:
        raise ValueError("at least one region is required")
    if not planes or any(plane < 0 or plane > 3 for plane in planes):
        raise ValueError("planes must contain values in 0..3")
    generated = []
    for region_x, region_y in sorted(set(regions), key=lambda value: (value[1], value[0])):
        for plane in planes:
            output = minimap_path(output_dir, region_x, region_y, plane)
            export_minimap(cache_dir, region_x, region_y, output, plane)
            generated.append(output)
        print(f"minimap mapsquare {region_x},{region_y}: {len(planes)} planes")
    return generated


def load_catalog(path: Path) -> list[tuple[int, int]]:
    payload = path.read_bytes()
    if len(payload) < 8:
        raise ValueError(f"truncated mapsquare catalog: {path}")
    magic, count = struct.unpack_from("<II", payload)
    if magic != MAPSQUARE_CATALOG_MAGIC or len(payload) != 8 + count * 2:
        raise ValueError(f"invalid mapsquare catalog: {path}")
    return [tuple(payload[offset:offset + 2]) for offset in range(8, len(payload), 2)]


def _export_batch(args: tuple[Path, list[tuple[int, int]], Path, list[int]]) -> int:
    return len(export_minimap_split(*args))


def partition_regions(regions: list[tuple[int, int]], jobs: int) -> list[list[tuple[int, int]]]:
    workers = min(jobs, len(regions))
    size, remainder = divmod(len(regions), workers)
    batches = []
    offset = 0
    for index in range(workers):
        count = size + (1 if index < remainder else 0)
        batches.append(regions[offset:offset + count])
        offset += count
    return batches


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=B237_CACHE)
    parser.add_argument("--catalog", type=Path)
    parser.add_argument("--region-x", type=int)
    parser.add_argument("--region-y", type=int)
    parser.add_argument("--planes", default="0,1,2,3")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args()
    cache = require_path(args.cache, "--cache", "RUNEC_B237_CACHE")
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    planes = [int(value) for value in args.planes.split(",") if value]
    if not planes or any(plane < 0 or plane > 3 for plane in planes):
        parser.error("--planes values must be in 0..3")
    if args.catalog:
        regions = load_catalog(args.catalog)
    elif args.region_x is not None and args.region_y is not None:
        regions = [(args.region_x, args.region_y)]
    else:
        parser.error("provide --catalog or --region-x/--region-y")

    batches = partition_regions(regions, args.jobs)
    work = [(cache, batch, args.output_dir, planes) for batch in batches]
    if len(work) == 1:
        _export_batch(work[0])
    else:
        with ProcessPoolExecutor(max_workers=len(work)) as executor:
            list(executor.map(_export_batch, work))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
