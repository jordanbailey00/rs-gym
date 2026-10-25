#ifndef RC_CONTENT_AMMUNITION_H
#define RC_CONTENT_AMMUNITION_H

#include "types.h"
#include "combat_formula.h"

int rc_content_ranged_resource_slot(const RcPlayer *player, const char **failure);
void rc_content_ranged_calc(const RcWorld *world, const RcNpc *target, RcCombatCalc *calc);
int rc_content_consume_ammunition(RcWorld *world, int slot, int count,
    int x, int y, int plane, int delay, const char **failure);

#endif
