#ifndef __DBFILES_H
#define __DBFILES_H

void initialize_file_cache();
void cleanup_file_cache();

int read_sector_from_cache(size_t path_size, char *utf8path, char *rev, uint32_t offset, uint32_t file_size, uint8_t *buf);

#endif

