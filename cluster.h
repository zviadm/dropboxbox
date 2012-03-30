#ifndef __CLUSTER_H
#define __CLUSTER_H

#include <stdint.h>

#include "dbfat.h"

// FAT Entry Constants
#define FAT_FREE_ENTRY  0x00000000
#define FAT_EOFC_ENTRY  0x0FFFFFFF

void initialize_clusters(struct DirEntry *ROOT_DIR_ENTRY);
void cleanup_clusters();

uint32_t find_free_cluster();
int is_cluster_free(uint32_t cluster);

uint32_t allocate_cluster_chain(struct DirEntry *dir_entry, uint32_t size);
uint32_t reallocate_cluster_chain(uint32_t first_cluster, uint32_t new_size);
uint32_t get_cluster_chain_size(uint32_t first_cluster, uint32_t last_cluster);

struct DirEntry * get_cluster_dir_entry(uint32_t cluster);
int read_fat_sector(uint32_t fat_sector, uint8_t *buf);

#endif
