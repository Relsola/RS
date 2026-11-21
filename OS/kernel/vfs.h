#ifndef RS_VFS_H
#define RS_VFS_H

#include <stddef.h>
#include <stdint.h>

enum { VFS_PATH_BYTES = 128, VFS_SNAPSHOT_BYTES = 144 };

/* 一次成功 open 拥有自己的偏移。生成文件的快照直接存在句柄内。 */
typedef struct {
    const struct VfsOps *ops;
    const void *node;
    size_t offset;
    size_t size;
    char snapshot[VFS_SNAPSHOT_BYTES];
} VfsFile;

typedef struct {
    const struct VfsMount *mount;
    const void *node;
    unsigned next;
    unsigned next_mount;
} VfsDir;

typedef struct {
    const char *name;
    bool directory;
    size_t size;  /* 生成文件在 open 时确定长度，目录项用 SIZE_MAX 表示。 */
} VfsEntry;

void vfs_mount(void);
const char *vfs_opendir(const char *path, VfsDir *directory);
bool vfs_readdir(VfsDir *directory, VfsEntry *entry);
const char *vfs_open(const char *path, VfsFile *file);
const char *vfs_read(VfsFile *file, void *target, size_t length, size_t *actual);
/* ELF 加载器可借用连续字节，须在下次读另一磁盘文件前完成复制。 */
const char *vfs_contents(const VfsFile *file, const uint8_t **bytes);

#endif
