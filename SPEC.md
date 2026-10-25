# RS-gym spec

## v0: island map (current)

A small island built from OSRS cache content, simulated by the copied engine and walkable in the viewer.

### Map

- 192×192 playable tiles (3×3 mapsquares) at world tiles 128–319, in cache coordinates the real
  world never uses (it starts at mapsquare 15, 19).
- A one-mapsquare ocean ring (world tiles 64–383) keeps the view and the engine's active window
  inside generated data.
- Generated island: wobbling coastline (radius 74 tiles, about ±15%), sand beach, two grass shades,
  rolling heights from the client's height noise, shallow-to-deep sea. All sea tiles are blocked.
- Real buildings copied with all four planes:
  - Draynor bank (bank booths, cooking fire)
  - Lumbridge general store, turned 90°
  - Lumbridge smithy (furnace, anvil), turned 270°
  - Varrock south-east mine (copper, tin, iron)
- Stone roads, 45 trees, 10 oaks, 10 willows.
- 26 NPCs: chickens, cows, goblins, rats, dark wizards, men.
- Player start: world tile 222, 222 on the town road.

### Systems (RuneC behaviour, unchanged)

| System | v0 |
|---|---|
| Movement, pathfinding, run energy | yes |
| Melee, ranged, magic combat; attack styles; prayer | yes |
| Inventory, equipment, consumables, NPC loot drops | yes |
| Banking at the Draynor booths | yes |
| Skilling | no: rocks and trees deplete only; no XP or products |
| Shops | no buy/sell |
| Death | no item loss or respawn point |
| Stairs and ladders in copied buildings | no (need traversal rows) |
| Wilderness or multi-combat zones | no (empty area-flag table) |
| Trading, PvP | no: one player per world |

### Pipeline

`maps/*.toml` → `tools/gymmap` → overlay cache (map index replaced) → copied exporters →
`data/regions`, `data/spawns`. Output is deterministic for a given spec and seed.

### Viewer

Starts at the gym start. The dev panel (Clan Chat tab) has plane buttons and a Start teleport. The
bank is seeded with test gear and a combat dummy stands beside the start.

### Verified

- Every real b237 mapsquare round-trips through the compiler's encoder.
- `tests/test_gymmap.py`: format, rotation, compile and overlay-cache tests.
- `tests/test_gym_world.c`: the world loads, the player starts on the island, the sea stops
  movement, NPCs spawn, the Draynor booth opens the bank.
- Viewer smoke run and a rendered frame.

## Later versions

- v1 gameplay: skilling loops, shops, death and respawn, fishing spots, area flags (a wilderness strip).
- v2 RL: keep-loaded active area, per-episode reseeding, observation and action API, PufferLib
  binding, thread-safe caches.
- v3 multi-agent: several players per world for trading and PvP.
