#include "events.h"
#include "types.h"
#include <string.h>

void rc_events_init(RcEventBus *bus) {
    if (!bus) return;
    memset(bus, 0, sizeof(*bus));
}

int rc_event_subscribe(RcWorld *world, int evt, RcEventFn fn, void *ctx) {
    if (!world || !fn || evt <= 0 || evt >= RC_EVT_MAX) return -1;
    RcEventSlot *slot = &world->events.slots[evt];
    for (int i = 0; i < slot->count; i++) {
        if (slot->handlers[i].fn == fn && slot->handlers[i].ctx == ctx)
            return -1;
    }
    if (slot->count >= RC_MAX_EVENT_HANDLERS) return -1;
    slot->handlers[slot->count].fn = fn;
    slot->handlers[slot->count].ctx = ctx;
    slot->count++;
    return 0;
}

int rc_event_unsubscribe(RcWorld *world, int evt, RcEventFn fn,
                         void *ctx) {
    if (!world || !fn || evt <= 0 || evt >= RC_EVT_MAX) return -1;
    RcEventSlot *slot = &world->events.slots[evt];
    for (int i = 0; i < slot->count; i++) {
        if (slot->handlers[i].fn == fn && slot->handlers[i].ctx == ctx) {
            // Slide remaining handlers down
            for (int j = i; j + 1 < slot->count; j++) {
                slot->handlers[j] = slot->handlers[j + 1];
            }
            slot->count--;
            return 0;
        }
    }
    return -1;
}

int rc_event_fire(RcWorld *world, int evt, const void *payload) {
    if (!world || evt <= 0 || evt >= RC_EVT_MAX) return -1;
    if (world->events.dispatching[evt]) return -1;
    world->events.dispatching[evt] = 1;
    RcEventSlot snapshot = world->events.slots[evt];
    for (int i = 0; i < snapshot.count; i++) {
        snapshot.handlers[i].fn(world, evt, payload,
                                snapshot.handlers[i].ctx);
    }
    world->events.dispatching[evt] = 0;
    return 0;
}
