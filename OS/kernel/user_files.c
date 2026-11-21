#include "user_files.h"
#include "paging.h"
#include "vfs.h"

enum { FIRST_FD = 3, FILES_PER_TASK = 4, READ_LIMIT = 256 };

typedef struct {
    bool open;
    VfsFile file;
} Descriptor;

static Descriptor descriptors[USER_TASK_COUNT][FILES_PER_TASK];

void user_files_release(unsigned slot)
{
    for (unsigned i = 0; i < FILES_PER_TASK; ++i) {
        descriptors[slot][i].open = false;
    }
}

static Descriptor *lookup(unsigned slot, uint64_t fd)
{
    if (fd < FIRST_FD || fd >= FIRST_FD + FILES_PER_TASK) {
        return nullptr;
    }
    Descriptor *entry = &descriptors[slot][fd - FIRST_FD];
    return entry->open ? entry : nullptr;
}

long user_file_open(unsigned slot, uintptr_t path, size_t length)
{
    char name[VFS_PATH_BYTES];
    if (!length || length >= sizeof(name) || !user_copy_in(slot, name, path, length)) {
        return -1;
    }
    for (size_t i = 0; i < length; ++i) {
        if (name[i] == '\0') {
            return -1;
        }
    }
    name[length] = '\0';
    for (unsigned i = 0; i < FILES_PER_TASK; ++i) {
        Descriptor *entry = &descriptors[slot][i];
        if (!entry->open) {
            if (vfs_open(name, &entry->file)) {
                return -1;
            }
            entry->open = true;
            return (long)(FIRST_FD + i);
        }
    }
    return -1;
}

long user_file_read(unsigned slot, uint64_t fd, uintptr_t destination, size_t count)
{
    uint8_t buffer[READ_LIMIT];
    Descriptor *entry = lookup(slot, fd);
    if (!entry || count > sizeof(buffer)) {
        return -1;
    }
    size_t saved_offset = entry->file.offset;
    size_t actual = 0;
    if (vfs_read(&entry->file, buffer, count, &actual)) {
        return -1;
    }
    /* 交给用户的字节与游标一起提交，调用者可以修正地址后重试。 */
    if (!user_copy_out(slot, destination, buffer, actual)) {
        entry->file.offset = saved_offset;
        return -1;
    }
    return (long)actual;
}

long user_file_close(unsigned slot, uint64_t fd)
{
    Descriptor *entry = lookup(slot, fd);
    if (!entry) {
        return -1;
    }
    entry->open = false;
    return 0;
}
