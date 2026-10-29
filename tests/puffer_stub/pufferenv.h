/* Stand-in for PufferLib 5.0's src/pufferenv.h, enough to build and test
 * rc-env/puffer/binding.c without a PufferLib checkout: the Agent buffers,
 * the configuration Dict and the puf_* declarations, as the binding sees them. */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char key[128];
    char *str;
    double value;
} DictItem;

typedef struct {
    DictItem items[32];
    int size;
} Dict;

static inline DictItem *dict_find(Dict *dict, const char *key) {
    for (int i = 0; dict && i < dict->size; i++)
        if (strcmp(dict->items[i].key, key) == 0) return &dict->items[i];
    return NULL;
}

static inline void dict_set(Dict *dict, const char *key, double value) {
    DictItem *item = dict_find(dict, key);
    if (!item && dict->size < 32) {
        item = &dict->items[dict->size++];
        memset(item, 0, sizeof(*item));
        snprintf(item->key, sizeof(item->key), "%s", key);
    }
    if (item) item->value = value;
}

typedef struct Env Env;
typedef struct Log Log;

typedef struct Agent {
    obs_t *observations;
    float *actions;
    float *rewards;
    float *terminals;
    unsigned char *action_mask;
    int policy;
} Agent;

void puf_init(Env *env, Dict *kwargs);
void puf_reset(Env *env);
void puf_step(Env *env);
void puf_render(Env *env);
void puf_close(Env *env);
void puf_log(Log *log, Dict *out);
