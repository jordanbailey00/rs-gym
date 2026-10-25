#include "spawn_index.h"

#include "assets.h"

#include <stdlib.h>
#include <string.h>

enum {
    SPAWN_INDEX_HEADER_U32S = 9,
    SPAWN_INDEX_HEADER_BYTES = SPAWN_INDEX_HEADER_U32S * 4,
    SPAWN_INDEX_ENTRY_BYTES = 8,
};

typedef struct {
    uint32_t first_row;
    uint32_t row_count;
} SpawnPage;

static uint32_t read_u32_le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int compare_source_order(const void *a, const void *b) {
    uint32_t aa = read_u32_le(a);
    uint32_t bb = read_u32_le(b);
    return aa < bb ? -1 : aa > bb;
}

static int read_page(RcAssetReader *reader, uint16_t mapsquare,
                     uint32_t total_rows, SpawnPage *page) {
    uint64_t offset = SPAWN_INDEX_HEADER_BYTES
                    + (uint64_t)mapsquare * SPAWN_INDEX_ENTRY_BYTES;
    unsigned char raw[SPAWN_INDEX_ENTRY_BYTES];
    if (!rc_asset_reader_read_at(reader, offset, raw, sizeof(raw))) return 0;
    page->first_row = read_u32_le(raw);
    page->row_count = read_u32_le(raw + 4);
    return page->first_row <= total_rows
        && page->row_count <= total_rows - page->first_row;
}

static int mapsquare_bounds(int min_x, int min_y, int max_x, int max_y,
                            int *min_rx, int *min_ry,
                            int *max_rx, int *max_ry) {
    RcTileRect bounds;
    if (!rc_tile_rect_intersect_world(min_x, min_y, max_x, max_y, &bounds))
        return 0;
    *min_rx = bounds.min_x / RC_MAPSQUARE_SIZE;
    *min_ry = bounds.min_y / RC_MAPSQUARE_SIZE;
    *max_rx = bounds.max_x / RC_MAPSQUARE_SIZE;
    *max_ry = bounds.max_y / RC_MAPSQUARE_SIZE;
    return 1;
}

int rc_spawn_index_read(const char *path, uint32_t expected_magic,
                        uint32_t expected_record_size, int use_filter,
                        int min_x, int min_y, int max_x, int max_y,
                        RcSpawnIndexSlice *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!path || !path[0] || !out || expected_record_size < 4
            || (use_filter && (min_x > max_x || min_y > max_y))) {
        return 0;
    }
    RcAssetReader *reader = rc_asset_reader_open(path);
    if (!reader) return 0;

    uint32_t header[SPAWN_INDEX_HEADER_U32S];
    unsigned char raw_header[SPAWN_INDEX_HEADER_BYTES];
    if (!rc_asset_reader_read_at(reader, 0, raw_header, sizeof(raw_header))) {
        rc_asset_reader_close(reader);
        return 0;
    }
    for (int i = 0; i < SPAWN_INDEX_HEADER_U32S; i++)
        header[i] = read_u32_le(raw_header + i * 4);
    if (header[0] != expected_magic
            || header[1] != RC_SPAWN_INDEX_VERSION
            || header[3] != expected_record_size
            || header[4] > RC_SPAWN_INDEX_MAPSQUARE_COUNT
            || (uint64_t)header[5] + header[6] + header[7] + header[8]
                   != header[2]) {
        rc_asset_reader_close(reader);
        return 0;
    }

    uint64_t records_offset = SPAWN_INDEX_HEADER_BYTES
        + (uint64_t)RC_SPAWN_INDEX_MAPSQUARE_COUNT
            * SPAWN_INDEX_ENTRY_BYTES;
    uint64_t records_size = (uint64_t)header[2] * header[3];
    uint64_t expected_size = records_offset
                           + records_size;
    if (records_size > SIZE_MAX
            || rc_asset_reader_size(reader) != expected_size) {
        rc_asset_reader_close(reader);
        return 0;
    }

    out->total_rows = header[2];
    out->record_size = header[3];
    out->occupied_pages = header[4];
    memcpy(out->source_plane_counts, &header[5],
           sizeof(out->source_plane_counts));

    if (!use_filter) {
        out->record_count = out->total_rows;
        out->pages_loaded = out->occupied_pages;
        if (out->record_count > 0) {
            out->records = malloc((size_t)out->record_count * out->record_size);
            if (!out->records
                    || !rc_asset_reader_read_at(
                        reader, records_offset, out->records,
                        (size_t)out->record_count * out->record_size)) {
                rc_spawn_index_slice_free(out);
                rc_asset_reader_close(reader);
                return 0;
            }
        }
        rc_asset_reader_close(reader);
        return 1;
    }

    int min_rx, min_ry, max_rx, max_ry;
    if (!mapsquare_bounds(min_x, min_y, max_x, max_y,
                          &min_rx, &min_ry, &max_rx, &max_ry)) {
        rc_asset_reader_close(reader);
        return 1;
    }
    uint32_t page_capacity = (uint32_t)(max_rx - min_rx + 1)
                           * (uint32_t)(max_ry - min_ry + 1);
    SpawnPage *pages = calloc(page_capacity, sizeof(*pages));
    if (!pages) {
        rc_asset_reader_close(reader);
        return 0;
    }

    uint32_t selected_pages = 0;
    uint32_t selected_rows = 0;
    for (int rx = min_rx; rx <= max_rx; rx++) {
        for (int ry = min_ry; ry <= max_ry; ry++) {
            uint16_t mapsquare;
            if (!rc_mapsquare_key(rx, ry, &mapsquare)) {
                free(pages);
                rc_asset_reader_close(reader);
                return 0;
            }
            SpawnPage page;
            if (!read_page(reader, mapsquare, out->total_rows, &page)) {
                free(pages);
                rc_asset_reader_close(reader);
                return 0;
            }
            if (page.row_count == 0) continue;
            if (selected_rows > out->total_rows - page.row_count) {
                free(pages);
                rc_asset_reader_close(reader);
                return 0;
            }
            pages[selected_pages++] = page;
            selected_rows += page.row_count;
        }
    }

    if (selected_rows > 0) {
        out->records = malloc((size_t)selected_rows * out->record_size);
        if (!out->records) {
            free(pages);
            rc_asset_reader_close(reader);
            return 0;
        }
        unsigned char *dst = out->records;
        for (uint32_t i = 0; i < selected_pages; i++) {
            uint64_t offset = records_offset
                            + (uint64_t)pages[i].first_row * out->record_size;
            size_t size = (size_t)pages[i].row_count * out->record_size;
            if (!rc_asset_reader_read_at(reader, offset, dst, size)) {
                free(pages);
                rc_spawn_index_slice_free(out);
                rc_asset_reader_close(reader);
                return 0;
            }
            dst += size;
        }
    }
    free(pages);
    rc_asset_reader_close(reader);
    out->record_count = selected_rows;
    out->pages_loaded = selected_pages;
    return 1;
}

int rc_spawn_index_sort_source_order(RcSpawnIndexSlice *slice) {
    if (!slice || (slice->record_count > 0 && !slice->records)) return 0;
    if (slice->record_count > 1) {
        qsort(slice->records, slice->record_count, slice->record_size,
              compare_source_order);
    }
    for (uint32_t i = 0; i < slice->record_count; i++) {
        const unsigned char *record = slice->records
            + (size_t)i * slice->record_size;
        if (read_u32_le(record) >= slice->total_rows) return 0;
        if (i > 0) {
            const unsigned char *previous = record - slice->record_size;
            if (read_u32_le(previous) == read_u32_le(record)) return 0;
        }
    }
    return 1;
}

uint64_t rc_spawn_index_record_key(const char *path, uint32_t source_order,
                                   uint32_t ordinal) {
    if (!path || !path[0]) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    hash ^= 0xffu;
    hash *= UINT64_C(1099511628211);
    for (int shift = 0; shift < 32; shift += 8) {
        hash ^= (source_order >> shift) & 0xffu;
        hash *= UINT64_C(1099511628211);
    }
    for (int shift = 0; shift < 32; shift += 8) {
        hash ^= (ordinal >> shift) & 0xffu;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}

void rc_spawn_index_slice_free(RcSpawnIndexSlice *slice) {
    if (!slice) return;
    free(slice->records);
    memset(slice, 0, sizeof(*slice));
}
