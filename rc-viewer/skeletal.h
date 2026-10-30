/**
 * @fileoverview Skeletal ("maya") animation playback: loads a mode's skeletal.bin
 * (tools/cache_pipeline/export_skeletal.py) and skins model base vertices with it.
 *
 * Newer OSRS models weight each vertex to bones, and newer sequences animate
 * those bones. The export bakes, per sequence and client tick (20 ms), each used
 * bone's skin matrix in model space; a vertex is posed as the weighted sum of
 * v M over its bones (weights / 255), the way the client skins it.
 *
 * Each sequence is registered in the AnimCache as a stub of one-tick frames
 * (framebase = skeleton id, no transforms) pointing at its matrices, so the
 * viewer's frame timing, primary/pose selection and walk flags work unchanged;
 * posing checks AnimSequence.skel before the vertex-group path.
 *
 * Rigs are found by model entry id (an NPC definition in npcs.models, a model
 * id in projectiles.models) and base vertex count.
 */

#ifndef RC_VIEWER_SKELETAL_H
#define RC_VIEWER_SKELETAL_H

#include "../rc-core/io.h"
#include "anims.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKEL_MAGIC 0x314C4B53u /* bytes are "SKL1" */
#define SKEL_VERSION 1u

enum { SKEL_RIG_NPC = 0, SKEL_RIG_MODEL = 1 };

typedef struct {
    uint16_t slot;   /* bone slot in the rig's skeleton */
    uint16_t weight; /* 0..255 */
} SkelPair;

typedef struct SkelRig {
    uint8_t kind;
    uint32_t id;
    uint32_t skeleton;      /* index into SkelSet.skeleton_ids */
    int vertex_count;
    uint8_t *counts;        /* pairs per vertex */
    uint32_t *first;        /* first pair per vertex */
    SkelPair *pairs;
} SkelRig;

typedef struct SkelSequence {
    uint32_t id;
    uint32_t skeleton;
    int frame_count;
    int bone_count;
    int8_t walk_flag, run_flag;
    float *matrices;        /* [frame][slot][12]: linear rows, then translation */
} SkelSequence;

typedef struct SkelSet {
    uint32_t *skeleton_ids;
    int *skeleton_slots;    /* bone slots per skeleton */
    int skeleton_count;
    SkelRig *rigs;
    int rig_count;
    SkelSequence *sequences;
    int sequence_count;
} SkelSet;

static void skel_set_free(SkelSet *set) {
    if (!set) return;
    for (int i = 0; i < set->rig_count; i++) {
        free(set->rigs[i].counts);
        free(set->rigs[i].first);
        free(set->rigs[i].pairs);
    }
    for (int i = 0; i < set->sequence_count; i++)
        free(set->sequences[i].matrices);
    free(set->skeleton_ids);
    free(set->skeleton_slots);
    free(set->rigs);
    free(set->sequences);
    free(set);
}

static int skel_read(AnimReader *r, void *dst, size_t n) {
    if (!anim_reader_need(r, n)) return 0;
    memcpy(dst, r->p, n);
    r->p += n;
    return 1;
}

static void skel_align4(AnimReader *r, const uint8_t *start) {
    size_t pad = (size_t)(-(r->p - start) & 3);
    anim_skip(r, pad);
}

// The mode's skeletal animation, or NULL when the file is missing (no skeletal
// content) or invalid (reported).
static SkelSet *skel_set_load(const char *path) {
    FILE *f = rc_asset_fopen(path, "rb");
    if (!f) return NULL;
    long size = rc_file_size(f, path, "skeletal animation");
    uint8_t *buf = size > 0 ? (uint8_t *)malloc((size_t)size) : NULL;
    if (!buf || !rc_read_exact(f, buf, 1, (size_t)size, path, "skeletal animation bytes")) {
        free(buf);
        rc_asset_close(f);
        return NULL;
    }
    rc_asset_close(f);
    AnimReader r = {buf, buf + size, path, 1};
    SkelSet *set = (SkelSet *)calloc(1, sizeof(SkelSet));
    uint32_t magic = anim_read_u32(&r), version = anim_read_u32(&r);
    uint32_t skeletons = anim_read_u32(&r), rigs = anim_read_u32(&r);
    uint32_t sequences = anim_read_u32(&r);
    if (!set || !r.ok || magic != SKEL_MAGIC || version != SKEL_VERSION
            || skeletons > 4096 || rigs > 4096 || sequences > 65535) {
        fprintf(stderr, "skeletal: bad header in %s\n", path);
        free(buf);
        skel_set_free(set);
        return NULL;
    }
    set->skeleton_ids = (uint32_t *)calloc(skeletons ? skeletons : 1, sizeof(uint32_t));
    set->skeleton_slots = (int *)calloc(skeletons ? skeletons : 1, sizeof(int));
    set->rigs = (SkelRig *)calloc(rigs ? rigs : 1, sizeof(SkelRig));
    set->sequences = (SkelSequence *)calloc(sequences ? sequences : 1, sizeof(SkelSequence));
    if (!set->skeleton_ids || !set->skeleton_slots || !set->rigs || !set->sequences)
        r.ok = 0;
    for (uint32_t i = 0; r.ok && i < skeletons; i++) {
        set->skeleton_ids[i] = anim_read_u32(&r);
        set->skeleton_slots[i] = (int)anim_read_u32(&r);
        anim_skip(&r, (size_t)set->skeleton_slots[i] * 2u);   /* bone ids, for tools */
        skel_align4(&r, buf);
        set->skeleton_count++;
    }
    for (uint32_t i = 0; r.ok && i < rigs; i++) {
        SkelRig *rig = &set->rigs[i];
        rig->kind = anim_read_u8(&r);
        anim_skip(&r, 3);
        rig->id = anim_read_u32(&r);
        rig->skeleton = anim_read_u32(&r);
        uint32_t verts = anim_read_u32(&r), pair_count = anim_read_u32(&r);
        if (!r.ok || rig->skeleton >= skeletons || verts > 65535 || pair_count > verts * 16u) {
            r.ok = 0;
            break;
        }
        rig->vertex_count = (int)verts;
        rig->counts = (uint8_t *)malloc(verts ? verts : 1);
        rig->first = (uint32_t *)malloc((verts ? verts : 1) * sizeof(uint32_t));
        rig->pairs = (SkelPair *)malloc((pair_count ? pair_count : 1) * sizeof(SkelPair));
        set->rig_count++;
        if (!rig->counts || !rig->first || !rig->pairs || !skel_read(&r, rig->counts, verts)) {
            r.ok = 0;
            break;
        }
        skel_align4(&r, buf);
        uint32_t next = 0;
        for (uint32_t v = 0; v < verts; v++) {
            rig->first[v] = next;
            next += rig->counts[v];
        }
        if (next != pair_count || !skel_read(&r, rig->pairs, pair_count * sizeof(SkelPair))) {
            r.ok = 0;
            break;
        }
        int slots = set->skeleton_slots[rig->skeleton];
        for (uint32_t p = 0; p < pair_count; p++)
            if (rig->pairs[p].slot >= slots) r.ok = 0;
    }
    for (uint32_t i = 0; r.ok && i < sequences; i++) {
        SkelSequence *seq = &set->sequences[i];
        seq->id = anim_read_u32(&r);
        seq->skeleton = anim_read_u32(&r);
        uint32_t frames = anim_read_u32(&r);
        seq->walk_flag = anim_read_i8(&r);
        seq->run_flag = anim_read_i8(&r);
        anim_skip(&r, 2);
        if (!r.ok || seq->skeleton >= skeletons || seq->id > 0xFFFF
                || frames == 0 || frames > 0xFFFF) {
            r.ok = 0;
            break;
        }
        seq->frame_count = (int)frames;
        seq->bone_count = set->skeleton_slots[seq->skeleton];
        size_t floats = (size_t)frames * (size_t)seq->bone_count * 12u;
        seq->matrices = (float *)malloc((floats ? floats : 1) * sizeof(float));
        set->sequence_count++;
        if (!seq->matrices || !skel_read(&r, seq->matrices, floats * sizeof(float))) {
            r.ok = 0;
            break;
        }
    }
    if (!r.ok || r.p != r.end) {
        fprintf(stderr, "skeletal: invalid %s\n", path);
        free(buf);
        skel_set_free(set);
        return NULL;
    }
    free(buf);
    fprintf(stderr, "skeletal: %d rigs, %d sequences from %s\n",
            set->rig_count, set->sequence_count, path);
    return set;
}

// Adds each skeletal sequence to `cache` as a stub of one-tick frames
// (replacing an entry of the same id) and keeps `set` for posing. 0 on
// allocation failure.
static int skel_register(AnimCache *cache, SkelSet *set) {
    if (!cache || !set) return 1;
    int added = 0;
    for (int i = 0; i < set->sequence_count; i++)
        if (!anim_get_sequence(cache, (uint16_t)set->sequences[i].id)) added++;
    if (added) {
        AnimSequence *grown = (AnimSequence *)realloc(
            cache->sequences, (size_t)(cache->seq_count + added) * sizeof(AnimSequence));
        if (!grown) return 0;
        cache->sequences = grown;
    }
    int count = cache->seq_count;
    for (int i = 0; i < set->sequence_count; i++) {
        const SkelSequence *skel = &set->sequences[i];
        AnimSequence *seq = anim_get_sequence(cache, (uint16_t)skel->id);
        if (seq) {
            for (int f = 0; f < seq->frame_count; f++) free(seq->frames[f].frame.transforms);
            free(seq->frames);
            free(seq->interleave_order);
        } else {
            seq = &cache->sequences[count++];   /* past the sorted range: not searched yet */
        }
        memset(seq, 0, sizeof(*seq));
        seq->seq_id = (uint16_t)skel->id;
        seq->frame_count = (uint16_t)skel->frame_count;
        seq->walk_flag = skel->walk_flag;
        seq->skel = skel;
        seq->frames = (AnimSequenceFrame *)calloc((size_t)skel->frame_count,
                                                  sizeof(AnimSequenceFrame));
        if (!seq->frames) return 0;
        for (int f = 0; f < skel->frame_count; f++) {
            seq->frames[f].delay = 1;
            seq->frames[f].frame.framebase_id = (uint16_t)set->skeleton_ids[skel->skeleton];
        }
    }
    cache->seq_count = count;
    qsort(cache->sequences, (size_t)cache->seq_count, sizeof(AnimSequence),
          anim_compare_sequence);
    cache->skeletal = set;
    return 1;
}

static const SkelRig *skel_rig_find(const AnimCache *cache, uint32_t id,
                                    int vertex_count) {
    const SkelSet *set = cache ? cache->skeletal : NULL;
    for (int i = 0; set && i < set->rig_count; i++)
        if (set->rigs[i].id == id && set->rigs[i].vertex_count == vertex_count)
            return &set->rigs[i];
    return NULL;
}

// Poses `state` with frame `frame` of a skeletal sequence; 0 when the rig is
// for another skeleton.
static int skel_apply_frame(AnimModelState *state, const int16_t *base_verts,
                            const SkelRig *rig, const SkelSequence *seq,
                            int frame) {
    if (!state || !base_verts || !rig || !seq || rig->skeleton != seq->skeleton
            || rig->vertex_count != state->vert_count)
        return 0;
    if (frame < 0) frame = 0;
    if (frame >= seq->frame_count) frame = seq->frame_count - 1;
    const float *m = seq->matrices + (size_t)frame * (size_t)seq->bone_count * 12u;
    anim_reset_face_alphas(state);
    for (int v = 0; v < rig->vertex_count; v++) {
        float x = base_verts[v * 3], y = base_verts[v * 3 + 1], z = base_verts[v * 3 + 2];
        int n = rig->counts[v];
        if (n == 0) {
            memcpy(&state->verts[v * 3], &base_verts[v * 3], 3 * sizeof(int16_t));
            continue;
        }
        float ox = 0.0f, oy = 0.0f, oz = 0.0f;
        for (int k = 0; k < n; k++) {
            const SkelPair *p = &rig->pairs[rig->first[v] + (uint32_t)k];
            const float *b = m + (size_t)p->slot * 12u;
            float w = (float)p->weight / 255.0f;
            ox += w * (x * b[0] + y * b[3] + z * b[6] + b[9]);
            oy += w * (x * b[1] + y * b[4] + z * b[7] + b[10]);
            oz += w * (x * b[2] + y * b[5] + z * b[8] + b[11]);
        }
        state->verts[v * 3] = (int16_t)lrintf(ox);
        state->verts[v * 3 + 1] = (int16_t)lrintf(oy);
        state->verts[v * 3 + 2] = (int16_t)lrintf(oz);
    }
    return 1;
}

#endif
