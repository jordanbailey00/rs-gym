# rc-content

OSRS-specific content on top of `rc-core`:

- `encounters/`: boss scripts
- `combat/`: spells and runes, ammunition, dragonfire, NPC spell-target attributes
- consumables
- the bank storage policy (which objects and NPCs open the bank)

`rc_content_register_all` in `content.c` installs every module into a world.

Copied from RuneC_v4 at `338f2e6`. `combat/npc_spell_targets.inc` moved here from RuneC's `content/`
tree. The encounter scripts target real-world bosses; none are placed in the gym yet.
