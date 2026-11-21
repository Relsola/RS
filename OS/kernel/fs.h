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

/* 成功返回空指针。只含内核的旧盘挂载为空目录。 */
const char *fs_mount(void);
unsigned fs_count(void);
const FsFile *fs_at(unsigned index);
const FsFile *fs_find(const char *name);
/* 校验成功后才交出文件字节。指针及其内容有效至下次 fs_load。 */
const char *fs_load(const FsFile *file, const uint8_t **bytes);

#endif
