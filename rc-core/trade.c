#include "trade.h"

#include "items.h"
#include "player_command.h"
#include "players.h"
#include "storage.h"

#include <stdlib.h>
#include <string.h>

static RcPlayer *partner_of(RcWorld *w, const RcPlayer *p) {
    return p->trade.partner >= 0 ? rc_world_player_at(w, p->trade.partner) : NULL;
}

static int near(const RcPlayer *a, const RcPlayer *b) {
    return a->plane == b->plane && abs(a->x - b->x) <= RC_TRADE_RANGE
        && abs(a->y - b->y) <= RC_TRADE_RANGE;
}

static int alive(const RcPlayer *p) {
    return p && p->active && !p->is_dead && p->current_hp > 0;
}

static int result(RcPlayer *p, RcTradeResult r) {
    p->trade.last_result = (uint8_t)r;
    return r == RC_TRADE_OK || r == RC_TRADE_COMPLETED;
}

static int queue(RcWorld *w, RcPlayerCommandKind kind, int a0, int a1, int a2) {
    int args[8] = {a0, a1, a2, 0, 0, 0, 0, 0};
    int ok = rc_player_command_submit(w, kind, RC_ACTION_CATEGORY_SOFT, args, 0);
    if (ok) w->player->trade.last_result = RC_TRADE_QUEUED;
    return ok;
}

// Puts `p`'s offered items back into its inventory (or, with no room, its bank: never lost).
static void return_offer(RcWorld *w, RcPlayer *p) {
    RcPlayer *previous = w->player;
    w->player = p;
    for (int i = 0; i < p->trade.offer_count; i++) {
        RcInvSlot *s = &p->trade.offer[i];
        if (s->item_id < 0 || s->quantity <= 0) continue;
        RcItemTransaction tx;
        RcItemActionResult r = rc_item_tx_begin(&tx, w);
        if (r.code == RC_ITEM_RESULT_OK) r = rc_item_tx_add(&tx, s->item_id, s->quantity, s->state_id);
        if (r.code != RC_ITEM_RESULT_OK || rc_item_tx_commit(&tx).code != RC_ITEM_RESULT_OK)
            (void)rc_bank_add_item_state(w, s->item_id, s->quantity, s->state_id);
    }
    w->player = previous;
    p->trade.offer_count = 0;
}

static void close_trade(RcPlayer *p, RcTradeResult reason) {
    p->trade.stage = RC_TRADE_STAGE_NONE;
    p->trade.partner = -1;
    p->trade.accepted_revision = 0;
    p->trade.offer_count = 0;
    p->trade.last_result = (uint8_t)reason;
}

// Ends a trade for both sides, returning both offers.
static void end_trade(RcWorld *w, RcPlayer *p, RcTradeResult reason) {
    RcPlayer *q = partner_of(w, p);
    if (q && q->trade.partner == p->index) {
        return_offer(w, q);
        close_trade(q, reason);
    }
    return_offer(w, p);
    close_trade(p, reason);
}

void rc_trade_cancel(RcWorld *world, RcTradeResult reason) {
    RcPlayer *p = world ? world->player : NULL;
    if (!p || p->trade.stage == RC_TRADE_STAGE_NONE) return;
    end_trade(world, p, reason);
}

static void bump(RcWorld *w, RcPlayer *p) {
    RcPlayer *q = partner_of(w, p);
    uint32_t revision = p->trade.revision + 1;
    p->trade.revision = revision;
    p->trade.accepted_revision = 0;
    if (q) {
        q->trade.revision = revision;
        q->trade.accepted_revision = 0;
    }
}

static int open_trade(RcWorld *w, RcPlayer *p) {
    return p->trade.stage == RC_TRADE_STAGE_OPEN && partner_of(w, p) != NULL;
}

int rc_trade_request(RcWorld *w, int partner_slot) {
    if (rc_player_command_should_queue(w))
        return queue(w, RC_PLAYER_COMMAND_TRADE_REQUEST, partner_slot, 0, 0);
    RcPlayer *p = w ? w->player : NULL;
    if (!p) return 0;
    RcPlayer *q = rc_world_player_at(w, partner_slot);
    if (!q || q == p || !alive(q) || !alive(p)) return result(p, RC_TRADE_NO_PARTNER);
    if (!near(p, q)) return result(p, RC_TRADE_TOO_FAR);
    if (p->trade.stage == RC_TRADE_STAGE_OPEN) return result(p, RC_TRADE_BUSY);
    if (q->trade.stage == RC_TRADE_STAGE_OPEN) return result(p, RC_TRADE_BUSY);
    if (q->trade.stage == RC_TRADE_STAGE_REQUESTED && q->trade.partner == p->index) {
        // Mutual requests open the trade.
        uint32_t session = ++w->trade_sessions;
        for (int i = 0; i < 2; i++) {
            RcPlayer *a = i ? q : p;
            a->trade.stage = RC_TRADE_STAGE_OPEN;
            a->trade.partner = (int8_t)(i ? p->index : q->index);
            a->trade.session = session;
            a->trade.revision = 1;
            a->trade.accepted_revision = 0;
            a->trade.offer_count = 0;
        }
        return result(p, RC_TRADE_OK);
    }
    p->trade.stage = RC_TRADE_STAGE_REQUESTED;
    p->trade.partner = (int8_t)q->index;
    p->trade.requested_tick = w->tick;
    return result(p, RC_TRADE_OK);
}

int rc_trade_offer(RcWorld *w, int slot, int quantity) {
    if (rc_player_command_should_queue(w))
        return queue(w, RC_PLAYER_COMMAND_TRADE_OFFER, slot, quantity, 0);
    RcPlayer *p = w ? w->player : NULL;
    if (!p || !open_trade(w, p)) return p ? result(p, RC_TRADE_NOT_OPEN) : 0;
    if (slot < 0 || slot >= RC_INVENTORY_SIZE || quantity <= 0 || p->inventory[slot].item_id < 0)
        return result(p, RC_TRADE_NO_ITEM);
    RcInvSlot item = p->inventory[slot];
    const RcItemDef *def = rc_item_def_get(item.item_id);
    if (!def || !def->tradeable) return result(p, RC_TRADE_UNTRADEABLE);
    // Unstackable items offer one slot at a time, stacks any part of themselves.
    int available = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        if (p->inventory[i].item_id == item.item_id && p->inventory[i].state_id == item.state_id)
            available += p->inventory[i].quantity;
    if (quantity > available) quantity = available;
    int into = -1;
    for (int i = 0; i < p->trade.offer_count; i++)
        if (p->trade.offer[i].item_id == item.item_id && p->trade.offer[i].state_id == item.state_id
                && def->stackable)
            into = i;
    if (into < 0 && p->trade.offer_count >= RC_TRADE_SLOTS) return result(p, RC_TRADE_FULL);
    RcItemTransaction tx;
    RcItemActionResult r = rc_item_tx_begin(&tx, w);
    if (def->stackable) {
        if (r.code == RC_ITEM_RESULT_OK) r = rc_item_tx_remove_item(&tx, item.item_id, quantity);
    } else {
        int moved = 0;
        for (int i = 0; i < RC_INVENTORY_SIZE && moved < quantity && r.code == RC_ITEM_RESULT_OK; i++) {
            if (p->inventory[i].item_id != item.item_id || p->inventory[i].state_id != item.state_id)
                continue;
            if (p->trade.offer_count + moved >= RC_TRADE_SLOTS) break;
            r = rc_item_tx_remove_slot(&tx, i, 1, p->inventory[i].generation);
            moved++;
        }
        quantity = moved;
    }
    if (r.code != RC_ITEM_RESULT_OK || quantity <= 0
            || rc_item_tx_commit(&tx).code != RC_ITEM_RESULT_OK)
        return result(p, RC_TRADE_NO_ITEM);
    if (def->stackable) {
        if (into < 0) {
            into = p->trade.offer_count++;
            p->trade.offer[into] = (RcInvSlot){item.item_id, 0, item.state_id, 0};
        }
        p->trade.offer[into].quantity += quantity;
    } else {
        for (int i = 0; i < quantity; i++)
            p->trade.offer[p->trade.offer_count++] = (RcInvSlot){item.item_id, 1, item.state_id, 0};
    }
    bump(w, p);
    return result(p, RC_TRADE_OK);
}

int rc_trade_withdraw(RcWorld *w, int offer_slot, int quantity) {
    if (rc_player_command_should_queue(w))
        return queue(w, RC_PLAYER_COMMAND_TRADE_WITHDRAW, offer_slot, quantity, 0);
    RcPlayer *p = w ? w->player : NULL;
    if (!p || !open_trade(w, p)) return p ? result(p, RC_TRADE_NOT_OPEN) : 0;
    if (offer_slot < 0 || offer_slot >= p->trade.offer_count || quantity <= 0)
        return result(p, RC_TRADE_NO_ITEM);
    RcInvSlot *s = &p->trade.offer[offer_slot];
    if (quantity > s->quantity) quantity = s->quantity;
    RcItemTransaction tx;
    RcItemActionResult r = rc_item_tx_begin(&tx, w);
    if (r.code == RC_ITEM_RESULT_OK) r = rc_item_tx_add(&tx, s->item_id, quantity, s->state_id);
    if (r.code != RC_ITEM_RESULT_OK || rc_item_tx_commit(&tx).code != RC_ITEM_RESULT_OK)
        return result(p, RC_TRADE_NO_ROOM);
    s->quantity -= quantity;
    if (s->quantity <= 0) {
        memmove(s, s + 1, (size_t)(p->trade.offer_count - offer_slot - 1) * sizeof(*s));
        p->trade.offer_count--;
    }
    bump(w, p);
    return result(p, RC_TRADE_OK);
}

// Stages `items` into `to`'s inventory as one transaction (committed only if the other side's
// transaction also stages).
static int stage_receive(RcWorld *w, RcPlayer *to, const RcPlayer *from, RcItemTransaction *tx) {
    w->player = to;
    RcItemActionResult r = rc_item_tx_begin(tx, w);
    for (int i = 0; i < from->trade.offer_count && r.code == RC_ITEM_RESULT_OK; i++)
        r = rc_item_tx_add(tx, from->trade.offer[i].item_id, from->trade.offer[i].quantity,
                           from->trade.offer[i].state_id);
    return r.code == RC_ITEM_RESULT_OK;
}

int rc_trade_accept(RcWorld *w, uint32_t revision) {
    if (rc_player_command_should_queue(w))
        return queue(w, RC_PLAYER_COMMAND_TRADE_ACCEPT, (int)revision, 0, 0);
    RcPlayer *p = w ? w->player : NULL;
    if (!p || !open_trade(w, p)) return p ? result(p, RC_TRADE_NOT_OPEN) : 0;
    if (revision != p->trade.revision) return result(p, RC_TRADE_STALE);
    p->trade.accepted_revision = revision;
    RcPlayer *q = partner_of(w, p);
    if (q->trade.accepted_revision != revision) return result(p, RC_TRADE_OK);
    // Both accepted this revision: recheck everything, then exchange atomically.
    if (!alive(p) || !alive(q) || q->trade.partner != p->index || !near(p, q)) {
        end_trade(w, p, RC_TRADE_CANCELLED);
        return result(p, RC_TRADE_CANCELLED);
    }
    RcPlayer *acting = w->player;
    RcItemTransaction to_p, to_q;
    int ok = stage_receive(w, p, q, &to_p) && stage_receive(w, q, p, &to_q);
    if (ok) {
        w->player = p;
        ok = rc_item_tx_commit(&to_p).code == RC_ITEM_RESULT_OK;
        w->player = q;
        if (ok && rc_item_tx_commit(&to_q).code != RC_ITEM_RESULT_OK) {
            // Cannot happen after a clean stage; undo p's side rather than duplicate anything.
            ok = 0;
        }
    }
    w->player = acting;
    if (!ok) {
        end_trade(w, p, RC_TRADE_NO_ROOM);
        return result(p, RC_TRADE_NO_ROOM);
    }
    p->trade.offer_count = q->trade.offer_count = 0;  // delivered
    close_trade(q, RC_TRADE_COMPLETED);
    close_trade(p, RC_TRADE_COMPLETED);
    w->trades_completed++;
    return result(p, RC_TRADE_COMPLETED);
}

int rc_trade_decline(RcWorld *w) {
    if (rc_player_command_should_queue(w)) return queue(w, RC_PLAYER_COMMAND_TRADE_DECLINE, 0, 0, 0);
    RcPlayer *p = w ? w->player : NULL;
    if (!p || p->trade.stage == RC_TRADE_STAGE_NONE) return p ? result(p, RC_TRADE_NOT_OPEN) : 0;
    end_trade(w, p, RC_TRADE_CANCELLED);
    return 1;
}

void rc_trade_tick(RcWorld *w) {
    RcPlayer *p = w ? w->player : NULL;
    if (!p || p->trade.stage == RC_TRADE_STAGE_NONE) return;
    RcPlayer *q = partner_of(w, p);
    if (p->trade.stage == RC_TRADE_STAGE_REQUESTED) {
        if (!q || !alive(q) || w->tick - p->trade.requested_tick > RC_TRADE_REQUEST_TICKS)
            close_trade(p, RC_TRADE_CANCELLED);
        return;
    }
    if (!q || q->trade.partner != p->index || !alive(p) || !alive(q) || !near(p, q))
        end_trade(w, p, RC_TRADE_CANCELLED);
}

uint32_t rc_trade_completed_count(const RcWorld *w) {
    return w ? w->trades_completed : 0;
}
