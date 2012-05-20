#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbfat.h"
#include "dbfiles.h"

// 128KB blocks
#define CACHE_BLOCK_SIZE  (1 << 17)
#define CACHE_BLOCK_COUNT 32

struct CachedBlock {
    void *direntry_addr;
    uint32_t offset;
    uint8_t rev[DB_REV_SIZE];
    char buffer[CACHE_BLOCK_SIZE];
};

struct CachedBlock **file_cache;

void schedule_file_sector(struct DirEntry *dir_entry, uint32_t offset) {
    //uint32_t block_offset = offset & (CACHE_BLOCK_SIZE - 1);
    // check if block_offset is already in the cache
}


void initialize_file_cache() {
    file_cache = (struct CachedBlock **)calloc(CACHE_BLOCK_COUNT, sizeof(struct CachedBlock *));
    assert(file_cache != NULL);

    for (int i = 0; i < CACHE_BLOCK_COUNT; i++) {
        file_cache[i] = (struct CachedBlock *)calloc(1, sizeof(struct CachedBlock));
    }
}

void cleanup_file_cache() {
    for (int i = 0; i < CACHE_BLOCK_COUNT; i++) {
        free(file_cache[i]);
    }
    free(file_cache);
}

int read_file_cache(struct DirEntry *dir_entry, uint32_t offset, uint8_t *buf) {
    for (int i = 0; i < CACHE_BLOCK_COUNT; i++) {
        if ((file_cache[i]->direntry_addr == (void *)dir_entry) &&
                (file_cache[i]->offset <= offset) &&
                ((file_cache[i]->offset + CACHE_BLOCK_SIZE) > (offset + BPB_BytesPerSector)) &&
                (memcmp(file_cache[i]->rev, dir_entry->metadata.rev, DB_REV_SIZE) == 0)) {
            // with pretty high chance this cached copy is for the file that we are trying to fetch
            // TODO(zm): instead should cache full file path and revision, or maybe GUID (nah nah nah :))
            memcpy(buf, &(file_cache[i]->buffer[offset - file_cache[i]->offset]), BPB_BytesPerSector);
            return 0;
        }
    }
    return 1;
}
