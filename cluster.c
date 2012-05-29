#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbfat.h"
#include "cluster.h"

// FAT Entries
uint32_t *FAT_ENTRIES;
uint32_t LAST_FREE_ENTRY = 2;

// Directory Entries
struct DirEntry **DIR_ENTRIES;

void initialize_clusters(struct DirEntry *ROOT_DIR_ENTRY) {
    // BOOT_SECTOR Checks
    assert(BOOT_SECTOR[13] == BPB_SectorsPerCluster);
    assert(BOOT_SECTOR[66] == 0x29);
    assert(BOOT_SECTOR[510] == 0x55);
    assert(BOOT_SECTOR[511] == 0xAA);

    // FS_INFO Checks
    assert(FS_INFO[510] == 0x55);
    assert(FS_INFO[511] == 0xAA);

    printf("Size of FAT in sectors: %u (%u Bytes)\n", BPB_FATSz32, BPB_FATSz32 * BPB_BytesPerSector);

    FAT_ENTRIES = (uint32_t *)malloc(BPB_FATSz32 * BPB_BytesPerSector);
    DIR_ENTRIES = (struct DirEntry **)malloc(N_CLUSTERS * sizeof(struct DirEntry *));
    memset(FAT_ENTRIES, 0, BPB_FATSz32 * BPB_BytesPerSector);

    // initialize cluster 0 and 1 fat entries
    FAT_ENTRIES[0] = 0x0FFFFFF0;
    FAT_ENTRIES[1] = 0x08FFFFFF;

    // root directory fat entry
    FAT_ENTRIES[BPB_RootCluster] = FAT_EOFC_ENTRY;
    DIR_ENTRIES[BPB_RootCluster] = ROOT_DIR_ENTRY;
}

void cleanup_clusters() {
    free(FAT_ENTRIES);
    free(DIR_ENTRIES);
}

/// find_free_cluster()
///     Finds available cluster by traversing through FAT_ENTRIES
uint32_t find_free_cluster() {
    for (uint32_t i = LAST_FREE_ENTRY; i < N_CLUSTERS; i++) {
        if (FAT_ENTRIES[i] == FAT_FREE_ENTRY) {
            LAST_FREE_ENTRY = i;
            return i;
        }
    }
    for (uint32_t i = 2; i < LAST_FREE_ENTRY; i++) {
        if (FAT_ENTRIES[i] == FAT_FREE_ENTRY) {
            LAST_FREE_ENTRY = i;
            return i;
        }
    }
    // if there are no more available clusters that is bad, very bad...
    assert(0);
}

int is_cluster_free(uint32_t cluster) {
    return (FAT_ENTRIES[cluster] == FAT_FREE_ENTRY) ? 1 : 0;
}

uint32_t allocate_cluster_chain(struct DirEntry *dir_entry, uint32_t size) {
    uint32_t first_cluster = find_free_cluster();
    uint32_t current_size = BYTES_PER_CLUSTER;

    uint32_t next_cluster = first_cluster;
    while (current_size < size) {
        FAT_ENTRIES[next_cluster] = find_free_cluster();
        DIR_ENTRIES[next_cluster] = dir_entry;
        next_cluster = FAT_ENTRIES[next_cluster];
        current_size += BYTES_PER_CLUSTER;
    }
    FAT_ENTRIES[next_cluster] = FAT_EOFC_ENTRY;
    DIR_ENTRIES[next_cluster] = dir_entry;
    return first_cluster;
}

uint32_t reallocate_cluster_chain(uint32_t first_cluster, uint32_t new_size) {
    uint32_t next_cluster = first_cluster;
    uint32_t current_size = BYTES_PER_CLUSTER;

    uint8_t extending = 0;

    while (current_size < new_size) {
        if (FAT_ENTRIES[next_cluster] == FAT_EOFC_ENTRY) {
            // new size is larger than previous so extend the cluster
            extending = 1;
        }
        if (extending == 1) {
            FAT_ENTRIES[next_cluster] = find_free_cluster();
            DIR_ENTRIES[next_cluster] = DIR_ENTRIES[first_cluster];
        }

        next_cluster = FAT_ENTRIES[next_cluster];
        current_size += BYTES_PER_CLUSTER;
    }
    if (extending == 0 && FAT_ENTRIES[next_cluster] != FAT_EOFC_ENTRY) {
        // free clusters since we have shrunk current chain
        free_cluster_chain(FAT_ENTRIES[next_cluster]);
    }

    FAT_ENTRIES[next_cluster] = FAT_EOFC_ENTRY;
    DIR_ENTRIES[next_cluster] = DIR_ENTRIES[first_cluster];
    return first_cluster;
}

void free_cluster_chain(uint32_t first_cluster) {
    uint32_t next_cluster = first_cluster;
    do {
        uint32_t tmp = FAT_ENTRIES[next_cluster];
        FAT_ENTRIES[next_cluster] = FAT_FREE_ENTRY;
        next_cluster = tmp;
    } while (next_cluster != FAT_EOFC_ENTRY);
}

uint32_t get_cluster_chain_size(uint32_t first_cluster, uint32_t last_cluster) {
    uint32_t clusters = 0;
    while (first_cluster != last_cluster) {
        assert(first_cluster != FAT_FREE_ENTRY);
        assert(first_cluster != FAT_EOFC_ENTRY);
        first_cluster = FAT_ENTRIES[first_cluster];
        clusters += 1;
    }
    return clusters * BPB_SectorsPerCluster * BPB_BytesPerSector;
}

struct DirEntry * get_cluster_dir_entry(uint32_t cluster) {
    return DIR_ENTRIES[cluster];
}

int read_fat_sector(uint32_t fat_sector, uint8_t *buf) {
    memcpy(buf, &FAT_ENTRIES[fat_sector * BPB_BytesPerSector / sizeof(uint32_t)], BPB_BytesPerSector);
    return 0;
}
