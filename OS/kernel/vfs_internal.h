#ifndef RS_VFS_INTERNAL_H
#define RS_VFS_INTERNAL_H

#include "vfs.h"

/* node 是后端自己的对象标识，各挂载根目录的 node 均为 NULL。 */
typedef struct {
    const void *node;
    bool directory;
} VfsNode;

typedef struct VfsOps {
    const char *(*mount)(void);
    bool (*lookup)(const void *parent, const char *name, VfsNode *found);
    bool (*entry_at)(const void *parent, unsigned index, VfsEntry *entry);
    void (*open)(VfsFile *file);
    const char *(*contents)(const VfsFile *file, const uint8_t **bytes);
} VfsOps;

extern const VfsOps rsfs_ops;
extern const VfsOps procfs_ops;
bool vfs_name_equal(const char *left, const char *right);

#endif
