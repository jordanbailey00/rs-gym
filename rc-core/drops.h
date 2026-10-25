#ifndef RC_DROPS_H
#define RC_DROPS_H

#include "types.h"

#include <stdint.h>

enum {
    RC_DROP_ALWAYS = 0,
    RC_DROP_MAIN = 1,
    RC_DROP_TERTIARY = 2,
};

enum {
    RC_DROP_UNPUBLISHED_RATE = 1u,
    RC_DROP_OUTDATED_SOURCE = 2u,
};

enum {
    RC_SHARED_DROP_RDT = 0x5F544452u,
    RC_SHARED_DROP_GDT = 0x5F544447u,
    RC_SHARED_DROP_MRDT = 0x5444524Du,
};

typedef struct {
    uint32_t item_id;
    uint16_t qmin, qmax;
    uint32_t rarity_inv;
} RcDropEntry;

typedef struct {
    uint32_t npc_id;
    uint32_t rejection_flags;
    uint32_t rare_table_weight;
    uint32_t first[3];
    uint16_t count[3];
} RcDropTable;

typedef struct {
    RcDropTable *tables;
    RcDropEntry *entries;
    RcDropEntry *rdt_entries;
    RcDropEntry *gdt_entries;
    RcDropEntry *mrdt_entries;
    int table_count;
    int entry_count;
    int rdt_entry_count;
    int gdt_entry_count;
    int mrdt_entry_count;
    int table_by_npc[RC_MAX_NPC_ID];
} RcDropData;

enum {
    RC_LOOT_ROLL_CAPACITY = -1,
    RC_LOOT_ROLL_INVALID = -2,
    RC_LOOT_ROLL_NO_TABLE = -3,
    RC_LOOT_DELIVERY_FAILED = -4,
    RC_LOOT_ROLL_UNVERIFIED = -5,
};

typedef struct {
    int item_id;
    int quantity;
    int kind;
} RcLootDrop;

extern RcDropTable *g_rc_drop_tables;
extern RcDropEntry *g_rc_drop_entries;
extern RcDropEntry *g_rc_rdt_entries;
extern RcDropEntry *g_rc_gdt_entries;
extern RcDropEntry *g_rc_mrdt_entries;
extern int g_rc_drop_table_count;
extern int g_rc_drop_entry_count;
extern int g_rc_rdt_entry_count;
extern int g_rc_gdt_entry_count;
extern int g_rc_mrdt_entry_count;

int rc_load_drops(const char *path);
int rc_load_rdt(const char *path);
int rc_load_gdt(const char *path);
int rc_load_mrdt(const char *path);
void rc_drop_data_init(RcDropData *data);
void rc_drop_data_free(RcDropData *data);
int rc_load_drops_into(const char *path, RcDropData *data);
int rc_load_rdt_into(const char *path, RcDropData *data);
int rc_load_gdt_into(const char *path, RcDropData *data);
int rc_load_mrdt_into(const char *path, RcDropData *data);
int rc_drops_mirror_to_globals(const RcDropData *data);
void rc_drops_use_data(const RcDropData *data);
void rc_drops_reset_data_if_active(const RcDropData *data);

const RcDropTable *rc_drop_table_for_npc(int npc_id);
const RcDropEntry *rc_drop_entries_for(const RcDropTable *table, int kind,
                                       int *count);
const RcDropEntry *rc_shared_drop_entries(int kind, int *count);
int rc_roll_npc_loot(RcWorld *world, int npc_id, RcLootDrop *out, int max);
void rc_npc_prepare_loot(RcWorld *world, RcNpc *npc);
void rc_set_npc_loot_hook(RcWorld *world, RcNpcLootHook hook, void *ctx);

#endif
