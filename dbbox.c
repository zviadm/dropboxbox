#define _POSIX_C_SOURCE 201001L
#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "dbapi.h"
#include "dbfat.h"

const char *DBBOX_PATH = "/dbbox.img";
const off_t DBBOX_SIZE = BPB_TotalSectors * BPB_BytesPerSector;

static int dbbox_getattr(const char *path, struct stat *stbuf)
{
    int res = 0;

    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else if (strcmp(path, DBBOX_PATH) == 0) {
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = DBBOX_SIZE;
    } else {
        res = -ENOENT;
    }

    return res;
}

static int dbbox_readdir(
        const char *path, 
        void *buf, 
        fuse_fill_dir_t filler,
        off_t offset, 
        struct fuse_file_info *fi)
{
    (void) offset;
    (void) fi;

    if (strcmp(path, "/") != 0) {
        return -ENOENT;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    filler(buf, &DBBOX_PATH[1], NULL, 0);
    return 0;
}

static int dbbox_open(const char *path, struct fuse_file_info *fi)
{
    if (strcmp(path, DBBOX_PATH) != 0) {
        return -ENOENT;
    }

    if ((fi->flags & 3) != O_RDONLY) {
        return -EACCES;
    }

    return 0;
}

static int dbbox_read(
        const char *path, 
        char *buf, 
        size_t size, 
        off_t offset,
        struct fuse_file_info *fi)
{
    (void) fi;
    if(strcmp(path, DBBOX_PATH) != 0) {
        return -ENOENT;
    }

    if (offset < DBBOX_SIZE) {
        if (offset + size > DBBOX_SIZE) {
            size = DBBOX_SIZE - offset;
        }
        read_data((uint32_t)offset, (uint32_t)size, (uint8_t *)buf);
    } else{
        size = 0;
    }

    return size;
}

static struct fuse_operations dbbox_oper = {
    .getattr = dbbox_getattr,
    .readdir = dbbox_readdir,
    .open    = dbbox_open,
    .read    = dbbox_read,
};

int main(int argc, char *argv[])
{
    initialize();
    dbapi_test();
    return fuse_main(argc, argv, &dbbox_oper, NULL);
}
