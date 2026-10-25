#ifndef RC_STORAGE_H
#define RC_STORAGE_H

#include "npc.h"
#include "types.h"

enum {
    RC_STORAGE_NONE = 0,
    RC_STORAGE_BANK = 1,
    RC_STORAGE_DEPOSIT_BOX = 2,
};

typedef struct { int id; uint8_t bank_mask, deposit_mask; } RcStorageBinding;
typedef struct RcStoragePolicy {
    const RcStorageBinding *objects, *npcs;
    int object_count, npc_count;
    int (*item_bankable)(int item_id); // 1 allowed, 0 prohibited, -1 unreviewed
} RcStoragePolicy;

int rc_storage_kind_for_object(const RcWorld *world, int obj_id, int option);
int rc_storage_kind_for_npc(const RcWorld *world, const RcNpcDef *def, int option);
// Called only after the interaction owner has validated arrival at the source.
int rc_storage_open_interaction(RcWorld *world);
void rc_storage_close(RcWorld *world);
int rc_player_close_storage(RcWorld *world);
// Privileged setup APIs: validate definitions, but do not grant player access.
int rc_bank_add_item(RcWorld *world, int item_id, int quantity);
int rc_bank_add_item_tab(RcWorld *world, int item_id, int quantity, int tab);
int rc_bank_deposit_slot(RcWorld *world, int inv_slot, int quantity);
int rc_bank_deposit_all(RcWorld *world, bool equipment);
int rc_bank_withdraw_slot(RcWorld *world, int bank_slot, int quantity);
int rc_bank_withdraw_slot_mode(RcWorld *world, int bank_slot, int quantity,
                               bool noted, bool keep_one);
int rc_bank_execute_command(RcWorld *world, const RcPlayerCommand *command);
const char *rc_bank_result_message(const RcBankResult *result);

#endif
