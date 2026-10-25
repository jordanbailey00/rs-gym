#ifndef RC_ASSETS_H
#define RC_ASSETS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    RC_ASSET_BACKEND_AUTO = 0,
    RC_ASSET_BACKEND_LOOSE,
    RC_ASSET_BACKEND_PACK,
} RcAssetBackend;

typedef struct {
    unsigned char *data;
    size_t size;
} RcAssetBytes;

typedef struct RcAssetReader RcAssetReader;

// Asset configuration is mutable only before the first asset lookup. Reset is
// an explicit single-threaded test/tool operation that reopens configuration.
int rc_asset_set_backend(RcAssetBackend backend);
int rc_asset_set_data_root(const char *root);
int rc_asset_set_pack_dir(const char *dir);
int rc_asset_reset(void);

int rc_asset_exists(const char *path);
int rc_asset_size(const char *path, uint64_t *out_size);
// Packed range readers require an uncompressed entry; compressed pack assets
// remain available through rc_asset_fopen/rc_asset_read_all.
RcAssetReader *rc_asset_reader_open(const char *path);
uint64_t rc_asset_reader_size(const RcAssetReader *reader);
int rc_asset_reader_read_at(RcAssetReader *reader, uint64_t offset,
                            void *dst, size_t size);
void rc_asset_reader_close(RcAssetReader *reader);
FILE *rc_asset_fopen(const char *path, const char *mode);
int rc_asset_close(FILE *f);
RcAssetBytes rc_asset_read_all(const char *path);
void rc_asset_bytes_free(RcAssetBytes *bytes);

const char *rc_asset_logical_path(const char *path);

#endif
