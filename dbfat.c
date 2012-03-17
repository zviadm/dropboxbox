#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbfat.h"

void initialize() {
    // BOOT_SECTOR Checks
    assert(BOOT_SECTOR[13] == BPB_SectorsPerCluster);
    assert(BOOT_SECTOR[66] == 0x29);
    assert(BOOT_SECTOR[510] == 0x55);
    assert(BOOT_SECTOR[511] == 0xAA);

    // FS_INFO Checks
    assert(FS_INFO[510] == 0x55);
    assert(FS_INFO[511] == 0xAA);

    printf("Size of FAT in sectors: %u (%u Bytes)\n", BPB_FATSz32, BPB_FATSz32 * BPB_BytesPerSector);

    FAT_ENTRIES = (uint32_t*)malloc(N_CLUSTERS);
    // initialize cluster 0 and 1 fat entries
    FAT_ENTRIES[0] = 0x0FFFFFF0;
    FAT_ENTRIES[1] = 0x08FFFFFF;
    
    // initialize root directory entry
}

void cleanup() {
    free(FAT_ENTRIES);
}


/// name_checksum()
///     Returns an unsigned byte checksum computed on an unsigned byte
///     array. The array must be 11 bytes long and is assumed to contain
///     a name stored in the format of a MS-DOS directory entry.
///     Passed: short_name    Pointer to an unsigned byte array assumed to be
///     11 bytes long.
///     Returns: sum    An 8-bit unsigned checksum of the array pointed
///     to by short_name.
uint8_t name_checksum(unsigned char *short_name) {
    uint8_t sum = 0;
    for (uint8_t name_len = 11; name_len > 0; name_len--) {
        // NOTE: The operation is an unsigned char rotate right
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *short_name++;
    }
    return sum;
}


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
    return 0xFFFFFFFF;
}

uint32_t allocate_cluster_chain(uint32_t size) {
    uint32_t first_cluster = find_free_cluster();
    uint32_t current_size = BYTES_PER_CLUSTER;

    uint32_t next_cluster = first_cluster;
    while (current_size < size) {
        FAT_ENTRIES[next_cluster] = find_free_cluster();
        next_cluster = FAT_ENTRIES[next_cluster];
        current_size += BYTES_PER_CLUSTER;
    }
    FAT_ENTRIES[next_cluster] = FAT_EOFC_ENTRY;
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
        }

        next_cluster = FAT_ENTRIES[next_cluster];
        current_size += BYTES_PER_CLUSTER;
    }
    if (extending == 0) {
        // free clusters since we have shrunk current chain
        uint32_t tmp_cluster = next_cluster;
        while (FAT_ENTRIES[tmp_cluster] != FAT_EOFC_ENTRY) {
            uint32_t tmp = tmp_cluster;
            tmp_cluster = FAT_ENTRIES[tmp];
            FAT_ENTRIES[tmp] = FAT_FREE_ENTRY;
        }
    }
    
    FAT_ENTRIES[next_cluster] = FAT_EOFC_ENTRY;
    return first_cluster;
}

void add_child_entry(struct DirEntry *dir_entry, struct EntryMetaData *metadata) {
    uint32_t entry_extra_size = (((metadata->name_chars + LONG_NAME_CHARS_PER_ENTRY - 1) / LONG_NAME_CHARS_PER_ENTRY) + 1) * DIR_ENTRY_SIZE;
    // extend dir_entry if necessary to accomodate for extra entry_extra_size bytes
    uint32_t last_cluster_size = (dir_entry->metadata.size % BYTES_PER_CLUSTER);
    if ((last_cluster_size + entry_extra_size) > BYTES_PER_CLUSTER) {
        // extend dir_entry cluster chain
        dir_entry->metadata.size += entry_extra_size;
        dir_entry->first_cluster = reallocate_cluster_chain(dir_entry->first_cluster, dir_entry->metadata.size);
    }

    struct DirEntry *new_dir_entry = (struct DirEntry *)malloc(sizeof(struct DirEntry));
    memcpy(&new_dir_entry->metadata, metadata, sizeof(struct EntryMetaData));

    struct DirEntry *child = dir_entry->child;
    dir_entry->child = new_dir_entry;

    new_dir_entry->parent = dir_entry;
    new_dir_entry->next = child;
    new_dir_entry->child = NULL;
    
    if (new_dir_entry->metadata.is_dir == 1) {
        // adjust directory entry size for "dot" and "dotdot" entries
        new_dir_entry->metadata.size = 64;
    }

    // construct cluster chain
    new_dir_entry->first_cluster = allocate_cluster_chain(new_dir_entry->metadata.size);
}

uint32_t read_dir_contents_for_child(struct DirEntry *dir_entry, struct DirEntry *child_entry, unsigned char *buf) {
    uint8_t long_entries = (child_entry->metadata.name_chars + LONG_NAME_CHARS_PER_ENTRY - 1) / LONG_NAME_CHARS_PER_ENTRY;
    uint8_t name_offset = (long_entries - 1) * LONG_NAME_CHARS_PER_ENTRY;
    uint32_t buf_offset = 0;

    wchar wll[LONG_NAME_CHARS_PER_ENTRY] = { 
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
        0xFFFF, 
    };
    // prepare last long entry since it is more special than others
    for (uint8_t i = name_offset; i < child_entry->metadata.name_chars; i++) {
        wll[i - name_offset] = child_entry->metadata.name[i];
    }
    if (child_entry->metadata.name_chars - name_offset < LONG_NAME_CHARS_PER_ENTRY) {
        wll[child_entry->metadata.name_chars - name_offset] = 0x0000;
    }

    unsigned char *ll = (unsigned char*)wll;
    uint8_t last_entry_mask = 0x40;
    for (uint8_t entry_ord = long_entries; entry_ord > 0; entry_ord--) {
        // add all long entries
        unsigned char long_entry[32] = {
            last_entry_mask | entry_ord,
            ll[0], ll[1], ll[2], ll[3],
            ll[4], ll[5], ll[6], ll[7],
            ll[8], ll[9], 
            ATTR_LONG_NAME, 
            0x00, 
            child_entry->metadata.name_checksum, 
            ll[10], ll[11], ll[12], ll[13],
            ll[14], ll[15], ll[16], ll[17],
            ll[18], ll[19], ll[20], ll[21],
            0x00, 0x00,
            ll[22], ll[23], ll[24], ll[25],
        };
        last_entry_mask = 0x00;

        memcpy(&buf[buf_offset], long_entry, 32);
        buf_offset += 32;
        
        name_offset -= LONG_NAME_CHARS_PER_ENTRY;
        if (name_offset >= 0) {
            memcpy(wll, &child_entry->metadata.name[name_offset], LONG_NAME_CHARS_PER_ENTRY * sizeof(wchar));
        }
    }

    // add short entry
    ll = child_entry->metadata.short_name;
    unsigned char short_entry[32] = {
        ll[0], ll[1], ll[2], ll[3],
        ll[4], ll[5], ll[6], ll[7],
        ll[8], ll[9], ll[10],
        (child_entry->metadata.is_dir == 1) ? (ATTR_READ_ONLY | ATTR_DIRECTORY) : ATTR_READ_ONLY, 
        0x00,
        0x00,
        child_entry->first_cluster >> 16,
        child_entry->metadata.DIR_WrtTime,
        child_entry->metadata.DIR_WrtDate,
        child_entry->first_cluster & 0xFFFF,
        child_entry->metadata.size,
    };
    memcpy(&buf[buf_offset], short_entry, 32);
    buf_offset += 32;

    return buf_offset;
}

int read_sector(uint32_t sector, uint8_t* buf) {
    if (sector < BPB_ReservedSectorCount) {
        // Handle BOOT_SECTOR and FS_INFO regions
        if (sector == 0 || sector == BPB_BackupBootSector) {
            memcpy(buf, BOOT_SECTOR, BPB_BytesPerSector);
        } else if (sector == BPB_FSInfo) {
            memcpy(buf, FS_INFO, BPB_BytesPerSector);
        } else {
            memset(buf, 0, BPB_BytesPerSector);
        }
    } else if (sector < BPB_ReservedSectorCount + 2 * BPB_FATSz32) {
        // Handle FAT Region
        uint32_t fat_sector;
        if (sector < BPB_ReservedSectorCount + BPB_FATSz32) {
            fat_sector = sector - BPB_ReservedSectorCount;
        } else {
            fat_sector = sector - BPB_ReservedSectorCount - BPB_FATSz32;
        }

        memcpy(buf, &FAT_ENTRIES[fat_sector * BPB_BytesPerSector / sizeof(uint32_t)], BPB_BytesPerSector);
    } else {
        // Handle Data Region
        uint32_t cluster_n = 2 + 
            (sector - BPB_ReservedSectorCount + 2 * BPB_FATSz32) / BPB_SectorsPerCluster;

        if (FAT_ENTRIES[cluster_n] == FAT_FREE_ENTRY) {
            memset(buf, 0, BPB_BytesPerSector);
        } else {

        }
    }
    return 0;
}

int main(int argv, char* argc[]) {
    initialize();

    FILE *img_file = fopen("dbbox.img", "w");
    uint8_t sector_buf[BPB_BytesPerSector];
    for (uint32_t sector = 0; sector < BPB_TotalSectors; sector++) {
        read_sector(sector, sector_buf);
        fwrite(sector_buf, sizeof(uint8_t), BPB_BytesPerSector, img_file);
    }
    fclose(img_file);

    cleanup();
}
