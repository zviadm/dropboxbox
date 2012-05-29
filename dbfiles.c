#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>
#include <pthread.h>
#include <sys/timeb.h>

#include "dbapi.h"
#include "dbfat.h"
#include "dbfiles.h"

#define CACHE_BLOCK_SIZE  (1 << 22) // 512KB blocks
#define CACHE_BLOCK_COUNT 32        // number of blocks must be more than (max_prefetched_blocks * fuse_threads + block_fetcher_thread_count)

const int BLOCK_FETCHER_THREAD_COUNT = 10; // number of threads that fetch file blocks
const int MAX_BLOCK_PREFETCH = 5;          // maximum number of blocks to prefetch
const int READ_SECTOR_TIMEOUT = 30 * 1000; // sector reading timeout in milli seconds

enum BlockState {
    CLEAN = 0,
    SCHEDULED = 1,
    DOWNLOADING = 2,
    COMPLETED = 3,
};

struct CachedBlock {
    volatile int ref_count;
    volatile enum BlockState block_state;
    volatile long long int last_access;

    char *utf8path;
    size_t path_size;
    char rev[DB_REV_SIZE];
    uint32_t offset;
    char buffer[CACHE_BLOCK_SIZE];
};

struct CachedBlock **file_cache;
pthread_mutex_t file_cache_lock;

// forward declarations
void *block_fetcher_thread(void *args);


long long int time_msec() {
    struct timeb t;
    ftime(&t);
    return (long long int)t.time * 1000 + (long long int)t.millitm;
}

void initialize_file_cache() {
    file_cache = (struct CachedBlock **)calloc(CACHE_BLOCK_COUNT, sizeof(struct CachedBlock *));
    assert(file_cache != NULL);

    for (int i = 0; i < CACHE_BLOCK_COUNT; i++) {
        file_cache[i] = (struct CachedBlock *)calloc(1, sizeof(struct CachedBlock));
    }

    curl_global_init(CURL_GLOBAL_ALL);
    pthread_mutex_init(&file_cache_lock, NULL);

    // create block fetcher threads
    for (int i = 0; i < BLOCK_FETCHER_THREAD_COUNT; i++) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        pthread_t thread;
        pthread_create(&thread, &attr, block_fetcher_thread, NULL);
    }
}

void cleanup_file_cache() {
    // TODO(ZM): for this function to actually cleanup stuff all block_fetcher_thread-s need
    // to be terminated first!
    pthread_mutex_destroy(&file_cache_lock);
    for (int i = 0; i < CACHE_BLOCK_COUNT; i++) {
        if (file_cache[i]->utf8path) {
            free(file_cache[i]->utf8path);
        }
        free(file_cache[i]);
    }
    free(file_cache);
}

int schedule_sector(size_t path_size, char *utf8path, char *rev, uint32_t offset) {
    assert((offset & (BPB_BytesPerSector - 1)) == 0);

    pthread_mutex_lock(&file_cache_lock);
    uint32_t block_offset = offset & ~(CACHE_BLOCK_SIZE - 1);
    int block_index = -1;
    // check if block_offset is already in the cache
    for (int i = 0; i < CACHE_BLOCK_COUNT; i++) {
        if ((file_cache[i]->offset == block_offset) &&
                (file_cache[i]->path_size == path_size) &&
                (memcmp(file_cache[i]->utf8path, utf8path, file_cache[i]->path_size) == 0) &&
                (memcmp(file_cache[i]->rev, rev, DB_REV_SIZE) == 0)) {
            block_index = i;
            break;
        }

    }

    if (block_index == -1) {
        // could not find block in the cache, so choose free block
        // TODO(ZM): need to choose random block
        for (int i = 0; i < CACHE_BLOCK_COUNT; i++) {
            if ((file_cache[i]->ref_count == 0) &&
                    ((block_index == -1) ||
                     (file_cache[i]->last_access < file_cache[block_index]->last_access))) {
                block_index = i;
            }
        }

        // There should always be at least one free block
        assert(block_index != -1);

        file_cache[block_index]->offset = block_offset;
        file_cache[block_index]->path_size = path_size;
        file_cache[block_index]->utf8path = realloc(file_cache[block_index]->utf8path, file_cache[block_index]->path_size);
        assert(file_cache[block_index]->utf8path != NULL);
        memcpy(file_cache[block_index]->utf8path, utf8path, file_cache[block_index]->path_size);
        memcpy(file_cache[block_index]->rev, rev, DB_REV_SIZE);

        file_cache[block_index]->block_state = SCHEDULED;
    }

    file_cache[block_index]->last_access = time_msec();
    file_cache[block_index]->ref_count++;
    pthread_mutex_unlock(&file_cache_lock);

    return block_index;
}

void release_cache_block(int block_index) {
    pthread_mutex_lock(&file_cache_lock);
    file_cache[block_index]->ref_count--;
    pthread_mutex_unlock(&file_cache_lock);
}


int read_sector_from_cache(size_t path_size, char *utf8path, char *rev, uint32_t offset, uint32_t file_size, uint8_t *buf) {
    int prefetch_count = (file_size - offset + CACHE_BLOCK_SIZE - 1) / CACHE_BLOCK_SIZE;
    if (prefetch_count > MAX_BLOCK_PREFETCH) {
        prefetch_count = MAX_BLOCK_PREFETCH;
    }

    assert(prefetch_count <= 16);
    int block_indexes[16] = { 0 };

    for (int i = prefetch_count - 1; i >= 0; i--) {
        // schedule blocks in reverse order since they get prioritized backwards
        block_indexes[i] = schedule_sector(path_size, utf8path, rev, offset + CACHE_BLOCK_SIZE * i);
    }

    int block_index = block_indexes[0];

    long long int start_time = time_msec();
    long long int current_time = start_time;
    while ((file_cache[block_index]->block_state != COMPLETED) &&
            (current_time - start_time <= READ_SECTOR_TIMEOUT)) {
        usleep(100 * 1000);
        current_time = time_msec();
    }

    int ret;
    if (file_cache[block_index]->block_state != COMPLETED) {
        printf("[DEBUG] DBFiles failed to read sector: %s, offset: %u...\n", utf8path, offset);
        ret = -1;
    } else {
        memcpy(buf, &(file_cache[block_index]->buffer[offset & (CACHE_BLOCK_SIZE - 1)]), BPB_BytesPerSector);
        ret = 0;
    }

    for (int i = 0; i < prefetch_count; i++) {
        release_cache_block(block_indexes[i]);
    }
    return ret;
}

void *block_fetcher_thread(void *args) {
    CURL *curl = curl_easy_init();
    int block_index;

    while (1) {
        pthread_mutex_lock(&file_cache_lock);
        block_index = -1;
        for (int i = 0; i < CACHE_BLOCK_COUNT; i++) {
            // Prioritize block downloading first by ref_count then by offset
            if ((file_cache[i]->block_state == SCHEDULED) &&
                    ((block_index == -1) ||
                     (file_cache[i]->ref_count > file_cache[block_index]->ref_count) ||
                     ((file_cache[i]->ref_count == file_cache[block_index]->ref_count) &&
                      (file_cache[i]->offset < file_cache[block_index]->offset)))) {
                block_index = i;
            }
        }
        if (block_index != -1) {
            file_cache[block_index]->ref_count++;
            file_cache[block_index]->block_state = DOWNLOADING;
        }
        pthread_mutex_unlock(&file_cache_lock);

        if (block_index == -1) {
            usleep(50 * 1000);
        } else {
            // download file block
            printf("[DEBUG] DBFiles downloading block: %s, offset: %u, slot: %d...\n",
                    file_cache[block_index]->utf8path, file_cache[block_index]->offset, block_index);
            char *tmp_buf;
            size_t tmp_buf_size;

            int ret = dbapi_get_file(curl,
                    file_cache[block_index]->utf8path,
                    file_cache[block_index]->rev,
                    file_cache[block_index]->offset,
                    file_cache[block_index]->offset + CACHE_BLOCK_SIZE,
                    &tmp_buf, &tmp_buf_size);
            if (ret == 0) {
                assert(tmp_buf_size <= CACHE_BLOCK_SIZE);
                memset(file_cache[block_index]->buffer, 0, CACHE_BLOCK_SIZE);
                memcpy(file_cache[block_index]->buffer, tmp_buf, tmp_buf_size);
                free(tmp_buf);
                printf("[DEBUG] DBFiles successfully downloaded block: %s, offset: %u...\n",
                        file_cache[block_index]->utf8path, file_cache[block_index]->offset);
            } else {
                printf("[DEBUG] DBFiles failed to download block: %s, offset: %u...\n",
                        file_cache[block_index]->utf8path, file_cache[block_index]->offset);
            }

            pthread_mutex_lock(&file_cache_lock);
            file_cache[block_index]->block_state = (ret == 0) ? COMPLETED : SCHEDULED;
            file_cache[block_index]->ref_count--;
            pthread_mutex_unlock(&file_cache_lock);
        }
    }
}
