#ifndef RS_DISK_H
#define RS_DISK_H

#include <stdint.h>

bool disk_open(uint64_t *sectors);
/* 调用者提供起点按缓存块对齐、可容纳 count 个完整扇区的独占 DMA 缓冲区。 */
bool disk_read_sectors(uint64_t sector, uint32_t count, void *destination);

typedef struct {
    uint64_t read_requests;
    uint64_t read_sectors;
    uint64_t read_errors;
} DiskStats;

DiskStats disk_stats(void);

#endif
