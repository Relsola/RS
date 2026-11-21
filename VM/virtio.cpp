#include "bus.h"

#include <array>

static constexpr unsigned QUEUE_MAX = 128;
static constexpr uint32_t READ_ONLY = 1u << 5; // feature word 0, VIRTIO_BLK_F_RO
static constexpr uint32_t VERSION_1 = 1;        // feature word 1, bit 32
static constexpr uint32_t DRIVER_OK = 4, FEATURES_OK = 8, NEEDS_RESET = 64, FAILED = 128;
static constexpr uint16_t NEXT = 1, WRITE = 2;
static constexpr uint8_t OK = 0, IO_ERROR = 1, UNSUPPORTED = 2;

struct Descriptor
{
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
};

static Descriptor descriptor_at(const uint8_t *table, unsigned index)
{
    const uint8_t *p = table + 16 * index;
    return {read_le(p, 8), static_cast<uint32_t>(read_le(p + 8, 4)),
            static_cast<uint16_t>(read_le(p + 12, 2)), static_cast<uint16_t>(read_le(p + 14, 2))};
}

struct Segment
{
    uint64_t address;
    uint32_t length;
};

static uint8_t read_sectors(Bus *bus, uint64_t sector, const Segment *data,
                            unsigned count, uint32_t total, uint32_t *written)
{
    if (total % 512 != 0 || sector > bus->sectors || total / 512 > bus->sectors - sector) {
        return IO_ERROR;
    }
    bus->disk.clear();
    bus->disk.seekg(static_cast<std::streamoff>(sector * 512));
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *bytes = ram_bytes(bus, data[i].address, data[i].length);
        bus->disk.read(reinterpret_cast<char *>(bytes), data[i].length);
        // 即使宿主文件在挂载后缩短，已经 DMA 到 RAM 的字节也会让重叠的 LR 失效。
        uint32_t transferred = static_cast<uint32_t>(bus->disk.gcount());
        reservation_after_write(&bus->reservation, data[i].address, transferred);
        *written += transferred;
        if (transferred != data[i].length) {
            return IO_ERROR;
        }
    }
    return OK;
}

// 支持直接描述符链：请求头 → 一个或多个数据片段 → 状态字节。
// 无法定位合法状态字节时，仍归还队列头，used.len = 0 表示没有写入。
static uint32_t complete_request(Bus *bus, const uint8_t *table, unsigned head)
{
    const unsigned size = bus->block.queue_size;
    if (head >= size) {
        return 0;
    }
    Descriptor desc = descriptor_at(table, head);
    uint8_t *header = nullptr;
    if (desc.length >= 16 && desc.flags == NEXT) {
        header = ram_bytes(bus, desc.address, 16);
    }
    const uint32_t type = header ? static_cast<uint32_t>(read_le(header, 4)) : 0;
    const uint64_t sector = header ? read_le(header + 8, 8) : 0;
    bool valid = header != nullptr;
    std::array<Segment, QUEUE_MAX> data;
    unsigned count = 0;
    uint32_t total = 0;
    // 最多读 size 个描述符，即使来宾提交循环链也能终止。
    for (unsigned i = 1; i < size && (desc.flags & NEXT) && desc.next < size; ++i) {
        desc = descriptor_at(table, desc.next);
        if (!(desc.flags & NEXT)) {
            if (desc.flags != WRITE || !bus_is_ram(bus, desc.address, 1) || desc.length < 1) {
                return 0;
            }
            uint8_t result = IO_ERROR;
            uint32_t written = 0;
            if (header && type > 1) {
                result = UNSUPPORTED;
            } else if (type == 0 && valid && count > 0) {
                result = read_sectors(bus, sector, data.data(), count, total, &written);
            }
            bus_store(bus, desc.address, 1, result);
            return written + 1;
        }
        if (desc.flags != (NEXT | WRITE) || !bus_is_ram(bus, desc.address, desc.length) ||
            desc.length >= UINT32_MAX - total) {
            valid = false;
        } else {
            data[count++] = {desc.address, desc.length};
            total += desc.length;
        }
    }
    return 0;
}

static void queue_fault(BlockDevice *block)
{
    block->status |= NEEDS_RESET;
    block->interrupts |= 2; // 配置变化：队列需要重新初始化。
}

static bool queue_shape_valid(const BlockDevice *block)
{
    uint32_t size = block->queue_size;
    return size != 0 && size <= QUEUE_MAX && (size & (size - 1)) == 0 &&
           block->descriptors % 16 == 0 && block->available % 2 == 0 && block->used % 4 == 0;
}

static void notify_queue(Bus *bus)
{
    BlockDevice *block = &bus->block;
    if (!block->ready || (block->status & (DRIVER_OK | FEATURES_OK | NEEDS_RESET | FAILED)) !=
                         (DRIVER_OK | FEATURES_OK)) {
        return;
    }
    const unsigned size = block->queue_size;
    const uint8_t *table = ram_bytes(bus, block->descriptors, 16 * size);
    const uint8_t *available = ram_bytes(bus, block->available, 4 + 2 * size);
    if (!table || !available || !bus_is_ram(bus, block->used, 4 + 8 * size)) {
        queue_fault(block);
        return;
    }
    // idx 是持续递增的 16 位计数，只有选择 ring[] 槽位时才按队列长度取模。
    uint16_t end = static_cast<uint16_t>(read_le(available + 2, 2));
    if (static_cast<uint16_t>(end - block->available_index) > size) {
        queue_fault(block);
        return;
    }
    while (block->available_index != end) {
        unsigned avail_slot = block->available_index % size;
        uint32_t head = static_cast<uint32_t>(read_le(available + 4 + 2 * avail_slot, 2));
        uint32_t written = complete_request(bus, table, head);
        uint64_t entry = block->used + 4 + 8 * (block->used_index % size);
        bus_store(bus, entry, 4, head);
        bus_store(bus, entry + 4, 4, written);
        ++block->available_index;
        ++block->used_index;
        bus_store(bus, block->used + 2, 2, block->used_index);
        if ((read_le(available, 2) & 1) == 0) {
            block->interrupts |= 1;
        }
    }
}

uint32_t block_read(const Bus *bus, uint32_t offset)
{
    const BlockDevice *block = &bus->block;
    switch (offset) {
    case 0x000: return 0x74726976; // "virt"
    case 0x004: return 2;
    case 0x008: return 2;          // virtio-blk
    case 0x00c: return 0x5253;     // RS
    case 0x010:
        if (block->feature_select == 0) return READ_ONLY;
        if (block->feature_select == 1) return VERSION_1;
        return 0;
    case 0x034: return block->queue_select == 0 ? QUEUE_MAX : 0;
    case 0x044: return block->queue_select == 0 && block->ready;
    case 0x060: return block->interrupts;
    case 0x070: return block->status;
    case 0x0fc: return 0;          // 配置在运行期间保持固定。
    case 0x100: return static_cast<uint32_t>(bus->sectors);
    case 0x104: return static_cast<uint32_t>(bus->sectors >> 32);
    default: return 0;
    }
}

static uint64_t set_half(uint64_t address, uint32_t half, bool high)
{
    if (high) {
        return (uint64_t{half} << 32) | static_cast<uint32_t>(address);
    }
    return (address & 0xffffffff00000000ull) | half;
}

void block_write(Bus *bus, uint32_t offset, uint32_t value)
{
    BlockDevice *block = &bus->block;
    switch (offset) {
    case 0x014: block->feature_select = value; return;
    case 0x020:
        if (block->driver_select < 2) {
            block->driver_features[block->driver_select] = value;
        }
        return;
    case 0x024: block->driver_select = value; return;
    case 0x030: block->queue_select = value; return;
    case 0x050:
        if (value == 0) {
            notify_queue(bus);
        }
        return;
    case 0x064: block->interrupts &= ~value; return;
    case 0x070:
        if (value == 0) {
            *block = {};
        } else {
            block->status = value | (block->status & NEEDS_RESET);
            if ((block->driver_features[0] & ~READ_ONLY) || block->driver_features[1] != VERSION_1) {
                block->status &= ~FEATURES_OK;
            }
        }
        return;
    }
    if (block->queue_select != 0) {
        return;
    }
    if (offset == 0x044) {
        if (value == 0) {
            block->ready = false;
        } else if (value == 1 && queue_shape_valid(block)) {
            block->ready = true;
        } else {
            queue_fault(block);
        }
        return;
    }
    if (block->ready) {
        return;
    }
    switch (offset) {
    case 0x038: block->queue_size = value; break;
    case 0x080: case 0x084: block->descriptors = set_half(block->descriptors, value, offset == 0x084); break;
    case 0x090: case 0x094: block->available = set_half(block->available, value, offset == 0x094); break;
    case 0x0a0: case 0x0a4: block->used = set_half(block->used, value, offset == 0x0a4); break;
    }
}
