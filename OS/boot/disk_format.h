#ifndef RS_DISK_FORMAT_H
#define RS_DISK_FORMAT_H

#include <stddef.h>
#include <stdint.h>

static inline uint32_t load_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8
        | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

/* 与制盘工具 zlib.crc32 一致，只覆盖有效字节。 */
static inline uint32_t crc32(const uint8_t *bytes, size_t count)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
        }
    }
    return ~crc;
}

#endif
