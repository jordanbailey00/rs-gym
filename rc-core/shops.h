#ifndef RC_SHOPS_H
#define RC_SHOPS_H

#include "types.h"

#include <stdint.h>

typedef struct {
    char name[64];
    char owner[64];
    char location[64];
    char specialty[64];
    uint32_t first_stock;
    uint16_t stock_count;
    uint8_t members;
} RcShop;

typedef struct {
    int item_id;
    uint32_t buy_price;
    uint32_t sell_price;
    uint16_t base_stock;
    uint16_t buy_mult;
    uint16_t sell_mult;
    uint16_t restock_ticks;
} RcShopStock;

typedef struct {
    RcShop *shops;
    RcShopStock *stock;
    int shop_count;
    int stock_count;
} RcShopData;

extern RcShop *g_shops;
extern RcShopStock *g_shop_stock;
extern int g_shop_count;
extern int g_shop_stock_count;

int rc_load_shops(const char *path);
void rc_shop_data_init(RcShopData *data);
void rc_shop_data_free(RcShopData *data);
int rc_load_shops_into(const char *path, RcShopData *out);
int rc_shop_data_import_globals(RcShopData *out);
int rc_shops_mirror_to_globals(const RcShopData *data);
void rc_shops_use_data(const RcShopData *data);
void rc_shops_reset_data_if_active(const RcShopData *data);
const RcShop *rc_shop_get(int shop_idx);
const RcShopStock *rc_shop_stock_rows(const RcShop *shop, int *count);
int rc_shop_find_by_name(const char *name);
int rc_shop_has_item(int shop_idx, int item_id);

// ---- Live shops (gym) ----------------------------------------------------
// Content installs each shop with its stock; players trade with the shop of
// the shopkeeper they opened. Prices follow OSRS: a shop sells at sell_mult
// and buys at buy_mult (per mille of item value) at base stock, moving by
// `delta` per step of stock away from base (a step is one item, or a tenth of
// base stock for large stacks). Stock drifts back toward base over time.

typedef struct {
    int item_id, stock;
    uint16_t restock_ticks;
} RcShopItemDef;

typedef struct {
    const char *name;
    int npc_id;
    uint16_t buy_mult, sell_mult, delta;
    bool general;
    const RcShopItemDef *items;
    int item_count;
} RcShopDef;

int  rc_shop_install(RcWorld *world, const RcShopDef *def);
int  rc_shop_for_npc(const RcWorld *world, int npc_id);
// Acting-player calls. Buy/sell queue as commands outside a tick and return
// the number of items traded when run.
int  rc_shop_open(RcWorld *world, int shop);
void rc_shop_close(RcPlayer *player);
int  rc_shop_buy(RcWorld *world, int slot, int quantity);
int  rc_shop_buy_expected(RcWorld *world, int slot, int quantity, int expected_item);
int  rc_shop_sell(RcWorld *world, int inv_slot, int quantity);
// Coins the player pays for one item from `slot` / receives for one item_id
// (-1 when the shop will not trade it).
int  rc_shop_buy_price(const RcWorld *world, int shop, int slot);
int  rc_shop_sell_price(const RcWorld *world, int shop, int item_id);
void rc_shops_tick(RcWorld *world);

#endif
