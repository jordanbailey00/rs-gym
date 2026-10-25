#include "streaming_async.h"

#include <string.h>

static void destroy_job(ViewerAsyncLoader *loader, ViewerAsyncJob *job) {
    if (job->request && loader->destroy_request)
        loader->destroy_request(job->request, loader->user);
    memset(job, 0, sizeof(*job));
}

static void destroy_result(ViewerAsyncLoader *loader,
                           ViewerAsyncResult *result) {
    if (result->payload && loader->destroy_payload)
        loader->destroy_payload(result->payload, loader->user);
    memset(result, 0, sizeof(*result));
}

static void *async_worker(void *opaque) {
    ViewerAsyncLoader *loader = opaque;
    for (;;) {
        pthread_mutex_lock(&loader->mutex);
        while (loader->running && loader->queued_count == 0)
            pthread_cond_wait(&loader->wake, &loader->mutex);
        if (!loader->running) {
            pthread_mutex_unlock(&loader->mutex);
            return NULL;
        }
        ViewerAsyncJob job = loader->queued[0];
        loader->queued_count--;
        if (loader->queued_count > 0) {
            memmove(&loader->queued[0], &loader->queued[1],
                    (size_t)loader->queued_count * sizeof(loader->queued[0]));
        }
        memset(&loader->queued[loader->queued_count], 0,
               sizeof(loader->queued[0]));
        loader->working_generation = job.generation;
        loader->working_key = job.key;
        uint64_t active_generation = loader->active_generation;
        pthread_mutex_unlock(&loader->mutex);

        if (job.generation != active_generation) {
            destroy_job(loader, &job);
            pthread_mutex_lock(&loader->mutex);
            loader->working_generation = 0;
            loader->working_key = 0;
            pthread_mutex_unlock(&loader->mutex);
            continue;
        }

        size_t upload_bytes = 0;
        double decode_ms = 0.0;
        void *payload = loader->decode(
            job.request, &upload_bytes, &decode_ms, loader->user);
        destroy_job(loader, &job);
        ViewerAsyncResult result = {
            .generation = active_generation,
            .key = loader->working_key,
            .payload = payload,
            .upload_bytes = upload_bytes,
            .decode_ms = decode_ms,
            .success = payload != NULL,
        };

        pthread_mutex_lock(&loader->mutex);
        loader->working_generation = 0;
        loader->working_key = 0;
        if (!loader->running
                || result.generation != loader->active_generation
                || loader->completed_count
                    >= VIEWER_STREAMING_CHUNK_CAPACITY) {
            pthread_mutex_unlock(&loader->mutex);
            destroy_result(loader, &result);
            continue;
        }
        loader->completed[loader->completed_count++] = result;
        pthread_mutex_unlock(&loader->mutex);
    }
}

int viewer_async_loader_init(ViewerAsyncLoader *loader,
                             ViewerAsyncDecodeFn decode,
                             ViewerAsyncDestroyFn destroy_request,
                             ViewerAsyncDestroyFn destroy_payload,
                             void *user) {
    if (!loader || !decode || !destroy_request || !destroy_payload)
        return 0;
    memset(loader, 0, sizeof(*loader));
    loader->decode = decode;
    loader->destroy_request = destroy_request;
    loader->destroy_payload = destroy_payload;
    loader->user = user;
    loader->active_generation = 1;
    loader->running = 1;
    if (pthread_mutex_init(&loader->mutex, NULL) != 0)
        return 0;
    if (pthread_cond_init(&loader->wake, NULL) != 0) {
        pthread_mutex_destroy(&loader->mutex);
        return 0;
    }
    if (pthread_create(&loader->thread, NULL, async_worker, loader) != 0) {
        pthread_cond_destroy(&loader->wake);
        pthread_mutex_destroy(&loader->mutex);
        return 0;
    }
    loader->initialized = 1;
    return 1;
}

void viewer_async_loader_set_generation(ViewerAsyncLoader *loader,
                                        uint64_t generation) {
    if (!loader || !loader->initialized || generation == 0)
        return;
    pthread_mutex_lock(&loader->mutex);
    loader->active_generation = generation;
    for (int i = 0; i < loader->queued_count; i++)
        destroy_job(loader, &loader->queued[i]);
    for (int i = 0; i < loader->completed_count; i++)
        destroy_result(loader, &loader->completed[i]);
    loader->queued_count = 0;
    loader->completed_count = 0;
    pthread_mutex_unlock(&loader->mutex);
}

static int job_exists(const ViewerAsyncLoader *loader, uint64_t generation,
                      uint64_t key) {
    if (loader->working_generation == generation && loader->working_key == key)
        return 1;
    for (int i = 0; i < loader->queued_count; i++) {
        if (loader->queued[i].generation == generation
                && loader->queued[i].key == key)
            return 1;
    }
    for (int i = 0; i < loader->completed_count; i++) {
        if (loader->completed[i].generation == generation
                && loader->completed[i].key == key)
            return 1;
    }
    return 0;
}

int viewer_async_loader_submit(ViewerAsyncLoader *loader,
                               uint64_t generation, uint64_t key,
                               void *request) {
    if (!loader || !loader->initialized || !request || generation == 0)
        return 0;
    pthread_mutex_lock(&loader->mutex);
    if (!loader->running || generation != loader->active_generation
            || loader->queued_count >= VIEWER_STREAMING_CHUNK_CAPACITY
            || job_exists(loader, generation, key)) {
        pthread_mutex_unlock(&loader->mutex);
        return 0;
    }
    loader->queued[loader->queued_count++] = (ViewerAsyncJob){
        .generation = generation,
        .key = key,
        .request = request,
    };
    pthread_cond_signal(&loader->wake);
    pthread_mutex_unlock(&loader->mutex);
    return 1;
}

int viewer_async_loader_poll(ViewerAsyncLoader *loader,
                             ViewerAsyncResult *out) {
    if (!loader || !loader->initialized || !out)
        return 0;
    pthread_mutex_lock(&loader->mutex);
    if (loader->completed_count == 0) {
        pthread_mutex_unlock(&loader->mutex);
        return 0;
    }
    *out = loader->completed[0];
    loader->completed_count--;
    if (loader->completed_count > 0) {
        memmove(&loader->completed[0], &loader->completed[1],
                (size_t)loader->completed_count
                    * sizeof(loader->completed[0]));
    }
    memset(&loader->completed[loader->completed_count], 0,
           sizeof(loader->completed[0]));
    pthread_mutex_unlock(&loader->mutex);
    return 1;
}

int viewer_async_loader_pending(ViewerAsyncLoader *loader,
                                uint64_t generation) {
    if (!loader || !loader->initialized || generation == 0)
        return 0;
    pthread_mutex_lock(&loader->mutex);
    int count = loader->working_generation == generation;
    for (int i = 0; i < loader->queued_count; i++)
        count += loader->queued[i].generation == generation;
    for (int i = 0; i < loader->completed_count; i++)
        count += loader->completed[i].generation == generation;
    pthread_mutex_unlock(&loader->mutex);
    return count;
}

void viewer_async_loader_release_result(ViewerAsyncLoader *loader,
                                        ViewerAsyncResult *result) {
    if (!loader || !result)
        return;
    destroy_result(loader, result);
}

void viewer_async_loader_destroy(ViewerAsyncLoader *loader) {
    if (!loader || !loader->initialized)
        return;
    pthread_mutex_lock(&loader->mutex);
    loader->running = 0;
    pthread_cond_broadcast(&loader->wake);
    pthread_mutex_unlock(&loader->mutex);
    pthread_join(loader->thread, NULL);

    for (int i = 0; i < loader->queued_count; i++)
        destroy_job(loader, &loader->queued[i]);
    for (int i = 0; i < loader->completed_count; i++)
        destroy_result(loader, &loader->completed[i]);
    pthread_cond_destroy(&loader->wake);
    pthread_mutex_destroy(&loader->mutex);
    memset(loader, 0, sizeof(*loader));
}
