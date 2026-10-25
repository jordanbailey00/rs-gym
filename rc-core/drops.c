#include "drops.h"
#include "io.h"
#include "rng.h"
#include "items.h"
#include "npc.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define DROP_MAGIC 0x504F5244u
#define DROP_VERSION 2u

RcDropTable *g_rc_drop_tables = NULL;
RcDropEntry *g_rc_drop_entries = NULL;
RcDropEntry *g_rc_rdt_entries = NULL;
RcDropEntry *g_rc_gdt_entries = NULL;
RcDropEntry *g_rc_mrdt_entries = NULL;
int g_rc_drop_table_count = 0;
int g_rc_drop_entry_count = 0;
int g_rc_rdt_entry_count = 0;
int g_rc_gdt_entry_count = 0;
int g_rc_mrdt_entry_count = 0;

static int g_drop_table_by_npc[RC_MAX_NPC_ID];
static const RcDropTable *g_active_drop_tables = NULL;
static const RcDropEntry *g_active_drop_entries = NULL;
static const RcDropEntry *g_active_rdt_entries = NULL;
static const RcDropEntry *g_active_gdt_entries = NULL;
static const RcDropEntry *g_active_mrdt_entries = NULL;
static const int *g_active_drop_table_by_npc = g_drop_table_by_npc;
static int g_active_drop_table_count = 0;
static int g_active_drop_entry_count = 0;
static int g_active_rdt_entry_count = 0;
static int g_active_gdt_entry_count = 0;
static int g_active_mrdt_entry_count = 0;

static void reset_drop_index(void) {
    for (int i = 0; i < RC_MAX_NPC_ID; i++) g_drop_table_by_npc[i] = -1;
}

static void reset_drop_index_into(int *index) {
    if (!index) return;
    for (int i = 0; i < RC_MAX_NPC_ID; i++) index[i] = -1;
}

static void build_drop_index(const RcDropTable *tables, int table_count,
                             int *index) {
    reset_drop_index_into(index);
    if (!tables || !index) return;
    for (int i = 0; i < table_count; i++) {
        uint32_t npc_id = tables[i].npc_id;
        if (npc_id < RC_MAX_NPC_ID) index[npc_id] = i;
    }
}

void rc_drop_data_init(RcDropData *data) {
    if (!data) return;
    data->tables = NULL;
    data->entries = NULL;
    data->rdt_entries = NULL;
    data->gdt_entries = NULL;
    data->mrdt_entries = NULL;
    data->table_count = 0;
    data->entry_count = 0;
    data->rdt_entry_count = 0;
    data->gdt_entry_count = 0;
    data->mrdt_entry_count = 0;
    reset_drop_index_into(data->table_by_npc);
}

void rc_drop_data_free(RcDropData *data) {
    if (!data) return;
    free(data->tables);
    free(data->entries);
    free(data->rdt_entries);
    free(data->gdt_entries);
    free(data->mrdt_entries);
    rc_drop_data_init(data);
}

void rc_drops_use_data(const RcDropData *data) {
    if (!data) {
        g_active_drop_tables = g_rc_drop_tables;
        g_active_drop_entries = g_rc_drop_entries;
        g_active_rdt_entries = g_rc_rdt_entries;
        g_active_gdt_entries = g_rc_gdt_entries;
        g_active_mrdt_entries = g_rc_mrdt_entries;
        g_active_drop_table_by_npc = g_drop_table_by_npc;
        g_active_drop_table_count = g_rc_drop_table_count;
        g_active_drop_entry_count = g_rc_drop_entry_count;
        g_active_rdt_entry_count = g_rc_rdt_entry_count;
        g_active_gdt_entry_count = g_rc_gdt_entry_count;
        g_active_mrdt_entry_count = g_rc_mrdt_entry_count;
        return;
    }
    g_active_drop_tables = data->tables;
    g_active_drop_entries = data->entries;
    g_active_rdt_entries = data->rdt_entries;
    g_active_gdt_entries = data->gdt_entries;
    g_active_mrdt_entries = data->mrdt_entries;
    g_active_drop_table_by_npc = data->table_by_npc;
    g_active_drop_table_count = data->table_count;
    g_active_drop_entry_count = data->entry_count;
    g_active_rdt_entry_count = data->rdt_entry_count;
    g_active_gdt_entry_count = data->gdt_entry_count;
    g_active_mrdt_entry_count = data->mrdt_entry_count;
}

void rc_drops_reset_data_if_active(const RcDropData *data) {
    if (!data) return;
    if (g_active_drop_tables == data->tables
            || g_active_drop_entries == data->entries
            || g_active_rdt_entries == data->rdt_entries
            || g_active_gdt_entries == data->gdt_entries
            || g_active_mrdt_entries == data->mrdt_entries) {
        rc_drops_use_data(NULL);
    }
}

static int read_header(FILE *f, const char *path, uint32_t magic,
                       uint32_t *count) {
    uint32_t got_magic, version;
    if (!rc_read_exact(f, &got_magic, sizeof(got_magic), 1, path, "magic")
            || !rc_read_exact(f, &version, sizeof(version), 1, path,
                              "version")
            || !rc_read_exact(f, count, sizeof(*count), 1, path, "count")) {
        return 0;
    }
    uint32_t expected = magic == DROP_MAGIC ? DROP_VERSION : 1u;
    if (got_magic != magic || version != expected) {
        fprintf(stderr, "%s: expected magic=%08x version=%u, got %08x/%u; rebuild data\n",
                path, magic, expected, got_magic, version);
        return 0;
    }
    return 1;
}

static int append_entry(RcDropEntry **rows, int *count, int *cap,
                        RcDropEntry row) {
    if (*count >= *cap) {
        int next_cap = *cap ? *cap * 2 : 256;
        RcDropEntry *next = realloc(*rows, (size_t)next_cap * sizeof(**rows));
        if (!next) return 0;
        *rows = next;
        *cap = next_cap;
    }
    (*rows)[(*count)++] = row;
    return 1;
}

static int read_entry(FILE *f, const char *path, RcDropEntry *row,
                      int has_rarity) {
    row->rarity_inv = has_rarity ? 0 : 1;
    int ok = rc_read_exact(f, &row->item_id, sizeof(row->item_id), 1, path,
                         "item id")
        && rc_read_exact(f, &row->qmin, sizeof(row->qmin), 1, path, "qmin")
        && rc_read_exact(f, &row->qmax, sizeof(row->qmax), 1, path, "qmax")
        && (!has_rarity
            || rc_read_exact(f, &row->rarity_inv, sizeof(row->rarity_inv), 1,
                             path, "rarity"));
    if (ok && (row->item_id >= RC_MAX_ITEM_DEFS || (!row->qmin && row->item_id)
            || row->qmin > row->qmax || row->rarity_inv > INT_MAX)) {
        fprintf(stderr, "%s: invalid drop item, quantity or rarity\n", path);
        return 0;
    }
    return ok;
}

int rc_load_drops_into(const char *path, RcDropData *data) {
    if (!path || !data) return -1;
    FILE *f = rc_asset_fopen(path, "rb");
    if (!f) return -1;

    uint32_t count;
    if (!read_header(f, path, DROP_MAGIC, &count) || count > RC_MAX_NPC_ID) {
        rc_asset_close(f);
        return -1;
    }

    RcDropTable *tables = calloc(count ? count : 1u, sizeof(*tables));
    RcDropEntry *entries = NULL;
    int entry_count = 0, entry_cap = 0;
    if (!tables) {
        rc_asset_close(f);
        return -1;
    }

    unsigned char seen[RC_MAX_NPC_ID] = {0};
    for (uint32_t i = 0; i < count; i++) {
        RcDropTable *table = &tables[i];
        uint8_t n;
        if (!rc_read_exact(f, &table->npc_id, sizeof(table->npc_id), 1, path,
                           "npc id")) {
            free(tables); free(entries); rc_asset_close(f); return -1;
        }
        if (table->npc_id >= RC_MAX_NPC_ID || seen[table->npc_id]) {
            fprintf(stderr, "%s: duplicate or invalid NPC drop table %u\n", path, table->npc_id);
            free(tables); free(entries); rc_asset_close(f); return -1;
        }
        seen[table->npc_id] = 1;
        if (!rc_read_exact(f, &table->rejection_flags, sizeof(table->rejection_flags),
                           1, path, "loot rejection flags")
                || table->rejection_flags & ~(RC_DROP_UNPUBLISHED_RATE | RC_DROP_OUTDATED_SOURCE)) {
            fprintf(stderr, "%s: invalid loot rejection flags for NPC %u\n", path, table->npc_id);
            free(tables); free(entries); rc_asset_close(f); return -1;
        }
        for (int kind = RC_DROP_ALWAYS; kind <= RC_DROP_TERTIARY; kind++) {
            if (!rc_read_exact(f, &n, sizeof(n), 1, path, "drop count")) {
                free(tables); free(entries); rc_asset_close(f); return -1;
            }
            table->first[kind] = (uint32_t)entry_count;
            table->count[kind] = n;
            for (int j = 0; j < n; j++) {
                RcDropEntry row;
                if (!read_entry(f, path, &row, kind != RC_DROP_ALWAYS)
                        || !append_entry(&entries, &entry_count, &entry_cap,
                                         row)) {
                    free(tables); free(entries); rc_asset_close(f); return -1;
                }
            }
        }
        if (!rc_read_exact(f, &table->rare_table_weight,
                           sizeof(table->rare_table_weight), 1, path,
                           "rare table weight")) {
            free(tables); free(entries); rc_asset_close(f); return -1;
        }
        if (table->rejection_flags && (table->count[0] || table->count[1]
                || table->count[2] || table->rare_table_weight)) {
            fprintf(stderr, "%s: rejected NPC %u must not contain fallback rewards\n", path, table->npc_id);
            free(tables); free(entries); rc_asset_close(f); return -1;
        }
    }
    if (fgetc(f) != EOF || ferror(f)) {
        fprintf(stderr, "%s: unexpected trailing drop data\n", path);
        free(tables); free(entries); rc_asset_close(f); return -1;
    }
    rc_asset_close(f);

    free(data->tables);
    free(data->entries);
    data->tables = tables;
    data->entries = entries;
    data->table_count = (int)count;
    data->entry_count = entry_count;
    build_drop_index(data->tables, data->table_count, data->table_by_npc);
    return data->table_count;
}

int rc_drops_mirror_to_globals(const RcDropData *data) {
    if (!data) return 0;
    RcDropTable *tables = NULL;
    RcDropEntry *entries = NULL;
    RcDropEntry *rdt = NULL;
    RcDropEntry *gdt = NULL;
    RcDropEntry *mrdt = NULL;

    if (data->table_count > 0) {
        if (!data->tables) goto fail;
        tables = malloc((size_t)data->table_count * sizeof(*tables));
        if (!tables) goto fail;
        memcpy(tables, data->tables,
               (size_t)data->table_count * sizeof(*tables));
    }
    if (data->entry_count > 0) {
        if (!data->entries) goto fail;
        entries = malloc((size_t)data->entry_count * sizeof(*entries));
        if (!entries) goto fail;
        memcpy(entries, data->entries,
               (size_t)data->entry_count * sizeof(*entries));
    }
    if (data->rdt_entry_count > 0) {
        if (!data->rdt_entries) goto fail;
        rdt = malloc((size_t)data->rdt_entry_count * sizeof(*rdt));
        if (!rdt) goto fail;
        memcpy(rdt, data->rdt_entries,
               (size_t)data->rdt_entry_count * sizeof(*rdt));
    }
    if (data->gdt_entry_count > 0) {
        if (!data->gdt_entries) goto fail;
        gdt = malloc((size_t)data->gdt_entry_count * sizeof(*gdt));
        if (!gdt) goto fail;
        memcpy(gdt, data->gdt_entries,
               (size_t)data->gdt_entry_count * sizeof(*gdt));
    }
    if (data->mrdt_entry_count > 0) {
        if (!data->mrdt_entries) goto fail;
        mrdt = malloc((size_t)data->mrdt_entry_count * sizeof(*mrdt));
        if (!mrdt) goto fail;
        memcpy(mrdt, data->mrdt_entries,
               (size_t)data->mrdt_entry_count * sizeof(*mrdt));
    }

    free(g_rc_drop_tables);
    free(g_rc_drop_entries);
    free(g_rc_rdt_entries);
    free(g_rc_gdt_entries);
    free(g_rc_mrdt_entries);
    g_rc_drop_tables = tables;
    g_rc_drop_entries = entries;
    g_rc_rdt_entries = rdt;
    g_rc_gdt_entries = gdt;
    g_rc_mrdt_entries = mrdt;
    g_rc_drop_table_count = data->table_count;
    g_rc_drop_entry_count = data->entry_count;
    g_rc_rdt_entry_count = data->rdt_entry_count;
    g_rc_gdt_entry_count = data->gdt_entry_count;
    g_rc_mrdt_entry_count = data->mrdt_entry_count;
    build_drop_index(g_rc_drop_tables, g_rc_drop_table_count,
                     g_drop_table_by_npc);
    return 1;

fail:
    free(tables);
    free(entries);
    free(rdt);
    free(gdt);
    free(mrdt);
    return 0;
}

int rc_load_drops(const char *path) {
    RcDropData data;
    rc_drop_data_init(&data);
    int loaded = rc_load_drops_into(path, &data);
    if (loaded < 0) return -1;
    if (!rc_drops_mirror_to_globals(&data)) {
        rc_drop_data_free(&data);
        return -1;
    }
    rc_drop_data_free(&data);
    rc_drops_use_data(NULL);
    return g_rc_drop_table_count;
}

static int load_shared_into(const char *path, uint32_t magic,
                            RcDropEntry **dst, int *dst_count) {
    if (!path) return -1;
    FILE *f = rc_asset_fopen(path, "rb");
    if (!f) return -1;

    uint32_t count;
    if (!read_header(f, path, magic, &count) || count > RC_MAX_ITEM_DEFS) {
        rc_asset_close(f);
        return -1;
    }
    RcDropEntry *rows = calloc(count ? count : 1u, sizeof(*rows));
    if (!rows) {
        rc_asset_close(f);
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!read_entry(f, path, &rows[i], 1)) {
            free(rows);
            rc_asset_close(f);
            return -1;
        }
    }
    if (fgetc(f) != EOF || ferror(f)) {
        fprintf(stderr, "%s: unexpected trailing shared drop data\n", path);
        free(rows); rc_asset_close(f); return -1;
    }
    rc_asset_close(f);
    free(*dst);
    *dst = rows;
    *dst_count = (int)count;
    return *dst_count;
}

static int load_shared_global(const char *path, uint32_t magic,
                              RcDropEntry **dst, int *dst_count) {
    int loaded = load_shared_into(path, magic, dst, dst_count);
    if (loaded >= 0) rc_drops_use_data(NULL);
    return loaded;
}

int rc_load_rdt(const char *path) {
    return load_shared_global(path, RC_SHARED_DROP_RDT, &g_rc_rdt_entries,
                              &g_rc_rdt_entry_count);
}

int rc_load_gdt(const char *path) {
    return load_shared_global(path, RC_SHARED_DROP_GDT, &g_rc_gdt_entries,
                              &g_rc_gdt_entry_count);
}

int rc_load_mrdt(const char *path) {
    return load_shared_global(path, RC_SHARED_DROP_MRDT, &g_rc_mrdt_entries,
                              &g_rc_mrdt_entry_count);
}

int rc_load_rdt_into(const char *path, RcDropData *data) {
    return data ? load_shared_into(path, RC_SHARED_DROP_RDT,
                                   &data->rdt_entries,
                                   &data->rdt_entry_count) : -1;
}

int rc_load_gdt_into(const char *path, RcDropData *data) {
    return data ? load_shared_into(path, RC_SHARED_DROP_GDT,
                                   &data->gdt_entries,
                                   &data->gdt_entry_count) : -1;
}

int rc_load_mrdt_into(const char *path, RcDropData *data) {
    return data ? load_shared_into(path, RC_SHARED_DROP_MRDT,
                                   &data->mrdt_entries,
                                   &data->mrdt_entry_count) : -1;
}

const RcDropTable *rc_drop_table_for_npc(int npc_id) {
    if (npc_id < 0 || npc_id >= RC_MAX_NPC_ID
            || g_active_drop_table_count <= 0
            || !g_active_drop_tables || !g_active_drop_table_by_npc) {
        return NULL;
    }
    int idx = g_active_drop_table_by_npc[npc_id];
    return idx >= 0 && idx < g_active_drop_table_count ? &g_active_drop_tables[idx] : NULL;
}

const RcDropEntry *rc_drop_entries_for(const RcDropTable *table, int kind,
                                       int *count) {
    if (count) *count = 0;
    if (!table || kind < RC_DROP_ALWAYS || kind > RC_DROP_TERTIARY) {
        return NULL;
    }
    if (!g_active_drop_entries || table->first[kind] > (uint32_t)g_active_drop_entry_count
            || table->count[kind] > (uint32_t)g_active_drop_entry_count - table->first[kind])
        return NULL;
    if (count) *count = table->count[kind];
    return &g_active_drop_entries[table->first[kind]];
}

const RcDropEntry *rc_shared_drop_entries(int kind, int *count) {
    if (count) *count = 0;
    if (kind == RC_SHARED_DROP_RDT) {
        if (count) *count = g_active_rdt_entry_count;
        return g_active_rdt_entries;
    }
    if (kind == RC_SHARED_DROP_GDT) {
        if (count) *count = g_active_gdt_entry_count;
        return g_active_gdt_entries;
    }
    if (kind == RC_SHARED_DROP_MRDT) {
        if (count) *count = g_active_mrdt_entry_count;
        return g_active_mrdt_entries;
    }
    return NULL;
}

static int drop_quantity(RcWorld *world, const RcDropEntry *entry) {
    int min = (int)entry->qmin;
    int max = (int)entry->qmax;
    if (max < min) max = min;
    if (max == min) return min;
    return min + rc_rng_range(&world->rng_state, max - min);
}

static int drop_roll_succeeds(RcWorld *world, const RcDropEntry *entry) {
    uint32_t inv = entry->rarity_inv;
    if (inv == 0u) return 0;
    return inv == 1u || rc_rng_range(&world->rng_state, (int)inv - 1) == 0;
}

static uint32_t drop_weight(const RcDropEntry *entry) {
    if (entry->rarity_inv == 0u) return 0;
    uint32_t weight = 1000000u / entry->rarity_inv;
    return weight ? weight : 1u;
}

static int roll_main_entry(RcWorld *world, const RcDropEntry *entries,
                           int count) {
    uint64_t total = 0;
    for (int i = 0; i < count; i++) total += drop_weight(&entries[i]);
    if (total == 0) return -1;

    uint64_t roll = rc_rng_next(&world->rng_state) % total;
    for (int i = 0; i < count; i++) {
        uint32_t weight = drop_weight(&entries[i]);
        if (!weight) continue;
        if (roll < weight) return i;
        roll -= weight;
    }
    return -1;
}

static int append_loot(RcLootDrop *out, int max, int count,
                       const RcDropEntry *entry, int kind, int quantity) {
    if (!out || count >= max || entry->item_id == 0 || quantity <= 0) {
        return count;
    }
    out[count++] = (RcLootDrop){
        .item_id = (int)entry->item_id,
        .quantity = quantity,
        .kind = kind,
    };
    return count;
}

int rc_roll_npc_loot(RcWorld *world, int npc_id, RcLootDrop *out, int max) {
    if (!world || !out || max <= 0) return RC_LOOT_ROLL_INVALID;
    const RcDropTable *table = rc_drop_table_for_npc(npc_id);
    if (!table) return RC_LOOT_ROLL_NO_TABLE;
    if (table->rejection_flags) return RC_LOOT_ROLL_UNVERIFIED;
    int required = table->count[RC_DROP_ALWAYS]
                 + (table->count[RC_DROP_MAIN] > 0)
                 + table->count[RC_DROP_TERTIARY];
    if (required > max) return RC_LOOT_ROLL_CAPACITY;
    for (int kind = 0; kind < 3; kind++) {
        int count;
        const RcDropEntry *rows = rc_drop_entries_for(table, kind, &count);
        if (count != table->count[kind] || (count && !rows))
            return RC_LOOT_ROLL_INVALID;
        for (int i = 0; i < count; i++)
            if (!rc_item_def_get((int)rows[i].item_id)
                    || !rows[i].qmin || rows[i].qmax < rows[i].qmin
                    || !rows[i].rarity_inv)
                return RC_LOOT_ROLL_INVALID;
    }

    int total = 0;
    int n = 0;
    const RcDropEntry *entries =
        rc_drop_entries_for(table, RC_DROP_ALWAYS, &n);
    for (int i = 0; i < n && total < max; i++) {
        total = append_loot(out, max, total, &entries[i], RC_DROP_ALWAYS,
                            drop_quantity(world, &entries[i]));
    }

    entries = rc_drop_entries_for(table, RC_DROP_MAIN, &n);
    int main = roll_main_entry(world, entries, n);
    if (main >= 0 && total < max) {
        total = append_loot(out, max, total, &entries[main], RC_DROP_MAIN,
                            drop_quantity(world, &entries[main]));
    }

    entries = rc_drop_entries_for(table, RC_DROP_TERTIARY, &n);
    for (int i = 0; i < n && total < max; i++) {
        if (drop_roll_succeeds(world, &entries[i])) {
            total = append_loot(out, max, total, &entries[i],
                                RC_DROP_TERTIARY,
                                drop_quantity(world, &entries[i]));
        }
    }
    return total;
}

void rc_set_npc_loot_hook(RcWorld *world, RcNpcLootHook hook, void *ctx) {
    if (!world) return;
    world->npc_loot_hook = hook;
    world->npc_loot_ctx = ctx;
}

void rc_npc_prepare_loot(RcWorld *world, RcNpc *npc) {
    if (!world || !npc || !(world->enabled & RC_SUB_LOOT)
            || !npc->is_dead || npc->loot_prepared) return;
    npc->loot_prepared = true;
    if (!npc->player_loot_credit) return;
    const RcNpcDef *def = rc_npc_def_for_npc(world, npc);
    if (!def) return;
    RcLootReceipt receipt = {.npc_uid = npc->uid, .npc_def_id = def->id,
        .x = npc->x, .y = npc->y, .plane = npc->plane, .tick = world->tick};
    RcLootPolicy policy = world->npc_loot_hook
        ? world->npc_loot_hook(world, npc, receipt.grants, RC_MAX_LOOT_DROPS,
                              &receipt.count, world->npc_loot_ctx)
        : RC_LOOT_GENERIC;
    if (policy == RC_LOOT_SUPPRESS) receipt.count = 0;
    else if (receipt.count < 0 || receipt.count > RC_MAX_LOOT_DROPS
            || policy < RC_LOOT_GENERIC || policy > RC_LOOT_AUGMENT)
        receipt.result = RC_LOOT_ROLL_INVALID;
    else if (policy == RC_LOOT_GENERIC || policy == RC_LOOT_AUGMENT) {
        if (policy == RC_LOOT_GENERIC) receipt.count = 0;
        RcLootDrop rolls[RC_MAX_LOOT_DROPS];
        int n = rc_roll_npc_loot(world, def->id, rolls,
                                 RC_MAX_LOOT_DROPS - receipt.count);
        if (n < 0) receipt.result = n;
        else for (int i = 0; i < n; i++)
            receipt.grants[receipt.count++] = (RcGroundGrant){
                .item_id = rolls[i].item_id, .quantity = rolls[i].quantity};
    }
    if (!receipt.result && receipt.count) {
        RcGroundPolicy ground = {.reveal_ticks = 100, .lifetime_ticks = 200,
            .delay_ticks = npc->death_timer, .untradeable_lifetime_ticks = 300};
        RcGroundGrantResult result = rc_ground_grant(world, receipt.grants,
            receipt.count, receipt.x, receipt.y, receipt.plane,
            RC_GROUND_OWNER_LOCAL_PLAYER, ground);
        if (result != RC_GROUND_GRANT_OK)
            receipt.result = RC_LOOT_DELIVERY_FAILED;
    }
    world->last_loot = receipt;
    if (receipt.result == RC_LOOT_ROLL_UNVERIFIED) {
        const RcDropTable *table = rc_drop_table_for_npc(receipt.npc_def_id);
        const char *reason = table->rejection_flags == RC_DROP_UNPUBLISHED_RATE
            ? "unpublished drop rate" : table->rejection_flags == RC_DROP_OUTDATED_SOURCE
            ? "outdated drop source" : "unpublished drop rate and outdated drop source";
        fprintf(stderr, "npc loot rejected: uid=%d definition=%d (%s): %s; "
                        "see content/loot/rejections.txt; no loot granted or retry\n",
                receipt.npc_uid, receipt.npc_def_id, def->name, reason);
    } else if (receipt.result)
        fprintf(stderr, "npc loot failed: uid=%d definition=%d (%s) result=%d (%s); "
                        "no partial grant or retry (see world.last_loot)\n",
                receipt.npc_uid, receipt.npc_def_id, def->name, receipt.result,
                receipt.result == RC_LOOT_ROLL_NO_TABLE ? "NPC has no loaded drop table" :
                receipt.result == RC_LOOT_ROLL_CAPACITY ? "loot receipt capacity exceeded" :
                receipt.result == RC_LOOT_DELIVERY_FAILED ? "ground delivery rejected" :
                "invalid reward table or policy");
}
