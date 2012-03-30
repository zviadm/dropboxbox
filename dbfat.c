#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "dbfat.h"

// Directory Entries
struct DirEntry **DIR_ENTRIES;
struct DirEntry *ROOT_DIR_ENTRY;

// FAT Entries
uint32_t *FAT_ENTRIES;
uint32_t LAST_FREE_ENTRY = 2;

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

    FAT_ENTRIES = (uint32_t *)malloc(BPB_FATSz32 * BPB_BytesPerSector);
    DIR_ENTRIES = (struct DirEntry **)malloc(N_CLUSTERS * sizeof(struct DirEntry *));
    memset(FAT_ENTRIES, 0, BPB_FATSz32 * BPB_BytesPerSector);

    // initialize cluster 0 and 1 fat entries
    FAT_ENTRIES[0] = 0x0FFFFFF0;
    FAT_ENTRIES[1] = 0x08FFFFFF;

    // initialize root directory entry
    ROOT_DIR_ENTRY = (struct DirEntry *)malloc(sizeof(struct DirEntry));
    memset(ROOT_DIR_ENTRY, 0, sizeof(struct DirEntry));
    ROOT_DIR_ENTRY->first_cluster = BPB_RootCluster;
    ROOT_DIR_ENTRY->metadata.is_dir = 1;

    // root directory fat entry
    FAT_ENTRIES[BPB_RootCluster] = FAT_EOFC_ENTRY;
    DIR_ENTRIES[BPB_RootCluster] = ROOT_DIR_ENTRY;
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
uint8_t name_checksum(uint8_t *short_name) {
    uint8_t sum = 0;
    for (uint8_t name_len = 11; name_len > 0; name_len--) {
        // NOTE: The operation is an unsigned char rotate right
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *short_name++;
    }
    return sum;
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
    return 0xFFFFFFFF;
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
    DIR_ENTRIES[next_cluster] = DIR_ENTRIES[first_cluster];
    return first_cluster;
}

uint32_t get_entry_size(struct EntryMetaData *metadata) {
    return (((metadata->name_chars + LONG_NAME_CHARS_PER_ENTRY - 1) / LONG_NAME_CHARS_PER_ENTRY) + 1) * DIR_ENTRY_SIZE;
}

void add_child_entry(struct DirEntry *dir_entry, struct EntryMetaData *metadata) {
    uint32_t entry_extra_size = get_entry_size(metadata);
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
    
    // construct cluster chain
    new_dir_entry->first_cluster = allocate_cluster_chain(new_dir_entry, new_dir_entry->metadata.size);
}

uint32_t get_dir_contents(struct DirEntry *dir_entry, struct DirEntry *child_entry, uint8_t *buf) {
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

    uint8_t last_entry_mask = 0x40;
    for (uint8_t entry_ord = long_entries; entry_ord > 0; entry_ord--) {
        // add all long entries
        uint8_t long_entry[DIR_ENTRY_SIZE] = {
            last_entry_mask | entry_ord,
            UINT16_TOARRAY(wll[0]),
            UINT16_TOARRAY(wll[1]),
            UINT16_TOARRAY(wll[2]),
            UINT16_TOARRAY(wll[3]),
            UINT16_TOARRAY(wll[4]),
            ATTR_LONG_NAME, 
            0x00, 
            child_entry->metadata.name_checksum, 
            UINT16_TOARRAY(wll[5]),
            UINT16_TOARRAY(wll[6]),
            UINT16_TOARRAY(wll[7]),
            UINT16_TOARRAY(wll[8]),
            UINT16_TOARRAY(wll[9]),
            UINT16_TOARRAY(wll[10]),
            0x00, 0x00,
            UINT16_TOARRAY(wll[11]),
            UINT16_TOARRAY(wll[12]),
        };
        last_entry_mask = 0x00;

        memcpy(&buf[buf_offset], long_entry, DIR_ENTRY_SIZE);
        buf_offset += DIR_ENTRY_SIZE;
        
        name_offset -= LONG_NAME_CHARS_PER_ENTRY;
        if (name_offset >= 0) {
            memcpy(wll, &child_entry->metadata.name[name_offset], LONG_NAME_CHARS_PER_ENTRY * sizeof(wchar));
        }
    }

    // add short entry
    uint8_t *ll = child_entry->metadata.short_name;
    uint8_t short_entry[DIR_ENTRY_SIZE] = {
        ll[0], ll[1], ll[2], ll[3],
        ll[4], ll[5], ll[6], ll[7],
        ll[8], ll[9], ll[10],
        (child_entry->metadata.is_dir == 1) ? (ATTR_READ_ONLY | ATTR_DIRECTORY) : ATTR_READ_ONLY, 
        0x00,
        0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
        UINT16_TOARRAY(child_entry->first_cluster >> 16),
        UINT16_TOARRAY(child_entry->metadata.DIR_WrtTime),
        UINT16_TOARRAY(child_entry->metadata.DIR_WrtDate),
        UINT16_TOARRAY(child_entry->first_cluster & 0xFFFF),
        UINT32_TOARRAY((child_entry->metadata.is_dir == 1) ? 0 : child_entry->metadata.size),
    };
    memcpy(&buf[buf_offset], short_entry, DIR_ENTRY_SIZE);
    buf_offset += DIR_ENTRY_SIZE;

    return buf_offset;
}

void read_dir_sector(struct DirEntry *dir_entry, uint32_t offset, uint8_t *buf) {
    uint8_t tmp_buf[2 * BPB_BytesPerSector] = { 0x00 };
    uint32_t buf_offset = 0;

    // for non root dir_entry we have "dot" and "dotdot" entries
    uint32_t child_offset = (dir_entry == ROOT_DIR_ENTRY) ? 0 : (2 * DIR_ENTRY_SIZE);
    struct DirEntry *child_entry = dir_entry->child;
    while ((child_entry != NULL) && (child_offset < offset)) {
       uint32_t new_child_offset = child_offset + get_entry_size(&(child_entry->metadata));
       if (new_child_offset > offset)
           break;
       child_offset = new_child_offset;
       child_entry = child_entry->next;
    }

    if ((offset == 0) && (dir_entry != ROOT_DIR_ENTRY)) {
        // write "dot" and "dotdot" entries
        uint8_t dot_entry[DIR_ENTRY_SIZE] = {
            '.', ' ', ' ', ' ',
            ' ', ' ', ' ', ' ',
            ' ', ' ', ' ', 
            ATTR_DIRECTORY,
            0x00,
            0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
            UINT16_TOARRAY(dir_entry->first_cluster >> 16),
            UINT16_TOARRAY(dir_entry->metadata.DIR_WrtTime),
            UINT16_TOARRAY(dir_entry->metadata.DIR_WrtDate),
            UINT16_TOARRAY(dir_entry->first_cluster & 0xFFFF),
            UINT32_TOARRAY(0x00),
        };
        uint8_t dotdot_entry[DIR_ENTRY_SIZE] = {
            '.', '.', ' ', ' ',
            ' ', ' ', ' ', ' ',
            ' ', ' ', ' ', 
            ATTR_DIRECTORY,
            0x00,
            0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
            UINT16_TOARRAY((dir_entry->parent == ROOT_DIR_ENTRY) ? 0 : (dir_entry->parent->first_cluster >> 16)),
            UINT16_TOARRAY(dir_entry->metadata.DIR_WrtTime),
            UINT16_TOARRAY(dir_entry->metadata.DIR_WrtDate),
            UINT16_TOARRAY((dir_entry->parent == ROOT_DIR_ENTRY) ? 0 : (dir_entry->parent->first_cluster & 0xFFFF)),
            UINT32_TOARRAY(0x00),
        };
        memcpy(&tmp_buf[buf_offset], dot_entry, DIR_ENTRY_SIZE);
        buf_offset += DIR_ENTRY_SIZE;
        memcpy(&tmp_buf[buf_offset], dotdot_entry, DIR_ENTRY_SIZE);
        buf_offset += DIR_ENTRY_SIZE;
    } else if (child_offset < offset) {
        const uint32_t offset_delta = offset - child_offset;
        uint32_t entry_size = get_dir_contents(dir_entry, child_entry, tmp_buf);
        assert(entry_size > offset_delta);

        memmove(&tmp_buf[buf_offset], &tmp_buf[offset_delta], entry_size - offset_delta);
        buf_offset += entry_size - offset_delta;
        memset(&tmp_buf[buf_offset], 0, offset_delta);

        child_entry = child_entry->next;
    }

    while (child_entry && buf_offset < BPB_BytesPerSector) {
        uint32_t entry_size = get_dir_contents(dir_entry, child_entry, &tmp_buf[buf_offset]);
        buf_offset += entry_size;
        child_entry = child_entry->next;
    }
    printf("[DEBUG] Read Dir Sector: %u, %u, buf_offset: %u\n", dir_entry->first_cluster, offset, buf_offset);
    for (uint32_t i = 0; i < buf_offset; i += 32) {

    }
    memcpy(buf, tmp_buf, BPB_BytesPerSector);
}

int read_sector(uint32_t sector, uint8_t *buf) {
    if (sector < BPB_ReservedSectorCount) {
        // Handle BOOT_SECTOR and FS_INFO regions
        if (sector == 0 || sector == BPB_BackupBootSector) {
            memcpy(buf, BOOT_SECTOR, BPB_BytesPerSector);
        } else if (sector == BPB_FSInfo) {
            memcpy(buf, FS_INFO, BPB_BytesPerSector);
        } else {
            memset(buf, 0, BPB_BytesPerSector);
        }
    } else if (sector < (BPB_ReservedSectorCount + 2 * BPB_FATSz32)) {
        // Handle FAT Region
        uint32_t fat_sector;
        if (sector < (BPB_ReservedSectorCount + BPB_FATSz32)) {
            fat_sector = sector - BPB_ReservedSectorCount;
        } else {
            fat_sector = sector - BPB_ReservedSectorCount - BPB_FATSz32;
        }

        memcpy(buf, &FAT_ENTRIES[fat_sector * BPB_BytesPerSector / sizeof(uint32_t)], BPB_BytesPerSector);
    } else {
        // Handle Data Region
        uint32_t cluster_n = 2 + 
            (sector - (BPB_ReservedSectorCount + 2 * BPB_FATSz32)) / BPB_SectorsPerCluster;

        if (FAT_ENTRIES[cluster_n] == FAT_FREE_ENTRY) {
            memset(buf, 0, BPB_BytesPerSector);
        } else {
            printf("WHATSUP?\n");
            // Get DirEntry and offset of sector
            struct DirEntry *dir_entry = DIR_ENTRIES[cluster_n];
            uint32_t offset = 0;

            uint32_t first_cluster = dir_entry->first_cluster; 
            while (first_cluster != cluster_n) {
                first_cluster = FAT_ENTRIES[first_cluster];
                offset += BPB_SectorsPerCluster * BPB_BytesPerSector;
            }
            offset += (sector - ((cluster_n - 2) * BPB_SectorsPerCluster + (BPB_ReservedSectorCount + 2 * BPB_FATSz32))) *
                BPB_BytesPerSector;
            printf("WW: %u, %u, offset: %u\n", sector, dir_entry->first_cluster, offset);

            if (dir_entry->metadata.is_dir) {
                read_dir_sector(dir_entry, offset, buf);
            } else {
                memset(buf, 0, BPB_BytesPerSector);
            }
        }
    }
    return 0;
}

int read_data(uint32_t offset, uint32_t size, uint8_t *buf) {
    uint32_t sector = (offset / BPB_BytesPerSector);
    uint32_t sector_index = offset - sector * BPB_BytesPerSector;
    uint32_t buf_offset = 0;

    uint8_t tmp_sector[BPB_BytesPerSector];
    int r;
    while (buf_offset < size) {
        uint32_t sector_endindex = (offset + size) - (sector * BPB_BytesPerSector + sector_index);
        assert(sector_endindex > sector_index);

        uint32_t read_size;
        if ((sector_endindex - sector_index) < BPB_BytesPerSector) {
            read_size = sector_endindex - sector_index;
        } else{
            read_size = BPB_BytesPerSector;
        }
        
        if ((sector_index == 0) && (read_size == BPB_BytesPerSector)) {
            r = read_sector(sector, &buf[buf_offset]);
            if (r) {
                return r;
            }
        } else {
            r = read_sector(sector, tmp_sector);
            if (r) {
                return r;
            }
            memcpy(&buf[buf_offset], &tmp_sector[sector_index], read_size);
            sector_index = 0;
        }
        
        sector += 1;
        buf_offset += read_size;
    }
    assert(buf_offset == size);
    return 0;
}

void add_test_data() {
    struct EntryMetaData metadata;
    metadata.is_dir = 1;
    metadata.size = 64;

    metadata.DIR_WrtDate = 0b0000100010011110;
    metadata.DIR_WrtTime = 0; 

    metadata.name_chars = 5;
    metadata.name = (wchar *)malloc((metadata.name_chars + 1) * sizeof(wchar));
    swprintf(metadata.name, metadata.name_chars + 1, L"%ls", L"testy");
    snprintf((char *)metadata.short_name, 12, "%s", "TESTY      ");
    metadata.name_checksum = name_checksum(metadata.short_name);

    add_child_entry(ROOT_DIR_ENTRY, &metadata);
}

void create_test_image() {
    // some randomass testing
    FILE *img_file = fopen("dbbox.img", "w");
    uint8_t sector_buf[BPB_BytesPerSector];
    for (uint32_t sector = 0; sector < BPB_TotalSectors; sector++) {
        read_sector(sector, sector_buf);
        fwrite(sector_buf, sizeof(uint8_t), BPB_BytesPerSector, img_file);
    }
    fclose(img_file);
}

