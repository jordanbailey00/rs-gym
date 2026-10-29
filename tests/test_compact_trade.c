// Direct trades in the compact mode (run tools/build_mode.py first): a swap commits atomically and
// conserves every item; an offer change voids both acceptances; a stale accept, eating, dropping,
// death, a full inventory and separation never duplicate or lose anything; a busy partner refuses
// a third player; untradeable items cannot be offered. Item setup is test setup; trade calls are the
// players' commands.
#include "../rc-core/api.h"
#include "../rc-core/items.h"
#include "../rc-core/players.h"
#include "../rc-core/trade.h"
#include "../rc-content/content.h"
#include "gym_test_config.h"

#include <string.h>

enum { SKIP = 77, COINS = 995, LOBSTER = 379, DAGGER = 1205, SPINE = 28798, LOGS = 1511 };

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static RcWorld *w;

static void tick(int n) {
    for (int i = 0; i < n; i++) rc_world_tick(w);
}

static RcPlayer *as(int slot) {
    rc_world_select_player(w, slot);
    return w->player;
}

// Every unit of `item` anywhere: inventories, equipment, banks, trade offers and the ground.
static long total(int item) {
    long n = 0;
    for (int p = 0; p < w->player_count; p++) {
        const RcPlayer *pl = &w->players[p];
        for (int i = 0; i < RC_INVENTORY_SIZE; i++) n += pl->inventory[i].item_id == item ? pl->inventory[i].quantity : 0;
        for (int i = 0; i < RC_EQUIP_COUNT; i++) n += pl->equipment[i].item_id == item ? pl->equipment[i].quantity : 0;
        for (int i = 0; i < RC_BANK_SIZE; i++) n += pl->bank[i].item_id == item ? pl->bank[i].quantity : 0;
        for (int i = 0; i < pl->trade.offer_count; i++) n += pl->trade.offer[i].item_id == item ? pl->trade.offer[i].quantity : 0;
    }
    for (int i = 0; i < w->ground_item_count; i++)
        n += w->ground_items[i].active && w->ground_items[i].item_id == item ? w->ground_items[i].quantity : 0;
    return n;
}

static int count(int slot, int item) {
    int n = 0;
    for (int i = 0; i < RC_INVENTORY_SIZE; i++)
        n += w->players[slot].inventory[i].item_id == item ? w->players[slot].inventory[i].quantity : 0;
    return n;
}

static void give(int slot, int item, int qty) {  // test setup
    as(slot);
    const RcItemDef *d = rc_item_def_get(item);
    if (d && d->stackable) (void)rc_player_inventory_add(w, item, qty, 0);
    else for (int i = 0; i < qty; i++) (void)rc_player_inventory_add(w, item, 1, 0);
}

static void reset_players(void) {
    for (int p = 0; p < 3; p++) {
        RcPlayer *pl = as(p);
        rc_content_compact_new_character(w);
        for (int i = 0; i < RC_INVENTORY_SIZE; i++) pl->inventory[i] = (RcInvSlot){.item_id = -1};
        CHECK(rc_world_relocate_player(w, GYM_START_X + p, GYM_START_Y, 0), "place %d", p);
    }
}

static void open_trade(int a, int b) {
    as(a);
    CHECK(rc_trade_request(w, b), "request %d->%d", a, b);
    as(b);
    CHECK(rc_trade_request(w, a), "request %d->%d", b, a);
    tick(1);
    CHECK(w->players[a].trade.stage == RC_TRADE_STAGE_OPEN && w->players[b].trade.stage == RC_TRADE_STAGE_OPEN,
          "trade %d-%d not open", a, b);
}

static void offer(int p, int item, int qty) {
    as(p);
    CHECK(rc_trade_offer(w, rc_inv_find(w->players[p].inventory, item), qty), "offer %d", item);
}

static void accept(int p) {
    as(p);
    (void)rc_trade_accept(w, w->players[p].trade.revision);
}

static void check_swap(void) {
    reset_players();
    give(0, COINS, 50);
    give(0, DAGGER, 1);
    give(1, LOBSTER, 2);
    long coins = total(COINS), lobsters = total(LOBSTER), daggers = total(DAGGER);
    open_trade(0, 1);
    offer(0, COINS, 50);
    offer(0, DAGGER, 1);
    offer(1, LOBSTER, 2);
    tick(1);
    CHECK(count(0, COINS) == 0 && count(1, LOBSTER) == 0, "offered items still in the inventories");
    accept(0);
    tick(1);
    CHECK(w->players[0].trade.accepted_revision && w->players[1].trade.stage == RC_TRADE_STAGE_OPEN,
          "one acceptance should not commit");
    // A change voids both acceptances.
    give(1, LOGS, 1);
    offer(1, LOGS, 1);
    tick(1);
    CHECK(!w->players[0].trade.accepted_revision, "an offer change kept the old acceptance");
    as(1);
    uint32_t stale = w->players[1].trade.revision - 1;
    (void)rc_trade_accept(w, stale);
    tick(1);
    CHECK(w->players[1].trade.last_result == RC_TRADE_STALE, "a stale accept was taken");
    accept(1);
    accept(0);
    uint32_t done = rc_trade_completed_count(w);
    tick(1);
    CHECK(rc_trade_completed_count(w) == done + 1, "both accepted the same revision but no commit");
    CHECK(count(1, COINS) == 50 && count(1, DAGGER) == 1 && count(0, LOBSTER) == 2 && count(0, LOGS) == 1,
          "swap result: p1 coins %d dagger %d, p0 lobsters %d logs %d", count(1, COINS),
          count(1, DAGGER), count(0, LOBSTER), count(0, LOGS));
    CHECK(total(COINS) == coins && total(LOBSTER) == lobsters && total(DAGGER) == daggers,
          "a trade changed totals");
}

static void check_interruptions(void) {
    // Eating while a lobster is offered cannot eat the offered one.
    reset_players();
    give(0, LOBSTER, 1);
    give(1, COINS, 10);
    open_trade(0, 1);
    offer(0, LOBSTER, 1);
    tick(1);
    as(0);
    w->players[0].current_hp = 50;
    (void)rc_player_eat(w, 0);
    tick(1);
    CHECK(total(LOBSTER) == 1, "eating consumed an offered lobster (%ld left)", total(LOBSTER));
    // Dropping anything is another action: it cancels the trade and returns the offer.
    give(0, LOGS, 1);
    as(0);
    (void)rc_player_drop_item(w, rc_inv_find(w->players[0].inventory, LOGS));
    tick(1);
    CHECK(w->players[0].trade.stage == RC_TRADE_STAGE_NONE && w->players[1].trade.stage == RC_TRADE_STAGE_NONE,
          "dropping did not cancel the trade");
    CHECK(count(0, LOBSTER) == 1, "the offered lobster did not come back");

    // Death mid-trade: the offer comes back first, then goes to the recovery stack; nothing doubles.
    reset_players();
    give(0, DAGGER, 1);
    give(1, COINS, 30);
    long daggers = total(DAGGER), coins = total(COINS);
    open_trade(0, 1);
    offer(0, DAGGER, 1);
    offer(1, COINS, 30);
    tick(1);
    w->players[0].current_hp = 0;  // test setup: a lethal hit
    tick(1);
    CHECK(w->players[0].is_dead && w->players[1].trade.stage == RC_TRADE_STAGE_NONE, "death kept the trade");
    CHECK(total(DAGGER) == daggers && total(COINS) == coins && count(1, COINS) == 30,
          "death mid-trade: daggers %ld coins %ld", total(DAGGER), total(COINS));
    tick(12);

    // No room: the trade cancels and both offers return.
    reset_players();
    give(1, LOGS, 28);
    give(0, COINS, 5);
    give(0, LOBSTER, 1);
    open_trade(0, 1);
    offer(0, LOBSTER, 1);
    tick(1);
    accept(0);
    accept(1);
    tick(1);
    CHECK(w->players[0].trade.stage == RC_TRADE_STAGE_NONE && count(0, LOBSTER) == 1 && count(1, LOGS) == 28,
          "no-room trade: stage %d lobster %d logs %d", w->players[0].trade.stage, count(0, LOBSTER),
          count(1, LOGS));

    // Separation cancels; a busy partner refuses a third player; untradeables stay put.
    reset_players();
    give(0, SPINE, 1);
    give(0, COINS, 5);
    open_trade(0, 1);
    as(2);
    CHECK(!rc_trade_request(w, 0) || (tick(1), w->players[2].trade.last_result == RC_TRADE_BUSY),
          "a busy player accepted a third request");
    offer(0, SPINE, 1);
    tick(1);
    CHECK(w->players[0].trade.offer_count == 0 && w->players[0].trade.last_result == RC_TRADE_UNTRADEABLE,
          "an untradeable item was offered");
    offer(0, COINS, 5);
    tick(1);
    as(1);
    CHECK(rc_world_relocate_player(w, GYM_START_X + 30, GYM_START_Y, 0), "move apart");  // test setup
    tick(1);
    CHECK(w->players[0].trade.stage == RC_TRADE_STAGE_NONE && count(0, COINS) == 5,
          "separation: stage %d coins %d", w->players[0].trade.stage, count(0, COINS));
}

int main(void) {
    if (!gym_mode_built("compact_osrs_v1")) {
        fprintf(stderr, "mode not built; run tools/build_mode.py compact_osrs_v1\n");
        return SKIP;
    }
    RcWorldConfig cfg = gym_test_config_mode("compact_osrs_v1", 3);
    w = rc_world_create_config(&cfg);
    if (!w || !rc_world_install_content(w, rc_content_register_all)) {
        fprintf(stderr, "FAIL: world creation\n");
        return 1;
    }
    check_swap();
    check_interruptions();
    rc_world_destroy(w);
    if (failures) return 1;
    printf("compact trade: PASS\n");
    return 0;
}
