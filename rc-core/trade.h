#ifndef RC_TRADE_H
#define RC_TRADE_H

// Direct trades between two players. A trade request to a nearby player who has also requested you
// opens a trade. Offered items move out of the inventory into the trade, so they cannot be eaten,
// equipped, dropped or sold meanwhile. Every offer change bumps the trade's shared revision and
// clears both acceptances; when both players accept the same revision the exchange commits
// atomically, after rechecking both players' state, distance and inventory room. Moving, any other
// action, death, separation or a failed check cancels the trade and returns every offered item.
//
// Calls act for world->player and queue as soft player commands outside a tick.

#include "types.h"

#define RC_TRADE_RANGE 10            // tiles between partners
#define RC_TRADE_REQUEST_TICKS 100   // an unanswered request lapses

typedef enum {
    RC_TRADE_OK = 0,
    RC_TRADE_QUEUED,
    RC_TRADE_NO_PARTNER,     // not a living player, or yourself
    RC_TRADE_TOO_FAR,
    RC_TRADE_BUSY,           // the partner is trading with someone else
    RC_TRADE_NOT_OPEN,
    RC_TRADE_UNTRADEABLE,
    RC_TRADE_NO_ITEM,        // the slot is empty or the quantity is not there
    RC_TRADE_FULL,           // the trade has no free slot
    RC_TRADE_STALE,          // accepted an old revision
    RC_TRADE_NO_ROOM,        // a side lacks inventory room: cancelled
    RC_TRADE_CANCELLED,
    RC_TRADE_COMPLETED,
} RcTradeResult;

int  rc_trade_request(RcWorld *world, int partner_slot);
int  rc_trade_offer(RcWorld *world, int inv_slot, int quantity);
int  rc_trade_withdraw(RcWorld *world, int offer_slot, int quantity);
int  rc_trade_accept(RcWorld *world, uint32_t revision);
int  rc_trade_decline(RcWorld *world);

// Engine calls (never queued): cancel the acting player's trade and return both offers; per-tick
// checks for the acting player (a lapsed request, a partner gone, dead or too far).
void rc_trade_cancel(RcWorld *world, RcTradeResult reason);
void rc_trade_tick(RcWorld *world);

// Completed trades in this world (diagnostics).
uint32_t rc_trade_completed_count(const RcWorld *world);

#endif
