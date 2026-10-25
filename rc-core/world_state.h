#ifndef RC_WORLD_STATE_H
#define RC_WORLD_STATE_H

#include "types.h"

int rc_world_state_save_npcs(RcWorld *world);
int rc_world_state_restore_npcs(RcWorld *world);
void rc_world_state_clear_player_combat(RcWorld *world);

int rc_world_state_save_ground_items(RcWorld *world,
                                     int min_x, int min_y,
                                     int max_x, int max_y,
                                     int min_plane, int max_plane);
int rc_world_state_restore_ground_items(RcWorld *world,
                                        int min_x, int min_y,
                                        int max_x, int max_y,
                                        int min_plane, int max_plane);

int rc_world_state_clone_dormant(RcWorld *dst, const RcWorld *src);
void rc_world_state_discard_dormant(RcWorld *world);

void rc_world_state_destroy(RcWorld *world);

#endif
