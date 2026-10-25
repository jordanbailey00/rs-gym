#ifndef RC_PLAYER_COMMAND_H
#define RC_PLAYER_COMMAND_H

#include "types.h"

int rc_player_command_should_queue(const RcWorld *world);
int rc_player_command_submit(RcWorld *world, RcPlayerCommandKind kind,
                             RcActionCategory category,
                             const int args[8], uint64_t key);
void rc_player_command_process(RcWorld *world);
void rc_player_action_refresh(RcWorld *world);
void rc_player_cancel_action(RcWorld *world,
                             RcPlayerActionCancelReason reason);
void rc_player_replace_action_with_movement(
    RcWorld *world, RcPlayerActionCancelReason reason);

#endif
