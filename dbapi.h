#ifndef __DBAPI_H
#define __DBAPI_H

#include <stdint.h>
#include "dbfat.h"

struct DBMetaData {
    uint32_t size;
    uint32_t mtime;
    uint8_t is_dir;
    // TODO(zm): add rev
};

#endif
