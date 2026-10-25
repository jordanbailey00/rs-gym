#include "ui.h"
#include "../rc-core/combat_formula.h"
#include "../rc-core/spells.h"
#include "../rc-core/types.h"
#include <string.h>

static int spell_id(const char *name, int book) {
    // Cache display labels differ from three installed definition names.
    if (!strcmp(name, "Ape Atoll Teleport"))
        name = book == RC_SPELL_BOOK_STANDARD ? "Ape Atoll Teleport (standard)"
                                             : "Ape Atoll Teleport (Arceuus)";
    else if (!strcmp(name, "Hunter Kit")) name = "Hunter Kit (spell)";
    int id = rc_spell_find(name);
    const RcSpellDef *spell = rc_spell_def_get(id);
    return spell && spell->book == book ? id : -1;
}

int runec_ui_spell_runtime_id(const RuneCUiState *ui, int slot) {
    return ui && slot >= 0 && slot < ui->spell_count ? ui->spell_runtime_ids[slot] : -1;
}

int runec_ui_sync_spellbook(RuneCUiState *ui, const RcWorld *world) {
    if (!ui || !world) return 0;
    const RcPlayer *p = &world->player;
    if (ui->current_spellbook != p->current_spellbook) {
        if (!runec_ui_set_spellbook(ui, p->current_spellbook)) return 0;
    }
    if (ui->spell_sync_tick == UINT64_MAX) {
        for (int i = 0; i < ui->spell_count; i++)
            ui->spell_runtime_ids[i] = spell_id(ui->spells[i]->name, p->current_spellbook);
    }
    if (ui->spell_sync_tick == world->tick) return 1;
    ui->spell_sync_tick = world->tick;
    ui->autocast_available = rc_player_weapon_can_autocast(p);
    ui->autocast_slot = -1;
    ui->defensive_autocast = p->defensive_autocast;
    for (int i = 0; i < ui->spell_count; i++) {
        ui->spell_available[i] = rc_spell_available(world, ui->spell_runtime_ids[i], 0) == RC_SPELL_OK;
        const RcSpellDef *spell = rc_spell_def_get(ui->spell_runtime_ids[i]);
        ui->spell_can_autocast[i] = ui->autocast_available && spell
            && spell->type == RC_SPELL_TYPE_COMBAT && spell->max_hit > 0
            && (!world->combat_hooks.can_autocast_spell
                || world->combat_hooks.can_autocast_spell(p, spell));
        if (p->autocast_spell >= 0 && ui->spell_runtime_ids[i] == p->autocast_spell)
            ui->autocast_slot = i;
    }
    if (!ui->autocast_available) ui->autocast_picker = 0;
    return 1;
}
