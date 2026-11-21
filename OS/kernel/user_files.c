#include "user_files.h"
#include "paging.h"
#include "vfs.h"

enum { FIRST_FD = 3, FILES_PER_TASK = 4, READ_LIMIT = 256 };

typedef struct {
    unsigned references;
    VfsFile file;
} OpenFile;

/* 每个槽存的是引用。一次 open 有一份游标，fork 后两张表可以引用同一份。 */
static OpenFile *descriptors[USER_TASK_COUNT][FILES_PER_TASK];
static OpenFile open_files[USER_TASK_COUNT * FILES_PER_TASK];

static void drop_descriptor(OpenFile **descriptor)
{
    if (*descriptor) {
        --(*descriptor)->references;
        *descriptor = nullptr;
    }
}

void user_files_release(unsigned slot)
{
    for (unsigned i = 0; i < FILES_PER_TASK; ++i) {
        drop_descriptor(&descriptors[slot][i]);
    }
}

void user_files_clone(unsigned parent, unsigned child)
{
    /* 调度器给出一个空闲子槽。这里只增加引用，保留文件偏移与 procfs 的打开快照。 */
    for (unsigned i = 0; i < FILES_PER_TASK; ++i) {
        OpenFile *file = descriptors[parent][i];
        descriptors[child][i] = file;
        if (file) {
            ++file->references;
        }
    }
}

static OpenFile *lookup(unsigned slot, uint64_t fd)
{
    if (fd < FIRST_FD || fd >= FIRST_FD + FILES_PER_TASK) {
        return nullptr;
    }
    return descriptors[slot][fd - FIRST_FD];
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
    unsigned index = 0;
    while (index < FILES_PER_TASK && descriptors[slot][index]) {
        ++index;
    }
    if (index == FILES_PER_TASK) {
        return -1;
    }
    for (unsigned i = 0; i < USER_TASK_COUNT * FILES_PER_TASK; ++i) {
        OpenFile *entry = &open_files[i];
        if (!entry->references) {
            if (vfs_open(name, &entry->file)) {
                return -1;
            }
            entry->references = 1;
            descriptors[slot][index] = entry;
            return (long)(FIRST_FD + index);
        }
    }
    return -1;
}

long user_file_read(unsigned slot, uint64_t fd, uintptr_t destination, size_t count)
{
    uint8_t buffer[READ_LIMIT];
    OpenFile *entry = lookup(slot, fd);
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
    if (!lookup(slot, fd)) {
        return -1;
    }
    drop_descriptor(&descriptors[slot][fd - FIRST_FD]);
    return 0;
}
