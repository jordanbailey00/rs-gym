# TODO

## Data
- Make `data/` reproducible inside RS-gym. Non-spatial data was seeded by copying RuneC_v4's
  installed release; ship it as a versioned archive or port the definition exporters.

## Map
- Traversal rows for stairs and ladders in copied buildings (transform the real rows with the stamp),
  or drop those locs.
- Diagonal overlay shapes for a smoother coastline.
- Blend floors and heights where stamps meet generated ground.
- Area flags from the spec (wilderness strip, multi-combat zone).
- Fishing spots on the coast.

## Gameplay (rc-core / rc-content)
- Skilling loops: woodcutting, mining, fishing, cooking, smithing; generate gathering-node rows
  (with placement keys) for the gym's trees and rocks.
- Shops: buy and sell; bind the general store.
- Death: item loss rules and a gym respawn point.

## RL
- Keep-loaded active area so crossing a mapsquare does not re-activate the window.
- Reseed on reset; set the default player position to the gym start.
- Observation and action API, PufferLib binding, SPS benchmark on the gym.
- Thread-safe collision and object page caches.
- Several players per world (trading, PvP).

## Viewer
- Remove RuneC-only paths: the real-world smoke scenes and Varrock fixed-scene defaults.
