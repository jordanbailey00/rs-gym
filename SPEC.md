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

## v1: a small complete game for RL (implemented 2026-10-25, in testing)

### Goal

A small OSRS world that one agent, or several agents together, can learn in. The core loop is
gather → process → bank or sell → gear up → fight stronger monsters → risk death.

v1 should become the standard environment for trying new RL methods: complex enough that methods
show real differences, small and fast enough that interesting behaviour appears in about an hour of
training.

### Design targets

- Dense: every system is reachable within a few hundred ticks of the start.
- Graded: a fresh agent makes progress right away; a boss kill is a long-horizon goal.
- One environment step is one game tick (0.6 s of game time).
- Headless and deterministic: the same seed and actions give the same trajectory.
- Fast: no file I/O per step, the whole gym stays loaded, worlds step on any thread.
- Versioned: the environment reports its version (`RC_ENV_VERSION`).

### World

The 192×192 island (world tiles 128–319) and a 128×128 dungeon (world tiles 512–639 × 128–255).

| Area | Content |
|---|---|
| Town | Draynor bank, general store, Bob's tool shop, Lowe's combat shop, Lumbridge smithy (furnace, anvil), a Varrock house with a range and stairs, Lumbridge church altar, respawn at the start |
| Forest | 35 trees north-west of town, willows to the west, oaks to the east; maples and yews inside the wilderness (risk for the best logs) |
| Mine (south of town) | Varrock south-east mine (copper, tin, iron) plus coal and mithril rocks |
| Coast and river | net/bait spots on the south coast, lure/bait spots by the river, cage/harpoon spots on the east coast; the river has the Lumbridge bridge |
| Fields | chickens and cows to the south-west, goblins to the south-east, men in town, rats in the mine |
| Wilderness (north) | levels 1–10 in 5-tile bands; dark wizards, skeletons, giant spiders, ice warriors; Chaos Elemental in a multi-combat area |
| Dungeon | town trapdoor → Edgeville hill giant cave (hill and moss giants, lesser demons) → trapdoor → King Black Dragon lair (multi-combat; lever back out) |
| Ground items | respawning bronze axe, pickaxe, net, tinderbox and hammer near the start; bones; coins in the wilderness |

### Systems

| System | v1 behaviour |
|---|---|
| Players | up to 16 per world, each with their own stats, inventory, bank, interactions and command queue. NPCs target one player; loot goes to whoever dealt the most damage |
| Movement | walk/run with run energy; routes search a 128×128 window (OSRS-sized); stairs, ladders, trapdoors and the lair lever |
| Combat | melee, ranged, magic, attack styles, prayer, equipment, food and potions, NPC drops (unchanged engine). Gym drop tables for goblins, men, rats, dark wizards, skeletons and ice warriors |
| Woodcutting | trees (25 xp, depletes after a log), oak 15, willow 30, maple 45, yew 60 (1-in-8 depletion). OSRS success curves; axes bronze to dragon scale success. Stumps while depleted |
| Mining | copper, tin, iron 15, coal 30, mithril 55; deplete per ore; pickaxe sets swing speed (8 ticks bronze → 3 rune). Empty rocks while depleted |
| Fishing | small net (shrimps, anchovies), bait (sardine, herring; pike), lure (trout, salmon), cage (lobster), harpoon (tuna, swordfish). Bait and feathers consumed |
| Firemaking | tinderbox on logs lights a fire on the player's tile (100–200 ticks) and the player steps west |
| Cooking | "Cook" on a fire or range cooks every item of the first raw food (or use a raw item on it); burn chance falls with level |
| Smithing | furnace "Smelt": bronze, iron (50% at a furnace), steel, mithril bars; anvil "Smith" (hammer): 17 items per metal from recipes.bin. Make-all or make-one from a menu |
| Prayer | burying bones (4.5), big bones (15), dragon bones (72) |
| Banking | one bank per player |
| Shops | general store (buys anything, sells at 130%), tool shop (axes, pickaxes, fishing gear) and combat shop (runes, arrows, bows, staffs, leather and wizard gear, anti-dragon shield). Prices move 3% per step of stock away from base; stock is shared and restocks |
| Death | keep the 3 most valuable unstackable items (4 with Protect Item). The rest go to a gravestone only the owner sees (15 minutes) or, in the wilderness, to the floor for everyone (5 minutes). Respawn in town after 4 ticks with full stats |
| Zones | wilderness levels 1–10; multi-combat at the Chaos Elemental and in the KBD lair |
| Bosses | King Black Dragon (dragonfire: the anti-dragon shield cuts it from about 60 to 20 damage) and Chaos Elemental (teleports the player, unequips items) |

### RL interface (`rc-env`, see its README)

- **API.** `rc_env_create(config)`, `rc_env_reset(seed)`, `rc_env_set_action(agent, action)`,
  `rc_env_step()`, then per agent `rc_env_observe`, `rc_env_action_mask`, `rc_env_reward`,
  `rc_env_terminal`, `rc_env_episode_stats`. Game data loads once per process and is shared.
- **Observation:** 1,521 floats per agent: self state (vitals, status, wilderness, open interfaces),
  13 skills, equipment bonuses, 28 inventory slots and 11 equipment slots as item features, the 8
  nearest NPCs, 8 nearest usable objects, 6 ground items and 4 other players, walking distance and
  bearing to 22 named landmarks, an 11×11 walkability grid, the open shop's stock and prices, the
  first 16 bank slots, and the task.
- **Actions:** one discrete space of 453 actions with a validity mask: wait, walk 1/5/15 tiles in 8
  directions, travel toward a landmark, attack or use an option on a listed NPC or object, eat /
  drink / wield / bury, drop, light logs, unequip, prayers, attack style, run, pick up, make
  (furnace/anvil), shop buy 1 / buy 10 / sell, bank deposit / deposit all / withdraw, close, autocast.
- **Tasks** (rewards, starting kit, episode length): total, combat, woodcutting, mining, fishing,
  cooking, smithing, wealth, kbd, chaos_elemental. In boss tasks death ends the agent's episode.
- **Episodes:** the time limit ends every agent's episode together; deterministic for a seed and
  actions; any episode can be recorded and replayed exactly in rc-viewer.
- **PufferLib 5.0:** `rc-env/puffer` (`puf_*` entry points, one env = one world with `num_agents`
  players, same-step autoreset) and `tools/puffer_overlay.sh`.
- **Throughput** (one thread, this machine): about 60k–75k agent steps/s under random play and
  75k (1 agent) to 180k (16 agents) under a scripted skiller; 0.9M agent steps/s with 24 threads.

### Benchmark tasks

| Benchmark | Task preset | Score |
|---|---|---|
| Gather | woodcutting, mining, fishing | XP gained in the skill (3,000 ticks) |
| Supply chain | cooking, smithing | XP gained (6,000 ticks) |
| Wealth | wealth | net worth gained, at item value (6,000 ticks) |
| Combat | combat | combat XP, plus kills, minus deaths (6,000 ticks) |
| Boss | kbd, chaos_elemental | kills from a level-70 rune-gear start (2,000 ticks) |
| All-round | total | total levels gained (12,000 ticks) |
| Shared world | any, with 4–16 agents | the same scores, competing for nodes, monsters and stock |

### Not in v1

- General "use item on X" (only tinderbox on logs, raw food on fires/ranges, bars/ores on anvils/furnaces).
- Doors: agents cannot open them (the key buildings are reachable without doors).
- Pathing beyond 64 tiles in one click (as in OSRS); travel actions hop along landmark paths.
- Item-identity embeddings in observations; items are described by category, value and tier.

## v2 (planned)

- Trading between players.
- PvP in the wilderness.
- More monsters, NPCs and encounters to fight.
- Additional skills. Which skill systems to add is still to be decided.

## Not planned

Quests, farming and minigames.
