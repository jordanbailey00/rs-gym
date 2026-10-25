#ifndef RC_TRAVERSAL_H
#define RC_TRAVERSAL_H

#include "types.h"

#define RC_TRAVERSAL_MAX_SOURCE_ID 65536

enum {
    RC_TRAVERSAL_OBJECT = 1,
    RC_TRAVERSAL_ITEM = 2,
    RC_TRAVERSAL_SPELL = 3,
    RC_TRAVERSAL_KIND_COUNT = 4,
};

typedef enum {
    RC_TRAVERSAL_SOURCE_NONE = 0,
    RC_TRAVERSAL_SOURCE_PLAYER_TILE,
    RC_TRAVERSAL_SOURCE_INSTANCE_TEMPLATE,
} RcTraversalSourceSemantics;

typedef struct {
    uint8_t kind, option, source_semantics;
    uint8_t start_plane, dest_plane;
    uint32_t source_id, flags;
    uint16_t start_x, start_y, dest_x, dest_y;
    char action[32];
    char target[48];
} RcTraversalEdge;

typedef struct {
    uint32_t first, count;
} RcTraversalRange;

typedef struct {
    RcTraversalEdge *edges;
    int edge_count;
    RcTraversalRange index[RC_TRAVERSAL_KIND_COUNT]
                           [RC_TRAVERSAL_MAX_SOURCE_ID];
} RcTraversalData;

typedef struct {
    int x, y, plane;
} RcTraversalDestination;

// Relative offsets use the admitted player source/approach tile, not the
// clicked object's placement anchor.
typedef struct {
    RcTraversalDestinationPolicy policy;
    RcTraversalDestination destination;
    int delta_x, delta_y, delta_plane;
    int area_min_x, area_min_y, area_max_x, area_max_y, area_plane;
    const RcTraversalDestination *choices;
    int choice_count;
    int choice_index;
    bool context_available;
    bool instance_mapping_available;
} RcTraversalDestinationSpec;

typedef struct {
    RcTraversalPresentation presentation;
    int takeoff_ticks;
    int transit_ticks;
    int landing_ticks;
    bool has_facing;
    int facing_x;
    int facing_y;
} RcTraversalMotionSpec;

extern RcTraversalEdge *g_rc_traversal_edges;
extern int g_rc_traversal_edge_count;

int rc_load_traversal_edges(const char *path);
void rc_traversal_data_init(RcTraversalData *data);
void rc_traversal_data_free(RcTraversalData *data);
int rc_load_traversal_edges_into(const char *path, RcTraversalData *data);
int rc_traversal_mirror_to_globals(const RcTraversalData *data);
void rc_traversal_use_data(const RcTraversalData *data);
void rc_traversal_reset_data_if_active(const RcTraversalData *data);
const RcTraversalEdge *rc_traversal_edges_for(int kind, int source_id,
                                              int *count);
const RcTraversalEdge *rc_traversal_edges_all(int *count);
const RcTraversalEdge *rc_traversal_get(int idx);
int rc_traversal_index_of(const RcTraversalEdge *edge);
const RcTraversalEdge *rc_traversal_find(int kind, int source_id,
                                         int x, int y, int plane, int option);
const RcTraversalEdge *rc_traversal_find_target(int kind, const char *target);
int rc_traversal_resolve_destination(
    RcWorld *world, const RcTraversalDestinationSpec *spec,
    const RcTraversalDestination *source, RcTraversalDestination *out);
int rc_traversal_plan_object(
    RcWorld *world, const RcTraversalEdge *edge, uint64_t source_key,
    uint64_t interaction_generation,
    const RcTraversalDestinationSpec *destination,
    const RcTraversalMotionSpec *motion);
int rc_traversal_start(RcWorld *world, uint64_t interaction_generation);
void rc_traversal_tick(RcWorld *world);
void rc_traversal_cancel(RcWorld *world,
                         RcPlayerActionCancelReason reason);
void rc_traversal_clear(RcPlayer *player);
const RcTraversalState *rc_traversal_state(const RcPlayer *player);
const RcTraversalEvent *rc_traversal_last_event(const RcPlayer *player);
const RcTraversalOutcome *rc_traversal_last_outcome(const RcPlayer *player);

#endif
