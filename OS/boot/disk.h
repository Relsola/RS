#ifndef RS_DISK_H
#define RS_DISK_H

#include <stdint.h>

bool disk_open(uint64_t *sectors);
bool disk_read_sector(uint64_t sector, void *destination);

#endif
