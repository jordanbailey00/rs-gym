"""Tests for the gym map compiler (tools/gymmap). Run: python3 -m unittest discover -s tests"""

import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "cache_pipeline"))

import gymmap  # noqa: E402,F401
from gymmap.cachewrite import write_overlay_cache  # noqa: E402
from gymmap.compile import _rotate_loc, _rotate_tile, compile_gym  # noqa: E402
from gymmap.mapformat import PROCEDURAL, Loc, Tile, decode_locs, decode_terrain, empty_tiles, encode_locs, encode_terrain  # noqa: E402
from rc_cache import RcCacheStore, find_all_map_region_files, iter_location_placements, read_map_region_file  # noqa: E402

CACHE = ROOT / "data/source/b237-openrs2-2528/cache"
SPEC = ROOT / "maps/gym_v0.toml"
needs_cache = unittest.skipUnless(CACHE.is_dir(), "copied b237 cache not installed")


class MapFormatTest(unittest.TestCase):
    def test_terrain_round_trip_edge_values(self):
        tiles = empty_tiles()
        tiles[0][0][0] = Tile(height=255, underlay=0xFFFF - 81, overlay=0xFFFF, shape=11, rotation=3, settings=32)
        tiles[1][63][63] = Tile(height=0)
        tiles[3][5][9] = Tile(height=PROCEDURAL, overlay=0, shape=2, rotation=1)  # shape kept without overlay
        self.assertEqual(decode_terrain(encode_terrain(tiles)), tiles)

    def test_terrain_rejects_truncation(self):
        with self.assertRaises(ValueError):
            decode_terrain(encode_terrain(empty_tiles())[:-2])

    def test_locs_round_trip_large_ids_and_shared_tiles(self):
        locs = [Loc(5, 0, 1, 2, 10, 0), Loc(5, 0, 1, 2, 22, 3), Loc(40000, 3, 63, 63, 0, 1), Loc(65535, 1, 0, 0, 11, 2)]
        self.assertEqual(sorted(decode_locs(encode_locs(locs)), key=repr), sorted(locs, key=repr))
        self.assertEqual(encode_locs([]), b"\x00")

    def test_rotations_compose_to_identity(self):
        w, h, fw, fh = 7, 4, 3, 2
        for i in range(w - fw + 1):
            for j in range(h - fh + 1):
                x, y, cw, ch, sw, sh = i, j, fw, fh, w, h
                for _ in range(4):
                    x, y = _rotate_loc(x, y, cw, ch, sw, sh, 1)
                    cw, ch, sw, sh = ch, cw, sh, sw
                    self.assertTrue(0 <= x <= sw - cw and 0 <= y <= sh - ch)
                self.assertEqual((x, y), (i, j))
        self.assertEqual(_rotate_tile(0, 0, w, h, 1), (0, w - 1))  # south-west corner turns to north-west

    @needs_cache
    def test_real_mapsquares_round_trip(self):
        store = RcCacheStore(CACHE)
        squares = [(50, 50), (48, 48), (49, 53), (51, 52), (37, 49), (35, 47)]
        for rx, ry in squares:
            terrain = read_map_region_file(store, rx, ry, "terrain")
            tiles = decode_terrain(terrain)
            self.assertEqual(decode_terrain(encode_terrain(tiles)), tiles)
            locs = decode_locs(read_map_region_file(store, rx, ry, "locations"))
            self.assertEqual(sorted(decode_locs(encode_locs(locs)), key=repr), sorted(set(locs), key=repr))


@needs_cache
class CompileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gym = compile_gym(SPEC, CACHE)

    def test_layout_and_determinism(self):
        self.assertEqual(sorted(self.gym.mapsquares), [(x, y) for x in range(1, 6) for y in range(1, 6)])
        self.assertEqual(self.gym.area, (128, 128, 320, 320))
        self.assertEqual(compile_gym(SPEC, CACHE).mapsquares, self.gym.mapsquares)

    def test_viewer_start_matches_spec(self):
        header = (ROOT / "rc-viewer/dev_validation.h").read_text()
        x = int(re.search(r"RS_GYM_START_X = (\d+)", header).group(1))
        y = int(re.search(r"RS_GYM_START_Y = (\d+)", header).group(1))
        self.assertEqual((x, y, 0), self.gym.start)

    def test_ocean_ring_is_blocked_water(self):
        for (rx, ry), (terrain, locs) in self.gym.mapsquares.items():
            if rx in (1, 5) or ry in (1, 5):
                tiles = decode_terrain(terrain)
                self.assertTrue(all(t.settings & 1 and t.overlay for col in tiles[0] for t in col))
                self.assertEqual(decode_locs(locs), [])

    def test_stamps_keep_their_interactables(self):
        ids = {loc.id for _, locs in self.gym.mapsquares.values() for loc in decode_locs(locs)}
        self.assertIn(10355, ids)  # Draynor bank booth
        self.assertIn(24009, ids)  # Lumbridge furnace
        self.assertTrue({10943, 11161} & ids)  # copper rocks from the Varrock mine

    def test_overlay_cache_reads_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "cache"
            write_overlay_cache(CACHE, out, self.gym.mapsquares)
            store = RcCacheStore(out)
            regions = find_all_map_region_files(store)
            self.assertEqual(sorted((e.region_x, e.region_y) for e in regions.values()), sorted(self.gym.mapsquares))
            terrain = read_map_region_file(store, 3, 3, "terrain")
            self.assertEqual(terrain, self.gym.mapsquares[(3, 3)][0])
            placements = list(iter_location_placements(read_map_region_file(store, 3, 3, "locations"), 3, 3))
            self.assertEqual(len(placements), len(decode_locs(self.gym.mapsquares[(3, 3)][1])))
            self.assertTrue(store.read_group(2, 6))  # other indices link back to the source cache


if __name__ == "__main__":
    unittest.main()
