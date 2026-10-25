// Loads terrain mesh from .terrain binary into raylib Model.
// Binary: TERR magic, vertex_count, region_count, min_world_x/y, vertices, colors, heightmap
// RuneC raylib terrain loader.

#ifndef RC_TERRAIN_H
#define RC_TERRAIN_H

#include "../rc-core/io.h"
#include "streaming.h"
#include "raylib.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define TERR_MAGIC 0x54455252

typedef struct {
    Model model;
    Mesh cpu_mesh;
    int vertex_count;
    double cpu_decode_ms;
    double gpu_upload_ms;
    int min_world_x, min_world_y;
    int loaded;
    float *heightmap;
    int hm_min_x, hm_min_y, hm_width, hm_height;
} TerrainMesh;

static void terrain_free(TerrainMesh *tm);

static size_t terrain_upload_bytes(const TerrainMesh *tm) {
    return tm && tm->vertex_count > 0
        ? (size_t)tm->vertex_count
            * (3u * sizeof(float) + 3u * sizeof(float)
               + 4u * sizeof(unsigned char))
        : 0;
}

static TerrainMesh *terrain_load_cpu(const char *path) {
    double load_started_ms = viewer_streaming_now_ms();
    FILE *f = rc_asset_fopen(path, "rb");
    if (!f) { fprintf(stderr, "terrain_load: can't open %s\n", path); return NULL; }

    uint32_t magic, vert_count, region_count;
    int32_t min_wx, min_wy;
    if (!rc_read_exact(f, &magic, sizeof(magic), 1, path, "terrain magic")
            || magic != TERR_MAGIC) {
        fprintf(stderr, "terrain: bad magic\n");
        rc_asset_close(f);
        return NULL;
    }
    if (!rc_read_exact(f, &vert_count, sizeof(vert_count), 1, path, "terrain vertex count")
            || !rc_read_exact(f, &region_count, sizeof(region_count), 1, path, "terrain region count")
            || !rc_read_exact(f, &min_wx, sizeof(min_wx), 1, path, "terrain min world x")
            || !rc_read_exact(f, &min_wy, sizeof(min_wy), 1, path, "terrain min world y")) {
        rc_asset_close(f);
        return NULL;
    }
    fprintf(stderr, "terrain: %u verts, %u regions, origin (%d,%d)\n",
            vert_count, region_count, min_wx, min_wy);

    float *raw_verts = vert_count > 0
        ? malloc(vert_count * 3 * sizeof(float)) : NULL;
    if (vert_count > 0 && (!raw_verts
            || !rc_read_exact(f, raw_verts, sizeof(float), vert_count * 3,
                              path, "terrain vertices"))) {
        free(raw_verts);
        rc_asset_close(f);
        return NULL;
    }

    unsigned char *raw_colors = vert_count > 0
        ? malloc(vert_count * 4) : NULL;
    if (vert_count > 0 && (!raw_colors
            || !rc_read_exact(f, raw_colors, sizeof(unsigned char),
                              vert_count * 4, path, "terrain colors"))) {
        free(raw_verts);
        free(raw_colors);
        rc_asset_close(f);
        return NULL;
    }

    Mesh mesh = {0};
    mesh.vertexCount = (int)vert_count;
    mesh.triangleCount = (int)(vert_count / 3);
    mesh.vertices = raw_verts;
    mesh.colors = raw_colors;
    mesh.normals = vert_count > 0
        ? calloc(vert_count * 3, sizeof(float)) : NULL;

    for (int i = 0; i < mesh.triangleCount; i++) {
        int b = i * 9;
        float e1x = raw_verts[b+3]-raw_verts[b], e1y = raw_verts[b+4]-raw_verts[b+1], e1z = raw_verts[b+5]-raw_verts[b+2];
        float e2x = raw_verts[b+6]-raw_verts[b], e2y = raw_verts[b+7]-raw_verts[b+1], e2z = raw_verts[b+8]-raw_verts[b+2];
        float nx = e1y*e2z - e1z*e2y, ny = e1z*e2x - e1x*e2z, nz = e1x*e2y - e1y*e2x;
        float len = sqrtf(nx*nx + ny*ny + nz*nz);
        if (len > 1e-4f) { nx /= len; ny /= len; nz /= len; }
        for (int v = 0; v < 3; v++) {
            mesh.normals[i*9+v*3] = nx; mesh.normals[i*9+v*3+1] = ny; mesh.normals[i*9+v*3+2] = nz;
        }
    }
    TerrainMesh *tm = calloc(1, sizeof(TerrainMesh));
    if (!tm || (vert_count > 0 && !mesh.normals)) {
        free(mesh.vertices);
        free(mesh.colors);
        free(mesh.normals);
        free(tm);
        rc_asset_close(f);
        return NULL;
    }
    tm->cpu_mesh = mesh;
    tm->vertex_count = (int)vert_count;
    tm->min_world_x = min_wx;
    tm->min_world_y = min_wy;

    // Heightmap (appended after colors)
    int32_t hx, hy; uint32_t hw, hh;
    if (rc_read_exact(f, &hx, sizeof(hx), 1, path, "terrain heightmap min x")
            && rc_read_exact(f, &hy, sizeof(hy), 1, path, "terrain heightmap min y")
            && rc_read_exact(f, &hw, sizeof(hw), 1, path, "terrain heightmap width")
            && rc_read_exact(f, &hh, sizeof(hh), 1, path, "terrain heightmap height")
            && hw > 0 && hh > 0 && hw <= 8192 && hh <= 8192) {
        tm->hm_min_x = hx; tm->hm_min_y = hy;
        tm->hm_width = (int)hw; tm->hm_height = (int)hh;
        tm->heightmap = malloc(hw * hh * sizeof(float));
        if (!tm->heightmap
                || !rc_read_exact(f, tm->heightmap, sizeof(float), hw * hh,
                                  path, "terrain heightmap values")) {
            rc_asset_close(f);
            terrain_free(tm);
            return NULL;
        }
        fprintf(stderr, "terrain heightmap: %dx%d origin (%d,%d)\n", hw, hh, hx, hy);
    }
    rc_asset_close(f);
    tm->cpu_decode_ms = viewer_streaming_now_ms() - load_started_ms;
    return tm;
}

static int terrain_upload(TerrainMesh *tm) {
    if (!tm)
        return 0;
    if (tm->loaded)
        return 1;
    if (tm->cpu_mesh.vertexCount < 0
            || (tm->cpu_mesh.vertexCount > 0 && !tm->cpu_mesh.vertices))
        return 0;
    double started_ms = viewer_streaming_now_ms();
    UploadMesh(&tm->cpu_mesh, false);
    tm->model = LoadModelFromMesh(tm->cpu_mesh);
    tm->cpu_mesh = (Mesh){0};
    tm->gpu_upload_ms += viewer_streaming_now_ms() - started_ms;
    tm->loaded = tm->model.meshCount > 0;
    return tm->loaded;
}

static TerrainMesh *terrain_load(const char *path) {
    TerrainMesh *tm = terrain_load_cpu(path);
    if (!tm || terrain_upload(tm))
        return tm;
    terrain_free(tm);
    return NULL;
}

// Shift terrain so world coords (wx,wy) become origin (0,0)
static void terrain_offset(TerrainMesh *tm, int wx, int wy) {
    if (!tm) return;
    float dx = (float)wx, dz = (float)wy;
    Mesh *mesh = tm->loaded ? &tm->model.meshes[0] : &tm->cpu_mesh;
    float *v = mesh->vertices;
    if (v) {
        for (int i = 0; i < tm->vertex_count; i++) {
            v[i*3] -= dx;
            v[i*3+2] += dz;
        }
        if (tm->loaded) {
            UpdateMeshBuffer(*mesh, 0, v,
                             tm->vertex_count * 3 * sizeof(float), 0);
        }
    }
    tm->min_world_x -= wx; tm->min_world_y -= wy;
    if (tm->heightmap) { tm->hm_min_x -= wx; tm->hm_min_y -= wy; }
}

static float terrain_height_avg(TerrainMesh *tm, int x, int y) {
    if (!tm || !tm->heightmap) return -2.0f;
    int lx = x - tm->hm_min_x, ly = y - tm->hm_min_y;
    if (lx < 0 || lx >= tm->hm_width || ly < 0 || ly >= tm->hm_height)
        return -2.0f;
    int lx1 = lx + 1 < tm->hm_width ? lx + 1 : lx;
    int ly1 = ly + 1 < tm->hm_height ? ly + 1 : ly;
    return (tm->heightmap[lx + ly * tm->hm_width]
          + tm->heightmap[lx1 + ly * tm->hm_width]
          + tm->heightmap[lx + ly1 * tm->hm_width]
          + tm->heightmap[lx1 + ly1 * tm->hm_width]) * 0.25f;
}

static void terrain_free(TerrainMesh *tm) {
    if (!tm) return;
    if (tm->loaded) {
        UnloadModel(tm->model);
    } else {
        free(tm->cpu_mesh.vertices);
        free(tm->cpu_mesh.texcoords);
        free(tm->cpu_mesh.texcoords2);
        free(tm->cpu_mesh.normals);
        free(tm->cpu_mesh.tangents);
        free(tm->cpu_mesh.colors);
        free(tm->cpu_mesh.indices);
    }
    free(tm->heightmap);
    free(tm);
}

#endif
