#ifndef RC_COMBAT_FORMULA_H
#define RC_COMBAT_FORMULA_H

#include "combat.h"
#include "npc.h"

#define RC_HIT_CHANCE_SCALE 10000

int rc_hit_chance_scaled(int att_roll, int def_roll);
int rc_player_effective_attack_level(const RcPlayer *player);
int rc_player_effective_strength_level(const RcPlayer *player);
int rc_player_effective_defence_level(const RcPlayer *player);
int rc_player_effective_magic_defence_level(const RcPlayer *player);
int rc_player_effective_ranged_attack_level(const RcPlayer *player);
int rc_player_effective_ranged_strength_level(const RcPlayer *player);
int rc_player_effective_magic_attack_level(const RcPlayer *player);
int rc_player_offensive_roll(const RcPlayer *player, RcCombatStyle style);
int rc_player_defensive_roll(const RcPlayer *player, RcCombatStyle style);
int rc_player_max_hit_melee(const RcPlayer *player);
int rc_player_max_hit_ranged(const RcPlayer *player);
int rc_player_max_hit_magic(const RcPlayer *player, int spell_max_hit);
int rc_npc_offensive_roll(const RcNpc *npc, RcCombatStyle style);
int rc_npc_defensive_roll(const RcNpc *npc, RcCombatStyle style);
RcCombatStyle rc_combat_npc_preferred_style(int attack_types);
RcCombatCalc rc_calc_npc_attack_style(const RcNpc *npc, const RcNpcDef *definition,
                                      const RcPlayer *defender,
                                      RcCombatStyle style);

#endif
