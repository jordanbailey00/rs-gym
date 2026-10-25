# RS-gym spec

## v0: island map (done, branch `v0`)

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

## v1: a small complete game for RL (next)

### Goal

A small OSRS world that one agent, or several agents together, can learn in. The core loop is
gather → process → bank or sell → gear up → fight stronger monsters → risk death.

v1 should become the standard environment for trying new RL methods. It must be complex enough that
methods show real differences, and small and fast enough that interesting behaviour appears in an
experiment of about an hour, not days.

### Design targets

- Dense: every system is reachable within a few hundred ticks of the start.
- Graded: a fresh agent can make progress right away; a boss kill is a long-horizon goal that is
  still achievable.
- One environment step is one game tick (0.6 s of game time).
- Headless and deterministic: the same seed and actions give the same trajectory.
- Fast: no file I/O or allocation per step, and the whole gym stays loaded. Initial throughput
  target: at least 100k agent steps per second per CPU core, to be confirmed by the first benchmark.
- Versioned: the environment reports the spec version and a hash of its data, so results stay
  comparable over time.

### World

The same 192×192 surface, much denser, plus an underground dungeon of about 128×128 tiles.

| Area | Content |
|---|---|
| Town | bank, general store, tool shop, combat supplies shop, furnace and anvils, range, church altar, houses with working stairs, respawn point |
| Forest (north) | normal trees, oaks, willows, maples, yews |
| Mine (south-east) | copper, tin, iron, coal, mithril |
| Coast and river | net, bait, fly and lobster-pot fishing spots; a bridge over the river |
| Fields (west) | chickens, cows, goblins, men |
| Woods (north-east) | dark wizards and other low-to-mid monsters |
| Wilderness strip (north coast) | wilderness levels 1–10, stronger monsters with better drops, Chaos Elemental |
| Dungeon (town ladder or a cave) | hill giants, moss giants, lesser demons, and the King Black Dragon lair copied from the real map |
| Ground items | respawning spawns such as bones, coins and basic tools |

### Systems

| System | v1 behaviour |
|---|---|
| Movement | walking, running and run energy; stairs and ladders in copied buildings; dungeon entrances |
| Combat | melee, ranged and magic; attack styles; prayer; equipment; food and potions; NPC drops and respawns (all existing) |
| Skilling | woodcutting, mining, fishing, firemaking, cooking and smithing (smelting and anvil). Success depends on level and tool. Actions give XP and items. Nodes deplete and respawn |
| Banking | one bank per player (existing) |
| Shops | buying and selling, with OSRS-style prices that move with stock; stock is shared by all players. The combat shop sells runes, ammunition and an anti-dragon shield, since there are no quests |
| Death | keep the 3 most valuable items and respawn in town. Outside the wilderness the rest go to a gravestone that only the owner can loot, for a limited time. In the wilderness the rest drop where any player can take them |
| Zones | wilderness levels; multi-combat in the two boss areas |
| Bosses | King Black Dragon (dungeon; its dragonfire calls for an anti-dragon shield) and Chaos Elemental (wilderness). The copied engine already supports both |
| Players | up to 16 players per world, sharing monsters, resource nodes, ground items, shops and bosses. Each player has their own inventory, bank and stats. No trading or PvP (v2) |

Item chains:
- logs → fires → cooked food
- fish → food
- ores → bars → bronze, iron, steel and mithril weapons and armour
- drops and sold goods → coins → gear from shops

### RL interface

- **C API.** Create an environment from a config, reset it with a seed, and step it with one
  action per agent. Observations, action masks, rewards and episode ends go into fixed-size buffers.
  Many environments run per process.
- **Observations per agent:**
  - an egocentric tile grid showing walkability, terrain, objects, NPCs, ground items and other players
  - a self vector: levels, XP, HP, prayer, run energy, combat style
  - inventory and equipment
  - lists of nearby NPCs, objects and items, with relative positions and available options
  - the contents of an open bank or shop
- **Actions per agent** (discrete, with masks):
  - wait
  - walk to a tile in the local window
  - interact with a listed entity using one of its options
  - inventory slot actions: eat, drink, equip, drop, use on something
  - bank and shop slot actions
  - prayer toggles, attack style, run toggle
- **Config.** Agent count, episode length, starting stats, items and bank, spawn points, task
  preset and seed. The seed also drives drops and NPC movement.
- **Rewards.** Rewards come from task presets (below), which can be combined.
- **Tooling.** A PufferLib binding, like Fight Caves, and recorded episodes that replay in rc-viewer.

### Benchmark tasks

| Task | Agents | Score |
|---|---|---|
| Gather | 1 | XP gained in woodcutting, mining and fishing within a time limit |
| Supply chain | 1 | value of processed goods (cooked food, bars, smithed items) made within a time limit |
| Wealth | 1 | net worth (bank plus inventory, at shop prices) at the end |
| Combat ladder | 1 | total levels of monsters killed, minus a cost per death |
| Boss | 1–4 | King Black Dragon or Chaos Elemental kills from a mid-game start |
| Shared world | 4–16 | any task above, with agents competing for the same nodes, monsters and shop stock |

### Milestones

Each milestone ends with headless tests and a viewer check.

1. World: the dungeon, working stairs and ladders, zones, new buildings, monsters and ground items.
   Mostly compiler work.
2. Several players per world, with the whole gym kept loaded. This comes before the new systems so
   they are written for many players from the start.
3. Skilling.
4. Shops and death.
5. Bosses.
6. The RL interface, benchmark tasks and an SPS benchmark.

## v2 (planned)

- Trading between players.
- PvP in the wilderness.
- More monsters, NPCs and encounters to fight.
- Additional skills. Which skill systems to add is still to be decided.

## Not planned

Quests, farming and minigames.
