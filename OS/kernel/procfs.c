#include "paging.h"
#include "vfs_internal.h"
#include "boot/disk.h"

static char *append_metric(char *out, const char *name, uint64_t number)
{
    while (*name) {
        *out++ = *name++;
    }
    *out++ = ' ';
    char digits[20];
    unsigned used = 0;
    do {
        digits[used++] = (char)('0' + number % 10);
        number /= 10;
    } while (number);
    while (used) {
        *out++ = digits[--used];
    }
    *out++ = '\n';
    return out;
}

static void open_meminfo(VfsFile *file)
{
    /* 四个标签、四组空格和换行，每个无符号数的十进制表示最多二十位。 */
    static_assert(sizeof("pool_pagesresident_pagesreserved_pagesfaults") - 1 + 8 + 4 * 20
                  <= VFS_SNAPSHOT_BYTES);
    VmStats stats = vm_stats();
    char *p = append_metric(file->snapshot, "pool_pages", stats.pool_pages);
    p = append_metric(p, "resident_pages", stats.resident_pages);
    p = append_metric(p, "reserved_pages", stats.reserved_pages);
    p = append_metric(p, "faults", stats.faults);
    file->size = (size_t)(p - file->snapshot);
}

static void open_diskstats(VfsFile *file)
{
    static_assert(sizeof("read_requestsread_sectorsread_errors") - 1 + 6 + 3 * 20
                  <= VFS_SNAPSHOT_BYTES);
    DiskStats stats = disk_stats();
    char *p = append_metric(file->snapshot, "read_requests", stats.read_requests);
    p = append_metric(p, "read_sectors", stats.read_sectors);
    p = append_metric(p, "read_errors", stats.read_errors);
    file->size = (size_t)(p - file->snapshot);
}

typedef struct {
    const char *name;
    void (*snapshot)(VfsFile *file);
} ProcFile;

static const ProcFile files[] = {
    { "meminfo", open_meminfo },
    { "diskstats", open_diskstats },
};
enum { FILE_COUNT = sizeof(files) / sizeof(files[0]) };

static bool lookup(const void *parent, const char *name, VfsNode *found)
{
    if (parent) {
        return false;
    }
    for (unsigned i = 0; i < FILE_COUNT; ++i) {
        if (vfs_name_equal(name, files[i].name)) {
            *found = (VfsNode){ &files[i], false };
            return true;
        }
    }
    return false;
}

static bool entry_at(const void *parent, unsigned index, VfsEntry *entry)
{
    if (parent || index >= FILE_COUNT) {
        return false;
    }
    *entry = (VfsEntry){ files[index].name, false, SIZE_MAX };
    return true;
}

static void open_file(VfsFile *file)
{
    const ProcFile *node = file->node;
    node->snapshot(file);
}

static const char *contents(const VfsFile *file, const uint8_t **bytes)
{
    *bytes = (const uint8_t *)file->snapshot;
    return nullptr;
}

const VfsOps procfs_ops = {
    .lookup = lookup, .entry_at = entry_at,
    .open = open_file, .contents = contents,
};
