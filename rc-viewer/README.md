# rc-viewer

The OSRS-style raylib client. It renders the world `rc-core` simulates, with the fixed game frame
(inventory, equipment, prayer, spellbook, chat, minimap), click-to-walk and context menus, and
replays recorded rc-env episodes. It owns no gameplay rules.

Copied from RuneC_v4 at `338f2e6` and changed for the gym (see `CHANGELOG.md`).

## Run

From the repo root, after `tools/build_mode.py`:

```bash
./build/cmake/rc-viewer                          # play compact_osrs_v1: a fresh character
./build/cmake/rc-viewer --mode island            # play another mode
RUNEC_DEV_VALIDATION=1 ./build/cmake/rc-viewer   # dev: 99 stats, seeded bank, dev teleports
./build/cmake/rc-viewer --replay FILE            # watch a recorded episode
```

**Mouse:**
- Left-click walks or does the hovered action; right-click opens the options menu.
- Right-drag orbits the camera; the wheel zooms.
- Furnace/anvil menu (in the chatbox): left-click makes all, right-click makes one, Esc closes.
- Shop window: left-click buys or sells one (Shift: ten); right-click for Value, 5, 10 and 50.
- Trading: T asks the nearest player in range to trade (they must ask back); while the trade is
  open the chatbox shows both offers, a click on an inventory item offers it, and Accept / Decline
  act on the current revision (any change clears both acceptances; walking away cancels).

**Keys:**
- R toggles run, G toggles god mode, Space pauses.
- C shows collision, F3 shows the tile grid, F4 shows NPC tiles.
- PageUp and PageDown change the viewed plane; Home follows the player.
- In a replay, 1–9 follow agent 1–9. Clicks are ignored so the replay stays exact.

Roofs and upper floors are drawn while the player stands outdoors and hidden under a roof, as in
the client (`regions/roofs.bin` marks the roofed tiles).

Bone-animated ("maya") models play their sequences through `skeletal.h`: the mode's
`models/skeletal.bin` holds per-vertex bone weights and per-tick bone matrices, registered with
the animation cache so they time and blend like frame sequences.

**Environment:**
- `RUNEC_PLAYER_START_X/Y/PLANE` override the start; `RUNEC_CAMERA_YAW/PITCH/DIST` the camera.
- `RUNEC_VIEWER_ROOFS=0` never draws roofs.
- `RUNEC_VIEWER_SMOKE=1` checks startup without opening a window.
- Screenshots: `RC_VIEWER_SCREENSHOT=out.png` with `RC_VIEWER_EXIT_FRAMES=N` saves frame N and exits;
  `RC_VIEWER_SCREENSHOT_EVERY=K` also saves every Kth frame (`out_00120.png` ...);
  `RC_VIEWER_HIDE_UI=1` leaves only the scene, `RC_VIEWER_HIDDEN=1` keeps the window off screen and
  `RC_VIEWER_WIDTH/HEIGHT` size it.
- Dev fights: with `RUNEC_DEV_VALIDATION=1`, `RUNEC_DEV_TRANSPORT_DEST=boss` lands beside the mode's
  boss and starts its fight; `RUNEC_DEV_BOSS_HP_PCT=N` starts the boss at N% of its hitpoints.

## Data

The viewer streams `data/modes/<mode>/regions/{rx}_{ry}.p{plane}.*` for the mapsquares listed in
the mode's `regions/mapsquare.catalog`, plus models, sprites, fonts and animations under `data/`.
Locs the engine spawns (fires) are drawn from the mode's `models/spawn_objects.*`, skeletal
animation from its `models/skeletal.bin`, and its `defs/combat_visuals.tsv` rows (when it has them)
are read ahead of `data/defs/combat_visuals.tsv`.
