#ifndef RUNEC_VIEWER_STREAMING_ASYNC_H
#define RUNEC_VIEWER_STREAMING_ASYNC_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "streaming.h"

typedef void *(*ViewerAsyncDecodeFn)(void *request, size_t *upload_bytes,
                                     double *decode_ms, void *user);
typedef void (*ViewerAsyncDestroyFn)(void *value, void *user);

typedef struct {
    uint64_t generation;
    uint64_t key;
    void *request;
} ViewerAsyncJob;

typedef struct {
    uint64_t generation;
    uint64_t key;
    void *payload;
    size_t upload_bytes;
    double decode_ms;
    int success;
} ViewerAsyncResult;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    ViewerAsyncDecodeFn decode;
    ViewerAsyncDestroyFn destroy_request;
    ViewerAsyncDestroyFn destroy_payload;
    void *user;
    ViewerAsyncJob queued[VIEWER_STREAMING_CHUNK_CAPACITY];
    ViewerAsyncResult completed[VIEWER_STREAMING_CHUNK_CAPACITY];
    int queued_count;
    int completed_count;
    uint64_t active_generation;
    uint64_t working_generation;
    uint64_t working_key;
    int running;
    int initialized;
} ViewerAsyncLoader;

int viewer_async_loader_init(ViewerAsyncLoader *loader,
                             ViewerAsyncDecodeFn decode,
                             ViewerAsyncDestroyFn destroy_request,
                             ViewerAsyncDestroyFn destroy_payload,
                             void *user);
void viewer_async_loader_set_generation(ViewerAsyncLoader *loader,
                                        uint64_t generation);
int viewer_async_loader_submit(ViewerAsyncLoader *loader,
                               uint64_t generation, uint64_t key,
                               void *request);
int viewer_async_loader_poll(ViewerAsyncLoader *loader,
                             ViewerAsyncResult *out);
int viewer_async_loader_pending(ViewerAsyncLoader *loader,
                                uint64_t generation);
void viewer_async_loader_release_result(ViewerAsyncLoader *loader,
                                        ViewerAsyncResult *result);
void viewer_async_loader_destroy(ViewerAsyncLoader *loader);

#endif
