# TODO

## v1 milestone 1: world
- Dungeon of 2×2 mapsquares at an unused location, entered by a town ladder and a cave; copy in
  the King Black Dragon lair.
- Traversal rows: move the real rows for stairs and ladders along with each stamp; spec-authored
  links for the dungeon entrances.
- Spec support for zones (wilderness levels, multi-combat) and static ground items.
- New content:
  - church, houses, range, tool shop and combat supplies shop
  - maples, yews, coal and mithril
  - fishing spots, and a river with a bridge
  - the wilderness strip
  - giants, lesser demons and the other new monsters
- Smoother coastline (diagonal overlay shapes); blend floors and heights where stamps meet
  generated ground.

## v1 milestone 2: several players per world
- A player array in rc-core with per-player inventory, bank, stats and interactions; NPCs target a
  specific player.
- Keep the whole gym loaded instead of an active window around one player; make the default
  position the gym start.
- The viewer controls player 0 and draws the others.

## v1 milestone 3: skilling
- Woodcutting, mining, fishing, firemaking, cooking and smithing loops, using the recipe and
  skill-drop data.
- Generate gathering-node rows (with placement keys) for the gym's trees, rocks and fishing spots.

## v1 milestone 4: shops and death
- Buying and selling with stock-based prices; a shop interface in the viewer; bind the gym's
  shopkeepers.
- Death: keep 3 items, gravestones, wilderness drops, respawn in town.

## v1 milestone 5: bosses
- King Black Dragon and Chaos Elemental: gym spawns, with their fight areas moved to the gym.

## v1 milestone 6: RL interface
- C environment API, observations, masked actions, task presets and benchmark tasks.
- Per-episode reseeding, a determinism test, PufferLib binding, episode recording and viewer replay.
- Thread-safe collision and object caches; an SPS benchmark.

## Other
- Make `data/` reproducible inside RS-gym. Non-spatial data was seeded by copying RuneC_v4's
  installed release; ship it as a versioned archive or port the definition exporters.
- Viewer: remove RuneC-only paths (the real-world smoke scenes and the Varrock fixed-scene defaults).
