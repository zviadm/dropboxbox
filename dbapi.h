#ifndef __DBAPI_H
#define __DBAPI_H

#include <curl/curl.h>
#include <stdint.h>

void start_dbapi_thread();
void dbapi_test();

int dbapi_get_file(CURL *curl, char *path, char *rev, long int range_start, long int range_end,
        char **response_buffer, size_t *response_size);
#endif
