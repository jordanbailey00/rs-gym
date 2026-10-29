# rc-viewer

The OSRS-style raylib client. It renders the world `rc-core` simulates, with the fixed game frame
(inventory, equipment, prayer, spellbook, chat, minimap), click-to-walk and context menus, and
replays recorded rc-env episodes. It owns no gameplay rules.

Copied from RuneC_v4 at `338f2e6` and changed for the gym (see `CHANGELOG.md`).

## Run

From the repo root, after `tools/build_gym.py`:

```bash
./build/cmake/rc-viewer                          # play: a fresh player with the starter kit
RUNEC_DEV_VALIDATION=1 ./build/cmake/rc-viewer   # dev: 99 stats, seeded bank, dev teleports
./build/cmake/rc-viewer --replay FILE            # watch a recorded episode
```

**Mouse:**
- Left-click walks or does the hovered action; right-click opens the options menu.
- Right-drag orbits the camera; the wheel zooms.
- Furnace/anvil menu (in the chatbox): left-click makes all, right-click makes one, Esc closes.
- Shop window: left-click buys or sells one (Shift: ten); right-click for Value, 5, 10 and 50.

**Keys:**
- R toggles run, G toggles god mode, Space pauses.
- C shows collision, F3 shows the tile grid, F4 shows NPC tiles.
- PageUp and PageDown change the viewed plane; Home follows the player.
- In a replay, 1–9 follow agent 1–9. Clicks are ignored so the replay stays exact.

**Environment:**
- `RUNEC_PLAYER_START_X/Y/PLANE` override the start.
- `RUNEC_VIEWER_SMOKE=1` checks startup without opening a window.

## Data

The viewer streams `data/regions/{rx}_{ry}.p{plane}.*` for the mapsquares listed in
`data/regions/mapsquare.catalog`, plus models, sprites, fonts and animations under `data/`.
Fires players light are drawn from `data/models/spawn_objects.*`.
