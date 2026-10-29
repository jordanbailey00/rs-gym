#include "shops.h"
#include "interaction.h"
#include "io.h"
#include "items.h"
#include "player_command.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHOP_MAGIC 0x504F4853u
#define SHOP_VERSION 1u

RcShop *g_shops = NULL;
RcShopStock *g_shop_stock = NULL;
int g_shop_count = 0;
int g_shop_stock_count = 0;

static const RcShop *g_active_shops = NULL;
static const RcShopStock *g_active_shop_stock = NULL;
static int g_active_shop_count = 0;
static int g_active_shop_stock_count = 0;

void rc_shop_data_init(RcShopData *data) {
    if (!data) return;
    memset(data, 0, sizeof(*data));
}

void rc_shop_data_free(RcShopData *data) {
    if (!data) return;
    free(data->shops);
    free(data->stock);
    rc_shop_data_init(data);
}

static int read_str8(FILE *f, char *out, int cap,
                     const char *path, const char *what) {
    uint8_t len;
    if (!rc_read_exact(f, &len, sizeof(len), 1, path, what)) return 0;
    int keep = len < (uint8_t)(cap - 1) ? (int)len : cap - 1;
    if (keep && !rc_read_exact(f, out, 1, (size_t)keep, path, what)) return 0;
    out[keep] = '\0';
    if (len > (uint8_t)keep &&
            !rc_seek(f, (long)(len - (uint8_t)keep), SEEK_CUR, path, what)) {
        return 0;
    }
    return 1;
}

static int push_stock(RcShopStock **rows, int *count, int *cap,
                      RcShopStock row) {
    if (*count == *cap) {
        int next = *cap ? *cap * 2 : 4096;
        RcShopStock *grown = realloc(*rows, (size_t)next * sizeof(**rows));
        if (!grown) return 0;
        *rows = grown;
        *cap = next;
    }
    (*rows)[(*count)++] = row;
    return 1;
}

int rc_load_shops_into(const char *path, RcShopData *out) {
    if (!out) return -1;
    rc_shop_data_free(out);
    if (!path) return -1;
    FILE *f = rc_asset_fopen(path, "rb");
    if (!f) return -1;

    uint32_t magic, version, count;
    if (!rc_read_exact(f, &magic, sizeof(magic), 1, path, "magic")
            || !rc_read_exact(f, &version, sizeof(version), 1, path, "version")
            || !rc_read_exact(f, &count, sizeof(count), 1, path, "count")
            || magic != SHOP_MAGIC || version != SHOP_VERSION) {
        rc_asset_close(f);
        return -1;
    }

    RcShop *shops = calloc(count ? count : 1u, sizeof(*shops));
    RcShopStock *stock = NULL;
    int stock_count = 0, stock_cap = 0;
    if (!shops) {
        rc_asset_close(f);
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        RcShop *shop = &shops[i];
        if (!read_str8(f, shop->name, sizeof(shop->name), path, "name")
                || !read_str8(f, shop->owner, sizeof(shop->owner), path, "owner")
                || !read_str8(f, shop->location, sizeof(shop->location), path, "location")
                || !read_str8(f, shop->specialty, sizeof(shop->specialty), path, "specialty")
                || !rc_read_exact(f, &shop->members, sizeof(shop->members), 1,
                                  path, "members")
                || !rc_read_exact(f, &shop->stock_count, sizeof(shop->stock_count),
                                  1, path, "stock count")) {
            free(shops);
            free(stock);
            rc_asset_close(f);
            return -1;
        }
        shop->first_stock = (uint32_t)stock_count;
        for (int s = 0; s < shop->stock_count; s++) {
            RcShopStock row;
            uint32_t item_id;
            if (!rc_read_exact(f, &item_id, sizeof(item_id), 1, path, "item")
                    || !rc_read_exact(f, &row.buy_price, sizeof(row.buy_price),
                                      1, path, "buy")
                    || !rc_read_exact(f, &row.sell_price, sizeof(row.sell_price),
                                      1, path, "sell")
                    || !rc_read_exact(f, &row.base_stock, sizeof(row.base_stock),
                                      1, path, "stock")
                    || !rc_read_exact(f, &row.buy_mult, sizeof(row.buy_mult),
                                      1, path, "buy mult")
                    || !rc_read_exact(f, &row.sell_mult, sizeof(row.sell_mult),
                                      1, path, "sell mult")
                    || !rc_read_exact(f, &row.restock_ticks,
                                      sizeof(row.restock_ticks), 1, path,
                                      "restock")) {
                free(shops);
                free(stock);
                rc_asset_close(f);
                return -1;
            }
            row.item_id = (int)item_id;
            if (!push_stock(&stock, &stock_count, &stock_cap, row)) {
                free(shops);
                free(stock);
                rc_asset_close(f);
                return -1;
            }
        }
    }

    rc_asset_close(f);
    out->shops = shops;
    out->stock = stock;
    out->shop_count = (int)count;
    out->stock_count = stock_count;
    return out->shop_count;
}

int rc_shop_data_import_globals(RcShopData *out) {
    if (!out) return 0;
    rc_shop_data_free(out);
    if (g_shop_count <= 0) return 1;
    if (!g_shops) return 0;
    out->shops = malloc((size_t)g_shop_count * sizeof(*out->shops));
    if (!out->shops) return 0;
    memcpy(out->shops, g_shops, (size_t)g_shop_count * sizeof(*out->shops));
    out->shop_count = g_shop_count;
    out->stock_count = g_shop_stock_count;
    if (g_shop_stock_count > 0) {
        if (!g_shop_stock) {
            rc_shop_data_free(out);
            return 0;
        }
        out->stock = malloc((size_t)g_shop_stock_count * sizeof(*out->stock));
        if (!out->stock) {
            rc_shop_data_free(out);
            return 0;
        }
        memcpy(out->stock, g_shop_stock,
               (size_t)g_shop_stock_count * sizeof(*out->stock));
    }
    return 1;
}

int rc_shops_mirror_to_globals(const RcShopData *data) {
    RcShop *shops = NULL;
    RcShopStock *stock = NULL;
    int shop_count = data ? data->shop_count : 0;
    int stock_count = data ? data->stock_count : 0;
    if (shop_count > 0) {
        if (!data->shops) return 0;
        shops = malloc((size_t)shop_count * sizeof(*shops));
        if (!shops) return 0;
        memcpy(shops, data->shops, (size_t)shop_count * sizeof(*shops));
    }
    if (stock_count > 0) {
        if (!data->stock) {
            free(shops);
            return 0;
        }
        stock = malloc((size_t)stock_count * sizeof(*stock));
        if (!stock) {
            free(shops);
            return 0;
        }
        memcpy(stock, data->stock, (size_t)stock_count * sizeof(*stock));
    }
    free(g_shops);
    free(g_shop_stock);
    g_shops = shops;
    g_shop_stock = stock;
    g_shop_count = shop_count;
    g_shop_stock_count = stock_count;
    rc_shops_use_data(NULL);
    return 1;
}

void rc_shops_use_data(const RcShopData *data) {
    if (!data || data->shop_count <= 0) {
        g_active_shops = g_shops;
        g_active_shop_stock = g_shop_stock;
        g_active_shop_count = g_shop_count;
        g_active_shop_stock_count = g_shop_stock_count;
        return;
    }
    g_active_shops = data->shops;
    g_active_shop_stock = data->stock;
    g_active_shop_count = data->shop_count;
    g_active_shop_stock_count = data->stock_count;
}

void rc_shops_reset_data_if_active(const RcShopData *data) {
    if (!data) return;
    if (g_active_shops == data->shops || g_active_shop_stock == data->stock) {
        rc_shops_use_data(NULL);
    }
}

int rc_load_shops(const char *path) {
    RcShopData data;
    rc_shop_data_init(&data);
    int loaded = rc_load_shops_into(path, &data);
    if (loaded < 0) return -1;
    if (!rc_shops_mirror_to_globals(&data)) {
        rc_shop_data_free(&data);
        return -1;
    }
    rc_shop_data_free(&data);
    return g_shop_count;
}

const RcShop *rc_shop_get(int shop_idx) {
    if (shop_idx < 0 || shop_idx >= g_active_shop_count
            || !g_active_shops) return NULL;
    return &g_active_shops[shop_idx];
}

const RcShopStock *rc_shop_stock_rows(const RcShop *shop, int *count) {
    (void)g_active_shop_stock_count;
    if (!shop || !g_active_shop_stock) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = (int)shop->stock_count;
    return &g_active_shop_stock[shop->first_stock];
}

int rc_shop_find_by_name(const char *name) {
    if (!name || !g_active_shops) return -1;
    for (int i = 0; i < g_active_shop_count; i++) {
        if (strcmp(g_active_shops[i].name, name) == 0) return i;
    }
    return -1;
}

int rc_shop_has_item(int shop_idx, int item_id) {
    const RcShop *shop = rc_shop_get(shop_idx);
    int count = 0;
    const RcShopStock *rows = rc_shop_stock_rows(shop, &count);
    for (int i = 0; i < count; i++) {
        if (rows[i].item_id == item_id) return 1;
    }
    return 0;
}

// ---- Live shops ----------------------------------------------------------

#define COINS 995

int rc_shop_install(RcWorld *world, const RcShopDef *def) {
    if (!world || !def || world->shop_count >= RC_MAX_SHOPS
            || def->item_count > RC_SHOP_MAX_ITEMS)
        return -1;
    RcShopState *shop = &world->shops[world->shop_count];
    memset(shop, 0, sizeof(*shop));
    snprintf(shop->name, sizeof(shop->name), "%s", def->name ? def->name : "Shop");
    shop->npc_id = def->npc_id;
    shop->buy_mult = def->buy_mult;
    shop->sell_mult = def->sell_mult;
    shop->delta = def->delta;
    shop->general = def->general;
    for (int i = 0; i < def->item_count; i++) {
        shop->slots[i] = (RcShopSlot){
            .item_id = def->items[i].item_id, .base_stock = def->items[i].stock,
            .stock = def->items[i].stock,
            .restock_ticks = def->items[i].restock_ticks
                ? def->items[i].restock_ticks : 100,
        };
    }
    shop->slot_count = def->item_count;
    return world->shop_count++;
}

int rc_shop_for_npc(const RcWorld *world, int npc_id) {
    for (int i = 0; world && i < world->shop_count; i++)
        if (world->shops[i].npc_id == npc_id) return i;
    return -1;
}

static RcShopState *open_shop(RcWorld *world) {
    int i = world ? world->player->shop_open : -1;
    return i >= 0 && i < world->shop_count ? &world->shops[i] : NULL;
}

int rc_shop_open(RcWorld *world, int shop) {
    if (!world || shop < 0 || shop >= world->shop_count) return 0;
    world->player->shop_open = (int8_t)shop;
    return 1;
}

void rc_shop_close(RcPlayer *player) {
    if (player) player->shop_open = -1;
}

// Steps of stock away from base: one per item, or per tenth of base for big stacks.
static int stock_steps(const RcShopSlot *slot) {
    int unit = slot->base_stock > 10 ? slot->base_stock / 10 : 1;
    return (slot->stock - slot->base_stock) / unit;
}

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int item_value(int item_id) {
    const RcItemDef *def = rc_item_def_get(item_id);
    return def ? def->value : 0;
}

static int slot_buy_price(const RcShopState *shop, const RcShopSlot *slot) {
    int mult = clamp_int(shop->sell_mult - shop->delta * stock_steps(slot),
                         shop->sell_mult / 2, shop->sell_mult * 2);
    int price = (int)((int64_t)item_value(slot->item_id) * mult / 1000);
    return price > 0 ? price : 1;
}

static int slot_sell_price(const RcShopState *shop, const RcShopSlot *slot) {
    int mult = clamp_int(shop->buy_mult - shop->delta * stock_steps(slot),
                         100, 1000);
    return (int)((int64_t)item_value(slot->item_id) * mult / 1000);
}

static RcShopSlot *find_slot(RcShopState *shop, int item_id) {
    for (int i = 0; i < shop->slot_count; i++)
        if (shop->slots[i].item_id == item_id) return &shop->slots[i];
    return NULL;
}

int rc_shop_buy_price(const RcWorld *world, int shop, int slot) {
    if (!world || shop < 0 || shop >= world->shop_count) return -1;
    const RcShopState *s = &world->shops[shop];
    return slot >= 0 && slot < s->slot_count ? slot_buy_price(s, &s->slots[slot])
                                             : -1;
}

static int shop_accepts(const RcShopState *shop, int item_id) {
    const RcItemDef *def = rc_item_def_get(item_id);
    if (!def || item_id == COINS || !def->tradeable) return 0;
    for (int i = 0; i < shop->slot_count; i++)
        if (shop->slots[i].item_id == item_id) return 1;
    return shop->general;
}

int rc_shop_sell_price(const RcWorld *world, int shop, int item_id) {
    if (!world || shop < 0 || shop >= world->shop_count) return -1;
    const RcShopState *s = &world->shops[shop];
    if (!shop_accepts(s, item_id)) return -1;
    for (int i = 0; i < s->slot_count; i++)
        if (s->slots[i].item_id == item_id)
            return slot_sell_price(s, &s->slots[i]);
    RcShopSlot fresh = {.item_id = item_id};
    return slot_sell_price(s, &fresh);
}

static void start_restock(const RcWorld *world, RcShopSlot *slot) {
    if (slot->stock != slot->base_stock && !slot->next_restock)
        slot->next_restock = world->tick + slot->restock_ticks;
}

int rc_shop_buy(RcWorld *world, int slot_idx, int quantity) {
    if (rc_player_command_should_queue(world)) {
        RcShopState *shop = open_shop(world);
        if (!shop || slot_idx < 0 || slot_idx >= shop->slot_count) return 0;
        // The slot's item rides along: stock can shift slots before the tick.
        int args[8] = {slot_idx, quantity, shop->slots[slot_idx].item_id,
                       0, 0, 0, 0, 0};
        return rc_player_command_submit(world, RC_PLAYER_COMMAND_SHOP_BUY,
                                        RC_ACTION_CATEGORY_SOFT, args, 0);
    }
    return rc_shop_buy_expected(world, slot_idx, quantity, -1);
}

int rc_shop_buy_expected(RcWorld *world, int slot_idx, int quantity,
                         int expected_item) {
    RcShopState *shop = open_shop(world);
    if (!shop || slot_idx < 0 || slot_idx >= shop->slot_count || quantity <= 0
            || (expected_item >= 0
                && shop->slots[slot_idx].item_id != expected_item))
        return 0;
    RcShopSlot *slot = &shop->slots[slot_idx];
    RcPlayer *p = world->player;
    int bought = 0;
    while (bought < quantity) {
        if (slot->stock <= 0) {
            rc_interaction_publish_message(p, "The shop has run out of stock.");
            break;
        }
        int price = slot_buy_price(shop, slot);
        RcItemTransaction tx;
        RcItemActionResult r = rc_item_tx_begin(&tx, world);
        if (r.code == RC_ITEM_RESULT_OK) r = rc_item_tx_remove_item(&tx, COINS, price);
        if (r.code != RC_ITEM_RESULT_OK) {
            rc_interaction_publish_message(p, "You don't have enough coins.");
            break;
        }
        r = rc_item_tx_add(&tx, slot->item_id, 1, 0);
        if (r.code != RC_ITEM_RESULT_OK
                || rc_item_tx_commit(&tx).code != RC_ITEM_RESULT_OK) {
            rc_interaction_publish_message(p, "You don't have enough inventory space.");
            break;
        }
        slot->stock--;
        start_restock(world, slot);
        bought++;
    }
    return bought;
}

int rc_shop_sell(RcWorld *world, int inv_slot, int quantity) {
    if (rc_player_command_should_queue(world)) {
        if (!world || inv_slot < 0 || inv_slot >= RC_INVENTORY_SIZE) return 0;
        int args[8] = {inv_slot, (int)world->player->inventory[inv_slot].generation,
                       quantity, 0, 0, 0, 0, 0};
        return rc_player_command_submit(world, RC_PLAYER_COMMAND_SHOP_SELL,
                                        RC_ACTION_CATEGORY_SOFT, args, 0);
    }
    RcShopState *shop = open_shop(world);
    if (!shop || inv_slot < 0 || inv_slot >= RC_INVENTORY_SIZE || quantity <= 0)
        return 0;
    RcPlayer *p = world->player;
    int item_id = p->inventory[inv_slot].item_id;
    if (item_id < 0) return 0;
    if (!shop_accepts(shop, item_id)) {
        rc_interaction_publish_message(p, "You can't sell this item to this shop.");
        return 0;
    }
    RcShopSlot *slot = find_slot(shop, item_id);
    if (!slot) {
        if (shop->slot_count >= RC_SHOP_MAX_ITEMS) {
            rc_interaction_publish_message(p, "The shop is too full to buy that.");
            return 0;
        }
        slot = &shop->slots[shop->slot_count++];
        *slot = (RcShopSlot){.item_id = item_id, .restock_ticks = 100};
    }
    int sold = 0;
    while (sold < quantity) {
        int from = p->inventory[inv_slot].item_id == item_id
                 ? inv_slot : rc_inv_find(p->inventory, item_id);
        if (from < 0) break;
        int price = slot_sell_price(shop, slot);
        RcItemTransaction tx;
        RcItemActionResult r = rc_item_tx_begin(&tx, world);
        if (r.code == RC_ITEM_RESULT_OK)
            r = rc_item_tx_remove_slot(&tx, from, 1, p->inventory[from].generation);
        if (r.code == RC_ITEM_RESULT_OK && price > 0)
            r = rc_item_tx_add(&tx, COINS, price, 0);
        if (r.code != RC_ITEM_RESULT_OK
                || rc_item_tx_commit(&tx).code != RC_ITEM_RESULT_OK) {
            rc_interaction_publish_message(p, "You don't have enough inventory space.");
            break;
        }
        slot->stock++;
        start_restock(world, slot);
        sold++;
    }
    return sold;
}

void rc_shops_tick(RcWorld *world) {
    for (int s = 0; world && s < world->shop_count; s++) {
        RcShopState *shop = &world->shops[s];
        int w = 0;
        for (int i = 0; i < shop->slot_count; i++) {
            RcShopSlot *slot = &shop->slots[i];
            if (slot->next_restock && world->tick >= slot->next_restock) {
                // Big stacks restock a hundredth of base per step.
                int step = slot->base_stock > 100 ? slot->base_stock / 100 : 1;
                if (slot->stock < slot->base_stock)
                    slot->stock = slot->stock + step < slot->base_stock
                                ? slot->stock + step : slot->base_stock;
                else
                    slot->stock = slot->stock - step > slot->base_stock
                                ? slot->stock - step : slot->base_stock;
                slot->next_restock = 0;
                start_restock(world, slot);
            }
            // Items players sold that the shop does not stock leave at zero.
            if (slot->base_stock == 0 && slot->stock == 0) continue;
            shop->slots[w++] = *slot;
        }
        shop->slot_count = w;
    }
}
