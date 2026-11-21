#ifndef RS_FS_H
#define RS_FS_H

#include <stdint.h>

enum { FS_NAME_BYTES = 20, FS_MAX_FILE_BYTES = 64 * 1024 };

/* DMA 使用物理地址，分页器为这块缓冲区建立等址的监管态读写映射。 */
alignas(FS_MAX_FILE_BYTES) extern uint8_t fs_file_cache[FS_MAX_FILE_BYTES];

typedef struct {
    char name[FS_NAME_BYTES];
    uint32_t first_sector;
    uint32_t size;
    uint32_t crc32;
} FsFile;

#endif
