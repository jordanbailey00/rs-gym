#include "storage.h"
#include "api.h"
#include "config.h"
#include "interaction.h"
#include "items.h"
#include "objects.h"
#include "player_command.h"

#include <limits.h>
#include <string.h>

static int binding_kind(const RcStorageBinding *rows, int count, int id, int op) {
    if (op < 0 || op >= 5) return RC_STORAGE_NONE;
    int lo = 0, hi = count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (id < rows[mid].id) hi = mid;
        else if (id > rows[mid].id) lo = mid + 1;
        else {
            if (rows[mid].bank_mask & (1u << op)) return RC_STORAGE_BANK;
            if (rows[mid].deposit_mask & (1u << op)) return RC_STORAGE_DEPOSIT_BOX;
            break;
        }
    }
    return RC_STORAGE_NONE;
}

int rc_storage_kind_for_object(const RcWorld *w, int id, int op) {
    if (!w || !w->storage_policy) return RC_STORAGE_NONE;
    return binding_kind(w->storage_policy->objects, w->storage_policy->object_count, id, op);
}

int rc_storage_kind_for_npc(const RcWorld *w, const RcNpcDef *def, int op) {
    if (!w || !w->storage_policy || !def) return RC_STORAGE_NONE;
    return binding_kind(w->storage_policy->npcs, w->storage_policy->npc_count, def->id, op);
}

void rc_storage_close(RcWorld *w) {
    if (!w) return;
    RcPlayer *p = &w->player;
    p->storage_kind = RC_STORAGE_NONE;
    p->storage_session = 0;
    p->storage_target = p->storage_option = -1;
}

static int source_valid(RcWorld *w) {
    RcPlayer *p = &w->player;
    const RcInteractionTarget *s = &p->storage_source;
    if (!p->storage_session || p->is_dead || p->current_hp <= 0
            || p->x != p->storage_x || p->y != p->storage_y
            || p->plane != p->storage_plane) return 0;
    if (s->kind == RC_INTERACTION_NPC) {
        const RcNpc *npc = rc_npc_resolve(w, s->entity_uid);
        const RcNpcDef *def = rc_npc_def_for_npc(w, npc);
        return npc && npc->active && !npc->is_dead && npc->current_hp > 0 && npc->plane == s->plane
            && def && def->id == s->definition_id
            && rc_storage_kind_for_npc(w, def, p->storage_option) == p->storage_kind;
    }
    RcObjectPlacement loc;
    return s->kind == RC_INTERACTION_OBJECT && s->placement_key
        && rc_world_object_current_placement(w, s->definition_id,
            s->tile_x, s->tile_y, s->plane, s->placement_key, &loc)
        && (int)loc.obj_id == s->definition_id && loc.x == s->tile_x
        && loc.y == s->tile_y && loc.plane == s->plane
        && rc_storage_kind_for_object(w, (int)loc.obj_id, p->storage_option) == p->storage_kind;
}

int rc_storage_open_interaction(RcWorld *w) {
    if (!w || !(w->enabled & RC_SUB_STORAGE)) return 0;
    RcPlayer *p = &w->player;
    const RcPendingInteraction *pending = &p->interaction;
    if (!pending->active || !(pending->flags & RC_INTERACTION_ARRIVED)
            || pending->op < RC_INTERACTION_OP1 || pending->op > RC_INTERACTION_OP5) return 0;
    int op = pending->op - RC_INTERACTION_OP1;
    const RcInteractionTarget *s = &pending->target;
    int kind = s->kind == RC_INTERACTION_OBJECT
        ? rc_storage_kind_for_object(w, s->definition_id, op)
        : rc_storage_kind_for_npc(w, rc_npc_def_for_npc(w, rc_npc_resolve(w, s->entity_uid)), op);
    if (!kind) return 0;
    p->storage_kind = kind;
    p->storage_session = pending->generation;
    p->storage_source = *s;
    p->storage_target = s->kind == RC_INTERACTION_OBJECT ? s->definition_id : s->entity_uid;
    p->storage_option = op;
    p->storage_x = p->x; p->storage_y = p->y; p->storage_plane = p->plane;
    if (!source_valid(w)) { rc_storage_close(w); return 0; }
    p->interact_type = s->kind == RC_INTERACTION_NPC ? RC_INTERACT_NPC : RC_INTERACT_OBJECT;
    p->interact_target = p->storage_target;
    p->interact_option = op;
    return kind;
}

int rc_player_close_storage(RcWorld *w) {
    if (rc_player_command_should_queue(w))
        return rc_player_command_submit(w, RC_PLAYER_COMMAND_CLOSE_STORAGE,
                                         RC_ACTION_CATEGORY_SOFT, NULL, 0);
    if (!w || !(w->enabled & RC_SUB_STORAGE)) return 0;
    rc_storage_close(w);
    w->player.interact_type = RC_INTERACT_NONE;
    w->player.interact_target = w->player.interact_option = -1;
    return 1;
}

static const RcItemDef *base_item(int id) {
    const RcItemDef *def = rc_item_def_get(id);
    if (!def || def->placeholder) return NULL;
    if (!def->noted) return def;
    const RcItemDef *base = rc_item_def_get(def->linked_id_item);
    return base && !base->noted && !base->placeholder && base->linked_id_noted == id ? base : NULL;
}

static int bank_slot(const RcInvSlot *bank, const uint8_t *tabs,
                     int id, uint32_t state, int tab) {
    int empty = -1;
    for (int i = 0; i < RC_BANK_SIZE; i++) {
        if (bank[i].item_id == id && bank[i].state_id == state
                && (tab < 0 || tabs[i] == tab)) return i;
        if (bank[i].item_id < 0 && empty < 0) empty = i;
    }
    return empty;
}

int rc_bank_add_item_tab(RcWorld *w, int id, int qty, int tab) {
    const RcItemDef *def = base_item(id);
    if (!w || !(w->enabled & RC_SUB_STORAGE) || !def || qty <= 0
            || tab < -1 || tab > UINT8_MAX) return -1;
    RcPlayer *p = &w->player;
    int slot = bank_slot(p->bank, p->bank_tab, def->id, 0, tab);
    if (slot < 0 || p->bank[slot].quantity > INT_MAX - qty) return -1;
    if (p->bank[slot].item_id < 0) {
        if (++p->next_bank_generation == 0) ++p->next_bank_generation;
        p->bank[slot] = (RcInvSlot){def->id, 0, 0, p->next_bank_generation};
        p->bank_tab[slot] = tab < 0 ? 0 : (uint8_t)tab;
    }
    p->bank[slot].quantity += qty;
    p->bank_revision++;
    return slot;
}

int rc_bank_add_item(RcWorld *w, int id, int qty) { return rc_bank_add_item_tab(w, id, qty, -1); }

static int finish(RcWorld *w, RcBankResult result, RcBankResultCode code) {
    result.code = code;
    if (w) {
        result.sequence = w->player.bank_result.sequence + 1;
        w->player.bank_result = result;
    }
    return result.moved > 0 ? (int)(result.moved > INT_MAX ? INT_MAX : result.moved)
                           : code == RC_BANK_OK ? 0 : -1;
}

enum { DEPOSIT_SELECTED, DEPOSIT_INVENTORY, DEPOSIT_EQUIPMENT };

static int transfer(RcWorld *w, int slot, int qty, bool withdraw, bool noted, bool keep_one,
                    int bulk) {
    RcBankResult r = {.slot=slot, .item_id=-1, .requested=qty, .withdraw=withdraw};
    if (!w || !(w->enabled & RC_SUB_STORAGE) || !(w->enabled & RC_SUB_INVENTORY)
            || (bulk == DEPOSIT_EQUIPMENT && !(w->enabled & RC_SUB_EQUIPMENT)))
        return finish(w, r, RC_BANK_DISABLED);
    RcPlayer *p = &w->player;
    if ((p->storage_kind != RC_STORAGE_BANK && p->storage_kind != RC_STORAGE_DEPOSIT_BOX)
            || !source_valid(w)) {
        rc_storage_close(w);
        return finish(w, r, RC_BANK_CLOSED);
    }
    if ((withdraw && p->storage_kind != RC_STORAGE_BANK) || qty < 0
            || (!bulk && (slot < 0 || slot >= (withdraw ? RC_BANK_SIZE : RC_INVENTORY_SIZE))))
        return finish(w, r, RC_BANK_INVALID);
    RcInvSlot item = bulk ? (RcInvSlot){.item_id=-1}
                         : withdraw ? p->bank[slot] : p->inventory[slot];
    r.item_id = item.item_id;
    if (!bulk && (item.item_id < 0 || item.quantity <= 0)) return finish(w, r, RC_BANK_STALE);
    const RcItemDef *def = bulk ? NULL : base_item(item.item_id);
    if (!bulk && !def) return finish(w, r, RC_BANK_BAD_NOTE);
    if (!w->storage_policy || !w->storage_policy->item_bankable)
        return finish(w, r, RC_BANK_UNREVIEWED);
    RcItemTransaction tx;
    if (rc_item_tx_begin(&tx, w).code != RC_ITEM_RESULT_OK) return finish(w, r, RC_BANK_CONFLICT);
    if (withdraw) {
        if (noted && def->noteable && item.state_id == 0) {
            const RcItemDef *note = rc_item_def_get(def->linked_id_noted);
            if (!note || !note->noted || note->placeholder || !note->stackable
                    || note->linked_id_item != def->id)
                return finish(w, r, RC_BANK_BAD_NOTE);
            def = note;
        } else if (noted) r.cannot_note = true;
        int available = item.quantity - (keep_one ? 1 : 0);
        int wanted = qty == 0 || qty > available ? available : qty;
        if (!wanted) return finish(w, r, RC_BANK_OK);
        int capacity = 0;
        for (int i = 0; i < RC_INVENTORY_SIZE; i++) {
            if (def->stackable && tx.inventory[i].item_id == def->id
                    && tx.inventory[i].state_id == item.state_id) {
                capacity = INT_MAX - tx.inventory[i].quantity;
                break;
            }
            if (tx.inventory[i].item_id < 0) capacity = def->stackable ? INT_MAX : capacity + 1;
        }
        int move = wanted < capacity ? wanted : capacity;
        if (move <= 0) return finish(w, r, RC_BANK_CAPACITY);
        if (rc_item_tx_add(&tx, def->id, move, item.state_id).code != RC_ITEM_RESULT_OK
                || rc_item_tx_commit(&tx).code != RC_ITEM_RESULT_OK) return finish(w, r, RC_BANK_CONFLICT);
        p->bank[slot].quantity -= move;
        if (!p->bank[slot].quantity) {
            if (++p->next_bank_generation == 0) ++p->next_bank_generation;
            p->bank[slot] = (RcInvSlot){.item_id=-1, .generation=p->next_bank_generation};
            p->bank_tab[slot] = 0;
        }
        p->bank_revision++;
        r.moved = move;
        return finish(w, r, move < wanted ? RC_BANK_PARTIAL : RC_BANK_OK);
    }
    RcInvSlot staged[RC_BANK_SIZE];
    uint8_t tabs[RC_BANK_SIZE];
    uint32_t next_generation = p->next_bank_generation;
    memcpy(staged, p->bank, sizeof(staged));
    memcpy(tabs, p->bank_tab, sizeof(tabs));
    RcInvSlot *sources = bulk == DEPOSIT_EQUIPMENT ? tx.equipment : tx.inventory;
    int count = bulk == DEPOSIT_EQUIPMENT ? RC_EQUIP_COUNT : RC_INVENTORY_SIZE;
    RcInvSlot worn[RC_EQUIP_COUNT];
    if (bulk == DEPOSIT_EQUIPMENT) memcpy(worn, p->equipment, sizeof(worn));
    int64_t available = 0;
    for (int i = 0; i < count; i++)
        if (sources[i].item_id >= 0 && (bulk || sources[i].item_id == item.item_id))
            available += sources[i].quantity;
    if (bulk && !available) return finish(w, r, RC_BANK_OK);
    int64_t wanted_total = qty > 0 && qty < available ? qty : available;
    int64_t remaining = bulk ? available : qty == 0 ? INT_MAX : qty;
    bool unbankable = false;
    for (int n = 0; n < count && remaining > 0; n++) {
        int i = ((bulk ? 0 : slot) + n) % count;
        RcInvSlot source = sources[i];
        if (source.item_id < 0 || (!bulk && source.item_id != item.item_id) || source.quantity <= 0) continue;
        def = base_item(source.item_id);
        if (!def || (def->id != source.item_id && source.state_id)) {
            r.moved = 0;
            return finish(w, r, RC_BANK_BAD_NOTE);
        }
        int allowed = w->storage_policy->item_bankable(def->id);
        int source_allowed = w->storage_policy->item_bankable(source.item_id);
        if (allowed < 0 || source_allowed < 0) {
            r.moved = 0;
            return finish(w, r, RC_BANK_UNREVIEWED);
        }
        if (!allowed || !source_allowed) { unbankable = true; continue; }
        int wanted = source.quantity < remaining ? source.quantity : (int)remaining;
        int dest = bank_slot(staged, tabs, def->id, source.state_id, -1);
        int capacity = dest < 0 ? 0 : INT_MAX - staged[dest].quantity;
        int move = wanted < capacity ? wanted : capacity;
        if (!move) continue;
        RcItemActionResult removed = bulk == DEPOSIT_EQUIPMENT
            ? rc_item_tx_remove_equipment(&tx, i, move, source.generation)
            : rc_item_tx_remove_slot(&tx, i, move, source.generation);
        if (removed.code != RC_ITEM_RESULT_OK) {
            r.moved = 0;
            return finish(w, r, RC_BANK_CONFLICT);
        }
        if (staged[dest].item_id < 0) {
            tabs[dest] = 0;
            if (++next_generation == 0) ++next_generation;
            staged[dest].generation = next_generation;
        }
        staged[dest].item_id = def->id;
        staged[dest].state_id = source.state_id;
        staged[dest].quantity += move;
        remaining -= move;
        r.moved += move;
    }
    if (!r.moved) return finish(w, r, unbankable ? RC_BANK_UNBANKABLE : RC_BANK_CAPACITY);
    if (rc_item_tx_commit(&tx).code != RC_ITEM_RESULT_OK) {
        r.moved = 0;
        return finish(w, r, RC_BANK_CONFLICT);
    }
    memcpy(p->bank, staged, sizeof(staged));
    memcpy(p->bank_tab, tabs, sizeof(tabs));
    p->next_bank_generation = next_generation;
    p->bank_revision++;
    if (bulk == DEPOSIT_EQUIPMENT) {
        for (int i = 0; i < RC_EQUIP_COUNT; i++) {
            int removed = worn[i].quantity - tx.equipment[i].quantity;
            if (worn[i].item_id < 0 || removed <= 0) continue;
            RcPayloadItemEvent event = {.item_id=(uint32_t)worn[i].item_id,
                .quantity=(uint32_t)removed,.slot=(uint8_t)i,.ground_uid=-1};
            rc_event_fire(w, RC_EVT_ITEM_UNEQUIPPED, &event);
        }
    }
    return finish(w, r, unbankable ? RC_BANK_UNBANKABLE
                                  : r.moved < wanted_total ? RC_BANK_PARTIAL : RC_BANK_OK);
}

static int submit(RcWorld *w, int slot, int qty, bool withdraw, bool noted, bool keep_one) {
    if (!rc_player_command_should_queue(w)) return transfer(w, slot, qty, withdraw, noted, keep_one, DEPOSIT_SELECTED);
    RcPlayer *p = &w->player;
    RcInvSlot source = {.item_id=-1};
    if (slot >= 0 && slot < (withdraw ? RC_BANK_SIZE : RC_INVENTORY_SIZE))
        source = withdraw ? p->bank[slot] : p->inventory[slot];
    int args[8] = {slot, qty, (int)source.generation, source.item_id, (int)source.state_id, noted, keep_one};
    if (rc_player_command_submit(w, withdraw ? RC_PLAYER_COMMAND_BANK_WITHDRAW : RC_PLAYER_COMMAND_BANK_DEPOSIT,
                                 RC_ACTION_CATEGORY_BACKGROUND, args, p->storage_session)) return 1;
    finish(w, (RcBankResult){.slot=slot,.item_id=source.item_id,.requested=qty,.withdraw=withdraw}, RC_BANK_QUEUE_REJECTED);
    return 0;
}

int rc_bank_deposit_slot(RcWorld *w, int slot, int qty) { return submit(w, slot, qty, false, false, false); }
int rc_bank_deposit_all(RcWorld *w, bool equipment) {
    int bulk = equipment ? DEPOSIT_EQUIPMENT : DEPOSIT_INVENTORY;
    if (!rc_player_command_should_queue(w)) return transfer(w, -1, 0, false, false, false, bulk);
    int args[8] = {equipment};
    if (rc_player_command_submit(w, RC_PLAYER_COMMAND_BANK_DEPOSIT_ALL,
            RC_ACTION_CATEGORY_BACKGROUND, args, w->player.storage_session)) return 1;
    finish(w, (RcBankResult){.slot=-1,.item_id=-1}, RC_BANK_QUEUE_REJECTED);
    return 0;
}
int rc_bank_withdraw_slot(RcWorld *w, int slot, int qty) { return submit(w, slot, qty, true, false, false); }
int rc_bank_withdraw_slot_mode(RcWorld *w, int slot, int qty, bool noted, bool keep_one) {
    return submit(w, slot, qty, true, noted, keep_one);
}

int rc_bank_execute_command(RcWorld *w, const RcPlayerCommand *command) {
    const int *a = command->args;
    bool withdraw = command->kind == RC_PLAYER_COMMAND_BANK_WITHDRAW;
    RcBankResult r = {.slot=a[0], .item_id=a[3], .requested=a[1], .withdraw=withdraw};
    if (!command->key || command->key != w->player.storage_session)
        return finish(w, r, RC_BANK_CLOSED);
    if (command->kind == RC_PLAYER_COMMAND_BANK_DEPOSIT_ALL)
        return rc_bank_deposit_all(w, a[0] != 0);
    if (a[0] < 0 || a[0] >= (withdraw ? RC_BANK_SIZE : RC_INVENTORY_SIZE))
        return finish(w, r, RC_BANK_INVALID);
    const RcInvSlot *source = withdraw ? &w->player.bank[a[0]] : &w->player.inventory[a[0]];
    if (source->generation != (uint32_t)a[2] || source->item_id != a[3] || source->state_id != (uint32_t)a[4])
        return finish(w, r, RC_BANK_STALE);
    return transfer(w, a[0], a[1], withdraw, a[5] != 0, a[6] != 0, DEPOSIT_SELECTED);
}

const char *rc_bank_result_message(const RcBankResult *r) {
    switch (r->code) {
    case RC_BANK_OK: return r->cannot_note ? "This item cannot be withdrawn as a note; withdrawn as an item."
        : r->slot == -1 && !r->moved ? "You have nothing to deposit." : NULL;
    case RC_BANK_PARTIAL: return "Only some items could be moved: not enough space or stack capacity.";
    case RC_BANK_INVALID: return "That storage operation or quantity is not available.";
    case RC_BANK_DISABLED: return "Banking requires storage/inventory; worn deposits also require equipment.";
    case RC_BANK_CLOSED: return "Your bank access has ended. Open a bank or deposit box again.";
    case RC_BANK_STALE: return "That item slot changed. Select the item again.";
    case RC_BANK_CAPACITY: return "No items moved: not enough space or stack capacity.";
    case RC_BANK_UNBANKABLE: return r->moved
        ? "Some items remain: they cannot be banked, or there is not enough bank space."
        : "A magical force prevents you from banking this item!";
    case RC_BANK_UNREVIEWED: return "Banking policy for this item is not reviewed.";
    case RC_BANK_BAD_NOTE: return "The item's note link or state is invalid; no items moved.";
    case RC_BANK_CONFLICT: return "The item transfer changed before completion; no items moved.";
    case RC_BANK_QUEUE_REJECTED: return "Bank action rejected: you are busy, dead, or the input queue is full.";
    }
    return "Unknown bank transfer result.";
}
