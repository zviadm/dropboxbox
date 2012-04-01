#include <assert.h>
#include <ctype.h>
#include <iconv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbfat.h"
#include "cluster.h"

// Directory Entries
struct DirEntry *ROOT_DIR_ENTRY;

void initialize() {
    // initialize DIR_ENTRIES and ROOT directory entry
    ROOT_DIR_ENTRY = (struct DirEntry *)malloc(sizeof(struct DirEntry));
    memset(ROOT_DIR_ENTRY, 0, sizeof(struct DirEntry));
    ROOT_DIR_ENTRY->first_cluster = BPB_RootCluster;
    ROOT_DIR_ENTRY->metadata.is_dir = 1;
    initialize_clusters(ROOT_DIR_ENTRY);
}

void cleanup() {
    cleanup_clusters();
    // TODO(zm): Recursively cleanup all directory entries starting from ROOT
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

uint16_t get_wrt_date(uint32_t epoch) {
    // TODO(zm): actually implement this
    return 0b0000100010011110;
}

uint16_t get_wrt_time(uint32_t epoch) {
    // TODO(zm): actually implement this
    return 0;
}

uint32_t get_entry_size(struct EntryMetaData *metadata) {
    return (((metadata->name_chars + LONG_NAME_CHARS_PER_ENTRY - 1) / LONG_NAME_CHARS_PER_ENTRY) + 1) * DIR_ENTRY_SIZE;
}

void _short_name_helper(struct EntryMetaData *metadata, uint32_t *short_index, uint32_t *long_index, uint8_t max_short_index) {
    while ((*short_index < max_short_index) && (*long_index < metadata->name_chars)) {
        if (metadata->name[*long_index] == PATH_DOT) {
            break;
        }
        if (metadata->name[*long_index] == PATH_SPACE) {
            (*long_index)++;
            continue;
        }
        
        if (metadata->name[*long_index] <= 0x7F) {
            // ascii character
            metadata->short_name[*short_index] = toupper((uint8_t)(metadata->name[*long_index] & 0x7F));
        } else {
            metadata->short_name[*short_index] = PATH_UNDERSCORE;
        }
        (*short_index)++;
        (*long_index)++;
    }
}

void set_short_name(struct DirEntry *dir_entry, struct EntryMetaData *metadata) {
    // TODO(zm): handle conflicts in short names...
    uint32_t short_index = 0;
    uint32_t long_index = 0;
        
    while (metadata->name[long_index] == PATH_DOT && long_index < metadata->name_chars) {
        long_index++;
    }
    const uint32_t long_index_start = long_index;

    _short_name_helper(metadata, &short_index, &long_index, 6);

    metadata->short_name[short_index] = PATH_TILDA;
    short_index++;
    metadata->short_name[short_index] = '1';
    short_index++;

    while (short_index < 8) {
        metadata->short_name[short_index] = PATH_SPACE;
        short_index++;
    }

    long_index = metadata->name_chars;
    while (long_index > long_index_start) {
        long_index--;
        if (metadata->name[long_index] == PATH_DOT) {
            _short_name_helper(metadata, &short_index, &long_index, 11);
            break;
        }
    }

    while (short_index < 11) {
        metadata->short_name[short_index] = PATH_SPACE;
        short_index++;
    }

    printf("[DEBUG] Generated Short Name: %.11s\n", metadata->short_name);

    metadata->name_checksum = name_checksum(metadata->short_name);
}

struct DirEntry * add_child_entry(struct DirEntry *dir_entry, struct EntryMetaData *metadata) {
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
    set_short_name(dir_entry, &new_dir_entry->metadata);

    struct DirEntry *child = dir_entry->child;
    dir_entry->child = new_dir_entry;

    new_dir_entry->parent = dir_entry;
    new_dir_entry->next = child;
    new_dir_entry->child = NULL;
    
    // construct cluster chain
    new_dir_entry->first_cluster = allocate_cluster_chain(new_dir_entry, new_dir_entry->metadata.size);
    return new_dir_entry;
}

void remove_child_entry(struct DirEntry *dir_entry, struct DirEntry *child_entry) {
    if (child_entry->metadata.is_dir == 1) {
        // recursively remove all subfolders/files
        struct DirEntry *cc = child_entry->child;
        while (cc != NULL) {
            remove_child_entry(child_entry, cc);
            cc = cc->next;
        }
    }
    
    // TODO(zm): maybe directory structure should be doubly-linked list?
    if (dir_entry->child == child_entry) {
       dir_entry->child = child_entry->next;
    } else {
        struct DirEntry *cc = dir_entry->child;
        while (cc->next != child_entry) {
            assert(cc != NULL);
            cc = cc->next;
        }
        cc->next = child_entry->next;
    }
    free_cluster_chain(child_entry->first_cluster);
    free(child_entry->metadata.name);
    free(child_entry);
}

struct DirEntry * get_child_entry(struct DirEntry *dir_entry, uint8_t name_chars, utf16_t *name) {
    struct DirEntry *child_entry = dir_entry->child;
    while (child_entry != NULL) {
        if ((child_entry->metadata.name_chars == name_chars) &&
            (memcmp(child_entry->metadata.name, name, name_chars * sizeof(utf16_t)) == 0)) {
            return child_entry;
        } else {
            child_entry = child_entry->next;
        }        
    }
    return NULL;
}

uint32_t get_dir_contents(struct DirEntry *dir_entry, struct DirEntry *child_entry, uint8_t *buf) {
    uint8_t long_entries = (child_entry->metadata.name_chars + LONG_NAME_CHARS_PER_ENTRY - 1) / LONG_NAME_CHARS_PER_ENTRY;
    uint8_t name_offset = (long_entries - 1) * LONG_NAME_CHARS_PER_ENTRY;
    uint32_t buf_offset = 0;

    utf16_t wll[LONG_NAME_CHARS_PER_ENTRY] = { 
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
            memcpy(wll, &child_entry->metadata.name[name_offset], LONG_NAME_CHARS_PER_ENTRY * sizeof(utf16_t));
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
        return 0;
    } else if (sector < (BPB_ReservedSectorCount + 2 * BPB_FATSz32)) {
        // Handle FAT Region
        uint32_t fat_sector;
        if (sector < (BPB_ReservedSectorCount + BPB_FATSz32)) {
            fat_sector = sector - BPB_ReservedSectorCount;
        } else {
            fat_sector = sector - BPB_ReservedSectorCount - BPB_FATSz32;
        }

        return read_fat_sector(fat_sector, buf);
    } else {
        // Handle Data Region
        uint32_t cluster_n = 2 + 
            (sector - (BPB_ReservedSectorCount + 2 * BPB_FATSz32)) / BPB_SectorsPerCluster;

        if (is_cluster_free(cluster_n)) {
            memset(buf, 0, BPB_BytesPerSector);
        } else {
            // Get DirEntry and offset of sector
            struct DirEntry *dir_entry = 
                get_cluster_dir_entry(cluster_n);
            uint32_t offset = 
                get_cluster_chain_size(dir_entry->first_cluster, cluster_n) + 
                (sector - ((cluster_n - 2) * BPB_SectorsPerCluster + (BPB_ReservedSectorCount + 2 * BPB_FATSz32))) * BPB_BytesPerSector;

            if (dir_entry->metadata.is_dir) {
                read_dir_sector(dir_entry, offset, buf);
            } else {
                memset(buf, 0, BPB_BytesPerSector);
            }
        }
        return 0;
    }
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

struct DirEntry * add_file_entry(uint32_t path_chars, utf16_t *path, struct DBMetaData *dbmetadata) {
    assert(path[0] == PATH_SEPARATOR);
    uint32_t path_last_index = 0;
    uint32_t path_index = 1;

    struct DirEntry *current_entry = ROOT_DIR_ENTRY;

    while (path_index < path_chars) {
        while ((path_index < path_chars) && (path[path_index] != PATH_SEPARATOR)) {
            path_index += 1;
        }
        
        // path:  path[0:path_last_index+1]
        // entry: path[path_last_index+1:path_index]
        uint8_t entry_name_chars = path_index - (path_last_index + 1);
        utf16_t *entry_name = &path[path_last_index + 1];
        struct DirEntry *child_entry = get_child_entry(current_entry, entry_name_chars, entry_name);

        if (path_index == path_chars) {
            if ((child_entry != NULL) && (child_entry->metadata.is_dir != dbmetadata->is_dir)) {
                remove_child_entry(current_entry, child_entry);
                child_entry = NULL;
            }

            if (child_entry == NULL) {
                // if file or directory does not exist create new one
                struct EntryMetaData metadata;
                metadata.is_dir = dbmetadata->is_dir;
                metadata.size = (dbmetadata->is_dir == 0) ? dbmetadata->size : 64;
                metadata.DIR_WrtDate = get_wrt_date(dbmetadata->mtime);
                metadata.DIR_WrtTime = get_wrt_time(dbmetadata->mtime); 

                metadata.name_chars = entry_name_chars;
                metadata.name = (utf16_t *)malloc(entry_name_chars * sizeof(utf16_t));
                memcpy(metadata.name, entry_name, entry_name_chars * sizeof(utf16_t));

                child_entry = add_child_entry(current_entry, &metadata);
            } else {
                // if file or directory already exists need to update metadata with new information
                if (child_entry->metadata.is_dir == 0) {
                    // for files need to update size and reallocate cluster chain
                    child_entry->metadata.size = dbmetadata->size;
                    child_entry->first_cluster = reallocate_cluster_chain(child_entry->first_cluster, child_entry->metadata.size);
                } 
                child_entry->metadata.DIR_WrtDate = get_wrt_date(dbmetadata->mtime);
                child_entry->metadata.DIR_WrtTime = get_wrt_time(dbmetadata->mtime); 
            }
        } else {
            // need to make sure that all parent directories in "path" exist if not 
            // as described in Dropbox Api "delta" protocol they must be created.
            // also if there is a file instead of directory in given "path" the file needs
            // to be removed.
            if ((child_entry != NULL) && (child_entry->metadata.is_dir == 0)) {
                remove_child_entry(current_entry, child_entry);
                child_entry = NULL;
            }

            if (child_entry == NULL) {
                struct EntryMetaData metadata;
                metadata.is_dir = 1;
                metadata.size = 64;
                metadata.DIR_WrtDate = get_wrt_date(dbmetadata->mtime);
                metadata.DIR_WrtTime = get_wrt_time(dbmetadata->mtime); 

                metadata.name_chars = entry_name_chars;
                metadata.name = (utf16_t *)malloc((entry_name_chars + 1) * sizeof(utf16_t));
                memcpy(metadata.name, entry_name, entry_name_chars * sizeof(utf16_t));

                child_entry = add_child_entry(current_entry, &metadata);
            }
        }

        path_last_index = path_index;
        path_index += 1;

        current_entry = child_entry;
    }
    return current_entry;
}

void remove_file_entry(uint32_t path_chars, utf16_t *path) {
    assert(path[0] == PATH_SEPARATOR);
    uint32_t path_last_index = 0;
    uint32_t path_index = 1;

    struct DirEntry *current_entry = ROOT_DIR_ENTRY;

    while (path_index < path_chars) {
        while ((path_index < path_chars) && (path[path_index] != PATH_SEPARATOR)) {
            path_index += 1;
        }

        // path:  path[0:path_last_index+1]
        // entry: path[path_last_index+1:path_index]
        uint8_t entry_name_chars = path_index - (path_last_index + 1);
        utf16_t *entry_name = &path[path_last_index + 1];
        struct DirEntry *child_entry = get_child_entry(current_entry, entry_name_chars, entry_name);

        if ((child_entry == NULL) || (path_index < path_chars && child_entry->metadata.is_dir == 0)) {
            return;
        }

        if (path_index == path_chars) {
            remove_child_entry(current_entry, child_entry);
        }
        current_entry = child_entry;
    }
}

void utf8_to_utf16(size_t utf8size, char *utf8string, size_t *utf16chars, utf16_t **utf16string) {
    const size_t BUF_SIZE = 64 * 1024;
    char OUTBUF[BUF_SIZE];
    char *outbuf = OUTBUF;
    size_t inbytesleft = utf8size;
    size_t outbytesleft = BUF_SIZE;

    iconv_t utf8_to_utf16 = iconv_open("UTF16LE", "UTF8");
    size_t r = iconv(utf8_to_utf16, &utf8string, &inbytesleft, &outbuf, &outbytesleft);
    assert(r != (size_t) -1);
    assert(inbytesleft == 0);

    assert(((BUF_SIZE - outbytesleft) % sizeof(utf16_t)) == 0);
    *utf16chars = (BUF_SIZE - outbytesleft) / sizeof(utf16_t);
    *utf16string = (utf16_t *)malloc((*utf16chars) * sizeof(utf16_t));
    memcpy(*utf16string, OUTBUF, (*utf16chars) * sizeof(utf16_t));
    iconv_close(utf8_to_utf16);
}

void add_test_file(char *path, uint8_t is_dir, uint32_t size, uint32_t mtime) {
    struct DBMetaData metadata = {
        .is_dir = is_dir,
        .size   = size,
        .mtime  = mtime,
    };

    utf16_t *utf16path;
    size_t utf16path_chars;
    utf8_to_utf16(strlen(path), path, &utf16path_chars, &utf16path);
    add_file_entry(utf16path_chars, utf16path, &metadata);
    free(utf16path);
}

void add_test_data() {
    add_test_file("/t1", 1, 0, 0);
    add_test_file("/t1/t2", 1, 0, 0);
    add_test_file("/t1/t2/t3", 0, 100, 0);
    add_test_file("/ttt1/ttt2/ttt3", 0, 1000, 0);
}

