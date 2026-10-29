"""Tests for the gym map compiler (tools/gymmap). Run: python3 -m unittest discover -s tests"""

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
from gymmap.drops import merge_gym_drops, read_drops, write_drops  # noqa: E402
from gymmap.spatial import AREA_MULTICOMBAT, AREA_WILDERNESS_LEVEL, read_traversal, write_traversal  # noqa: E402
from gymmap.mapformat import PROCEDURAL, Loc, Tile, decode_locs, decode_terrain, empty_tiles, encode_locs, encode_terrain  # noqa: E402
from rc_cache import RcCacheStore, find_all_map_region_files, iter_location_placements, read_map_region_file  # noqa: E402

CACHE = ROOT / "data/source/b237-openrs2-2528/cache"
SPEC = ROOT / "maps/island.toml"
EDGES = ROOT / "data/source/traversal_edges.b237.bin"
DROPS = ROOT / "data/source/drops.b237.bin"
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
        cls.gym = compile_gym(SPEC, CACHE, EDGES if EDGES.exists() else None)

    def test_areas_and_determinism(self):
        surface = {(x, y) for x in range(1, 6) for y in range(1, 6)}
        dungeon = {(x, y) for x in range(8, 10) for y in range(2, 4)}
        self.assertEqual(set(self.gym.mapsquares), surface | dungeon)
        self.assertEqual(self.gym.areas["surface"].bounds, (128, 128, 320, 320))
        self.assertEqual(compile_gym(SPEC, CACHE, EDGES if EDGES.exists() else None).mapsquares, self.gym.mapsquares)

    def test_ocean_ring_is_blocked_water(self):
        for (rx, ry), (terrain, locs) in self.gym.mapsquares.items():
            if rx <= 5 and (rx in (1, 5) or ry in (1, 5)):
                tiles = decode_terrain(terrain)
                self.assertTrue(all(t.settings & 1 and t.overlay for col in tiles[0] for t in col))
                self.assertEqual(decode_locs(locs), [])

    def test_stamps_keep_their_interactables(self):
        ids = {loc.id for _, locs in self.gym.mapsquares.values() for loc in decode_locs(locs)}
        for loc_id in (10355, 24009, 409, 1817, 11797, 14880):  # booth, furnace, altar, lair lever, stairs, trapdoor
            self.assertIn(loc_id, ids)
        self.assertTrue({10943, 11161} & ids)  # copper rocks from the Varrock mine

    def test_links_and_stamped_stairs(self):
        by_source = {}
        for e in self.gym.edges:
            by_source.setdefault(e.source_id, []).append(e)
            self.assertTrue(e.action in ("Climb-up", "Climb-down", "Pull"), e)
        down = by_source[14880]
        self.assertTrue(any(e.dest[0] >= 512 for e in down))  # town trapdoor leads into the dungeon
        self.assertTrue(any(e.dest[0] < 512 for e in by_source[17385]))  # cave ladder leads back up
        if EDGES.exists():
            stairs = by_source[11797]
            self.assertTrue(all(e.start[2] == 0 and e.dest[2] == 1 for e in stairs))

    def test_wilderness_levels_rise_north(self):
        bands = sorted((z.rect[1], z.value) for z in self.gym.zones if z.flags == AREA_WILDERNESS_LEVEL)
        self.assertEqual([v for _, v in bands], list(range(1, 11)))
        self.assertTrue(any(z.flags == AREA_MULTICOMBAT for z in self.gym.zones))

    def test_npcs_and_items(self):
        ids = {row[0] for row in self.gym.npc_spawns}
        for npc in (239, 2054, 2098, 1525, 2813):  # KBD, Chaos Elemental, hill giant, fishing spot, shop keeper
            self.assertIn(npc, ids)
        fishing = [row for row in self.gym.npc_spawns if row[0] in (1525, 1526, 1519)]
        self.assertTrue(all(row[5] == 0 for row in fishing))  # fishing spots do not wander
        self.assertTrue(self.gym.ground_items)

    def test_landmarks(self):
        names = [lm[0] for lm in self.gym.landmarks]
        self.assertEqual(len(names), len(set(names)))
        self.assertLessEqual(len(names), 24)  # RC_GYM_MAX_LANDMARKS
        by_name = {lm[0]: lm[1:] for lm in self.gym.landmarks}
        self.assertEqual(by_name["start"], self.gym.start)
        self.assertGreaterEqual(by_name["kbd_lair"][0], 512)  # in the dungeon area
        for name in ("bank", "furnace", "anvil", "mine", "trees", "net_fishing", "chaos_elemental"):
            self.assertIn(name, by_name)

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


@unittest.skipUnless(EDGES.exists() and DROPS.exists(), "source tables not installed")
class TablesTest(unittest.TestCase):
    def test_traversal_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "trav.bin"
            write_traversal(out, read_traversal(EDGES))
            self.assertEqual(out.read_bytes(), EDGES.read_bytes())

    def test_drops_round_trip_and_merge(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "drops.bin"
            write_drops(out, read_drops(DROPS))
            self.assertEqual(out.read_bytes(), DROPS.read_bytes())
            merged = merge_gym_drops(DROPS, out, [{"npcs": [3029], "always": [[526, 1, 1]],
                                                    "main": [[995, 1, 5, 4]]}])
            self.assertEqual(merged, 1)
            flags, always, main, _, _ = read_drops(out)[3029]
            self.assertEqual((flags, always, main), (0, [(526, 1, 1, 1)], [(995, 1, 5, 4)]))


if __name__ == "__main__":
    unittest.main()
