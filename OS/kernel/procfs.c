#include "paging.h"
#include "vfs_internal.h"

static const char meminfo[] = "meminfo";

static bool lookup(const void *parent, const char *name, VfsNode *found)
{
    if (parent) {
        return false;
    }
    unsigned i = 0;
    while (name[i] && name[i] == meminfo[i]) {
        ++i;
    }
    if (name[i] != meminfo[i]) {
        return false;
    }
    *found = (VfsNode){ meminfo, false };
    return true;
}

static bool entry_at(const void *parent, unsigned index, VfsEntry *entry)
{
    if (parent || index) {
        return false;
    }
    *entry = (VfsEntry){ meminfo, false, SIZE_MAX };
    return true;
}

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

static const char *contents(const VfsFile *file, const uint8_t **bytes)
{
    *bytes = (const uint8_t *)file->snapshot;
    return nullptr;
}

const VfsOps procfs_ops = {
    .lookup = lookup, .entry_at = entry_at,
    .open = open_meminfo, .contents = contents,
};
