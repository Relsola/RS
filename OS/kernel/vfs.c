#include "vfs_internal.h"

typedef struct VfsMount {
    const char *name;
    const VfsOps *ops;
    const char *error;
} VfsMount;

/* 第 0 项挂在 /，后续项依名字挂在根的直接子目录。 */
static VfsMount mounts[] = {
    { "", &rsfs_ops, nullptr },
    { "proc", &procfs_ops, nullptr },
};
enum { MOUNT_COUNT = sizeof(mounts) / sizeof(mounts[0]) };

void vfs_mount(void)
{
    for (unsigned i = 0; i < MOUNT_COUNT; ++i) {
        const VfsOps *ops = mounts[i].ops;
        mounts[i].error = ops->mount ? ops->mount() : nullptr;
    }
}

bool vfs_name_equal(const char *left, const char *right)
{
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

static const VfsMount *child_mount(const VfsMount *mount, const void *directory, const char *name)
{
    if (mount == &mounts[0] && !directory) {
        for (unsigned i = 1; i < MOUNT_COUNT; ++i) {
            if (vfs_name_equal(mounts[i].name, name)) {
                return &mounts[i];
            }
        }
    }
    return nullptr;
}

typedef struct {
    const VfsMount *mount;
    VfsNode item;
} Location;

static const char *resolve(const char *path, Location *here)
{
    if (!*path) {
        return "empty path";
    }
    size_t length = 0;
    while (path[length]) {
        if (++length == VFS_PATH_BYTES) {
            return "path too long";
        }
    }
    /* 每深入一层至少消耗一个组件字符和一个分隔符（首组件可省分隔符）。 */
    Location parents[(VFS_PATH_BYTES + 1) / 2];
    unsigned depth = 0;
    *here = (Location){ &mounts[0], { nullptr, true } };
    while (*path) {
        if (!here->item.directory) {
            return "not a directory";  /* 包括普通文件名后的斜线或 /..。 */
        }
        while (*path == '/') {
            ++path;
        }
        if (!*path) {
            break;
        }
        char name[VFS_PATH_BYTES];
        size_t used = 0;
        while (*path && *path != '/') {
            name[used++] = *path++;
        }
        name[used] = '\0';
        if (vfs_name_equal(name, ".")) {
            continue;
        }
        if (vfs_name_equal(name, "..")) {
            if (depth) {
                *here = parents[--depth];
            }
            continue;
        }
        parents[depth++] = *here;
        const VfsMount *child = child_mount(here->mount, here->item.node, name);
        if (child) {
            *here = (Location){ child, { nullptr, true } };
            continue;
        }
        if (here->mount->error) {
            return here->mount->error;
        }
        if (!here->mount->ops->lookup(here->item.node, name, &here->item)) {
            return "file not found";
        }
    }
    return here->mount->error;
}

const char *vfs_opendir(const char *path, VfsDir *directory)
{
    Location location;
    const char *error = resolve(path, &location);
    if (error) {
        return error;
    }
    if (!location.item.directory) {
        return "not a directory";
    }
    *directory = (VfsDir){ location.mount, location.item.node, 0, 1 };
    return nullptr;
}

bool vfs_readdir(VfsDir *directory, VfsEntry *entry)
{
    const VfsMount *mount = directory->mount;
    while (mount->ops->entry_at(directory->node, directory->next, entry)) {
        ++directory->next;
        if (!child_mount(mount, directory->node, entry->name)) {
            return true;
        }
    }
    /* 原目录的同名项已经跳过。挂载目录在这里各列出一次。 */
    if (mount == &mounts[0] && !directory->node && directory->next_mount < MOUNT_COUNT) {
        *entry = (VfsEntry){ mounts[directory->next_mount++].name, true, 0 };
        return true;
    }
    return false;
}

const char *vfs_open(const char *path, VfsFile *file)
{
    Location location;
    const char *error = resolve(path, &location);
    if (error) {
        return error;
    }
    if (location.item.directory) {
        return "is a directory";
    }
    file->ops = location.mount->ops;
    file->node = location.item.node;
    file->offset = 0;
    file->ops->open(file);
    return nullptr;
}

const char *vfs_contents(const VfsFile *file, const uint8_t **bytes)
{
    return file->ops->contents(file, bytes);
}

const char *vfs_read(VfsFile *file, void *target, size_t length, size_t *actual)
{
    *actual = 0;
    size_t remaining = file->size - file->offset;
    if (length > remaining) {
        length = remaining;
    }
    if (!length) {
        return nullptr;
    }
    const uint8_t *bytes;
    const char *error = vfs_contents(file, &bytes);
    if (error) {
        return error;
    }
    uint8_t *out = target;
    for (size_t i = 0; i < length; ++i) {
        out[i] = bytes[file->offset + i];
    }
    file->offset += length;
    *actual = length;
    return nullptr;
}
