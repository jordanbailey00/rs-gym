# rc-content

OSRS-specific content on top of `rc-core`:

- `skilling.c`: the gym's tools, rates and products for woodcutting, mining, fishing, firemaking,
  cooking and smithing, and the object/NPC/item handlers that start them (Chop down, Mine, fishing
  options, tinderbox on logs, Cook, Smelt, Smith)
- `gym.c`: the town's shops (stock, prices, Trade handlers), burying bones, and the death rules
- `compact.c`: a mode's content from its rules (`rc-core/mode.h`): skilling rules and production
  stations, fixed-price shops, the tanner and Historian Aldo, burying, closed drop tables, bone
  weapons and staff charges, death recovery and loot timing, and `rc_content_compact_new_character`. When a mode is active, `skilling.c`
  and `gym.c` stand down.
- `encounters/`: boss scripts (real-world bosses; the gym's King Black Dragon and Chaos Elemental
  run from the encounter data without scripts). `scurrius.c` is a mode's shared boss: phases,
  feeding, a jump that drops bricks, a screech that summons rats (twelve waves at most), group
  scaling, kill credit, loot rotation and reset. It names its attacks and actions for the viewer
  (the `npc_attack_key` and `npc_stand_key` hooks, `rc_combat_record_encounter_action`)
- `combat/`: spells and runes, ammunition, dragonfire, NPC spell-target attributes
- consumables, and the bank storage policy (which objects and NPCs open the bank)

`rc_content_register_all` in `content.c` installs every module into a world; `rc_world_reset`
re-runs it.

Copied from RuneC_v4 at `338f2e6`; `skilling.c` and `gym.c` are new. `combat/npc_spell_targets.inc`
moved here from RuneC's `content/` tree.
