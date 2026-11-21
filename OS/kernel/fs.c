#include "fs.h"
#include "vfs_internal.h"
#include "boot/disk.h"
#include "boot/disk_format.h"
#include "platform.h"

enum { TABLE_START = 32, RECORD_BYTES = 32, FILE_LIMIT = (SECTOR_BYTES - TABLE_START) / RECORD_BYTES };

static FsFile files[FILE_LIMIT];
static unsigned file_count;
alignas(RS_CACHE_BLOCK_BYTES) static uint8_t sector_buffer[SECTOR_BYTES];
alignas(FS_MAX_FILE_BYTES) uint8_t fs_file_cache[FS_MAX_FILE_BYTES];
static const FsFile *cached_file;
static_assert(FS_MAX_FILE_BYTES % SECTOR_BYTES == 0);

static bool magic_is(const uint8_t *field, const char *magic)
{
    for (unsigned i = 0; i < 8; ++i) {
        if (field[i] != (uint8_t)magic[i]) {
            return false;
        }
    }
    return true;
}

static bool zero_bytes(const uint8_t *bytes, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

/* 名称存储为 ASCII，最多 19 字节，包含终止零和尾部填零。 */
static bool decode_name(char *name, const uint8_t *record)
{
    for (unsigned i = 0; i < FS_NAME_BYTES; ++i) {
        uint8_t ch = record[i];
        name[i] = (char)ch;
        if (!ch) {
            return i != 0 && zero_bytes(record + i, FS_NAME_BYTES - i);
        }
        bool alphanumeric = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9');
        if (!alphanumeric && !(i && (ch == '-' || ch == '_' || ch == '.'))) {
            return false;
        }
    }
    return false;
}

static uint32_t sector_count(uint32_t size)
{
    return size / SECTOR_BYTES + (size % SECTOR_BYTES != 0);
}

static bool decode_record(unsigned index, const uint8_t *record, uint64_t first_data, uint64_t capacity)
{
    FsFile *file = &files[index];
    if (!decode_name(file->name, record)) {
        return false;
    }
    file->first_sector = load_u32_le(record + 20);
    file->size = load_u32_le(record + 24);
    file->crc32 = load_u32_le(record + 28);
    uint64_t start = file->first_sector;
    uint64_t end = start + sector_count(file->size);
    if (file->size > FS_MAX_FILE_BYTES || start < first_data || end > capacity ||
        (file->size == 0 && file->crc32 != 0)) {
        return false;
    }
    for (unsigned i = 0; i < index; ++i) {
        const FsFile *other = &files[i];
        uint64_t other_end = (uint64_t)other->first_sector + sector_count(other->size);
        if (vfs_name_equal(file->name, other->name) ||
            (file->size && other->size && start < other_end && other->first_sector < end)) {
            return false;
        }
    }
    return true;
}

static const char *fs_mount(void)
{
    uint64_t capacity;
    file_count = 0;
    cached_file = nullptr;
    if (!disk_open(&capacity) || !capacity || !disk_read_sectors(0, 1, sector_buffer)) {
        return "disk unavailable";
    }
    uint32_t kernel_size = load_u32_le(sector_buffer + 8);
    uint64_t directory = 1 + sector_count(kernel_size);
    if (!magic_is(sector_buffer, "RSDISK01") || !kernel_size || kernel_size > KERNEL_MAX_BYTES ||
        directory > capacity) {
        return "invalid disk header";
    }
    if (zero_bytes(sector_buffer + 16, 8)) {
        return nullptr;  /* 旧版 RSDISK01 只包含内核。 */
    }
    if (!magic_is(sector_buffer + 16, "RSFS0001") || directory == capacity) {
        return "invalid directory";
    }
    if (!disk_read_sectors(directory, 1, sector_buffer)) {
        return "directory read failed";
    }
    if (!magic_is(sector_buffer, "RSFS0001") ||
        crc32(sector_buffer + 12, SECTOR_BYTES - 12) != load_u32_le(sector_buffer + 8)) {
        return "invalid directory";
    }
    uint32_t count = load_u32_le(sector_buffer + 12);
    if (count > FILE_LIMIT || !zero_bytes(sector_buffer + 16, TABLE_START - 16)) {
        return "invalid directory";
    }
    for (unsigned i = 0; i < count; ++i) {
        if (!decode_record(i, sector_buffer + TABLE_START + i * RECORD_BYTES, directory + 1, capacity)) {
            return "invalid directory";
        }
    }
    unsigned used = TABLE_START + count * RECORD_BYTES;
    if (!zero_bytes(sector_buffer + used, SECTOR_BYTES - used)) {
        return "invalid directory";
    }
    file_count = count;  /* 全目录检查通过后才可供查询。 */
    return nullptr;
}

static const FsFile *fs_find(const char *name)
{
    for (unsigned i = 0; i < file_count; ++i) {
        if (vfs_name_equal(name, files[i].name)) {
            return &files[i];
        }
    }
    return nullptr;
}

static const char *fs_load(const FsFile *file, const uint8_t **bytes)
{
    *bytes = nullptr;
    if (file == cached_file) {
        *bytes = fs_file_cache;
        return nullptr;
    }
    /* DMA 会覆盖旧内容。遇到短读或 CRC 错误时，旧标签必须已经失效。 */
    cached_file = nullptr;
    /* 实际 DMA 以完整扇区为单位，静态缓冲区已按可接收的最大文件向上对齐。 */
    if (!disk_read_sectors(file->first_sector, sector_count(file->size), fs_file_cache)) {
        return "file read failed";
    }
    if (crc32(fs_file_cache, file->size) != file->crc32) {
        return "file CRC mismatch";
    }
    cached_file = file;
    *bytes = fs_file_cache;
    return nullptr;
}

static bool lookup(const void *parent, const char *name, VfsNode *found)
{
    const FsFile *file = parent ? nullptr : fs_find(name);
    if (!file) {
        return false;
    }
    *found = (VfsNode){ file, false };
    return true;
}

static bool entry_at(const void *parent, unsigned index, VfsEntry *entry)
{
    if (parent || index >= file_count) {
        return false;
    }
    const FsFile *file = &files[index];
    *entry = (VfsEntry){ file->name, false, file->size };
    return true;
}

static void open_file(VfsFile *file)
{
    const FsFile *disk_file = file->node;
    file->size = disk_file->size;
}

static const char *contents(const VfsFile *file, const uint8_t **bytes)
{
    return fs_load(file->node, bytes);
}

const VfsOps rsfs_ops = {
    .mount = fs_mount, .lookup = lookup, .entry_at = entry_at,
    .open = open_file, .contents = contents,
};
