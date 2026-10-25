# rc-viewer

The OSRS-style raylib client. It renders the world `rc-core` simulates, with the fixed game frame
(inventory, equipment, prayer, spellbook, chat, minimap), click-to-walk and context menus. It owns
no gameplay rules.

Copied from RuneC_v4 at `338f2e6` and changed to start in the gym (see `CHANGELOG.md`).

## Run

From the repo root, after `tools/build_gym.py`: `./build/cmake/rc-viewer`

**Mouse:**
- Left-click walks or does the hovered action.
- Right-click opens the options menu.
- Right-drag orbits the camera; the wheel zooms.

**Keys:**
- R toggles run, G toggles god mode, Space pauses.
- C shows collision, F3 shows the tile grid, F4 shows NPC tiles.
- PageUp and PageDown change the viewed plane; Home follows the player.

**Dev panel (Clan Chat tab):** plane buttons and a Start teleport.
- The bank is seeded with test gear; turn this off with `RUNEC_DEV_VALIDATION=0`.
- A combat dummy spawns beside the start; turn it off with `RUNEC_DEV_BANK_DUMMY=0`.

**Environment:**
- `RUNEC_PLAYER_START_X/Y/PLANE` override the start.
- `RUNEC_VIEWER_SMOKE=1` checks startup without opening a window.

## Data

The viewer streams `data/regions/{rx}_{ry}.p{plane}.*` for the mapsquares listed in
`data/regions/mapsquare.catalog`, plus models, sprites, fonts and animations under `data/`.
