#ifndef RS_FS_H
#define RS_FS_H

#include <stdint.h>

enum { FS_NAME_BYTES = 20, FS_MAX_FILE_BYTES = 64 * 1024 };

typedef struct {
    char name[FS_NAME_BYTES];
    uint32_t first_sector;
    uint32_t size;
    uint32_t crc32;
} FsFile;

#endif
