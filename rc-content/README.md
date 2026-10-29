# rc-content

OSRS-specific content on top of `rc-core`:

- `skilling.c`: the gym's tools, rates and products for woodcutting, mining, fishing, firemaking,
  cooking and smithing, and the object/NPC/item handlers that start them (Chop down, Mine, fishing
  options, tinderbox on logs, Cook, Smelt, Smith)
- `gym.c`: the town's shops (stock, prices, Trade handlers), burying bones, and the death rules
- `encounters/`: boss scripts (real-world bosses; the gym's King Black Dragon and Chaos Elemental
  run from the encounter data without scripts)
- `combat/`: spells and runes, ammunition, dragonfire, NPC spell-target attributes
- consumables, and the bank storage policy (which objects and NPCs open the bank)

`rc_content_register_all` in `content.c` installs every module into a world; `rc_world_reset`
re-runs it.

Copied from RuneC_v4 at `338f2e6`; `skilling.c` and `gym.c` are new. `combat/npc_spell_targets.inc`
moved here from RuneC's `content/` tree.
