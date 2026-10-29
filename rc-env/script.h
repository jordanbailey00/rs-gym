#ifndef RC_SCRIPT_H
#define RC_SCRIPT_H

// Scripted controllers: each agent's plan is ordinary blocking C that acts only through the player
// commands a human uses (walk, interact, use items, bank, shop, make, fight, eat, equip, pray) and
// waits on the world clock. Agents run as coroutines over one world: every tick each agent with
// work due runs in index order until it waits, then the world ticks once. No helper writes game
// state directly, reads hidden RNG, or teleports; scripts may know the reference map.

#include "../rc-core/types.h"

#include <stdio.h>
#include <ucontext.h>

#define SC_STACK_SIZE (512 * 1024)

typedef struct ScRun ScRun;
typedef struct ScAgent ScAgent;
typedef void (*ScPlan)(ScAgent *agent, void *arg);

struct ScAgent {
    ScRun *run;
    int index;                 // the agent's player
    ScPlan plan;
    void *arg;
    ucontext_t ctx;
    void *stack;
    int started, done, failed;
    RcTick wake_tick;
    char failure[192];
    int deaths;                // counted by the scheduler
    int was_dead;
};

struct ScRun {
    RcWorld *world;
    int count;
    ScAgent agents[RC_MAX_PLAYERS];
    ucontext_t main;
    int current;
    FILE *log;                 // optional event log (one line per noted event)
};

void sc_init(ScRun *run, RcWorld *world, int agents, FILE *log);
void sc_start(ScRun *run, int agent, ScPlan plan, void *arg);
// Runs until every plan finishes or the world reaches tick `until`; returns the ticks run.
int  sc_run(ScRun *run, RcTick until);
void sc_free(ScRun *run);

// ---- Inside a plan -------------------------------------------------------------------------------

RcWorld  *sc_world(ScAgent *a);
RcPlayer *sc_player(ScAgent *a);   // selects the agent's player
void sc_wait(ScAgent *a, int ticks);
void sc_fail(ScAgent *a, const char *fmt, ...);  // ends the plan
void sc_note(ScAgent *a, const char *fmt, ...);

// Movement within one area (links are locs: use sc_use_loc on them).
int sc_walk(ScAgent *a, int x, int y, int tolerance);
int sc_near(ScAgent *a, int x, int y, int tolerance);

// Locs: the nearest current placement of any of `ids` within `radius` tiles.
int sc_find_loc(ScAgent *a, const int *ids, int n, int radius, int *x, int *y, int *id);
int sc_loc_option(ScAgent *a, int id, int x, int y, const char *label);
int sc_use_loc(ScAgent *a, int id, int x, int y, const char *label);
int sc_use_item_on_loc(ScAgent *a, int item, int id, int x, int y);

// NPCs: the nearest living NPC of a definition, optionally one nobody else is fighting.
const RcNpc *sc_find_npc(ScAgent *a, int def_id, int radius, int unengaged);
int sc_talk(ScAgent *a, const RcNpc *npc, const char *option);

// Inventory.
int sc_count(ScAgent *a, int item);
int sc_slot(ScAgent *a, int item);
int sc_free_slots(ScAgent *a);
int sc_equip(ScAgent *a, int item);
int sc_eat(ScAgent *a);                       // eats the first known food; 0 without food
int sc_bury_all(ScAgent *a);
int sc_drop_all(ScAgent *a, int item);
int sc_pickup(ScAgent *a, const int *items, int n, int radius);

// Bank (the nearest booth).
int sc_bank_open(ScAgent *a);
int sc_bank_count(ScAgent *a, int item);
int sc_withdraw(ScAgent *a, int item, int qty);
int sc_deposit(ScAgent *a, int item, int qty);
int sc_deposit_all_except(ScAgent *a, const int *keep, int n);
void sc_bank_close(ScAgent *a);

// Shops: a shopkeeper's Trade window.
int sc_shop_open(ScAgent *a, int npc_def);
int sc_buy(ScAgent *a, int item, int qty);
int sc_sell(ScAgent *a, int item, int qty);
void sc_shop_close(ScAgent *a);

// Production: with a station menu open, make `count` of the option that outputs `item` (0: all).
int sc_make(ScAgent *a, int item, int count);
int sc_use_items(ScAgent *a, int item_a, int item_b);  // item on item (opens a menu)

// Gathering and fishing until the inventory holds `want` of the product or is full.
int sc_gather(ScAgent *a, const int *locs, int n, const char *option, int product, int want,
              int radius);
int sc_fish(ScAgent *a, int spot_def, const char *option, int product, int want, int radius);

// One kill: attack the nearest unengaged `def_id`, eating below `eat_below` hitpoints (tenths).
// Returns 1 after the kill, 0 when no target was found, -1 when the player died.
int sc_kill(ScAgent *a, int def_id, int radius, int eat_below);

#endif
