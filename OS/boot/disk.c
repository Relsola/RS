#include "disk.h"
#include "platform.h"

#include <stddef.h>

/* Virtio 1.x MMIO transport, 32 位寄存器偏移按字节计。 */
enum {
    MAGIC = 0x00, VERSION = 0x04, DEVICE_ID = 0x08,
    DEVICE_FEATURES = 0x10, DEVICE_FEATURES_SEL = 0x14,
    DRIVER_FEATURES = 0x20, DRIVER_FEATURES_SEL = 0x24,
    QUEUE_SEL = 0x30, QUEUE_NUM_MAX = 0x34, QUEUE_NUM = 0x38,
    QUEUE_READY = 0x44, QUEUE_NOTIFY = 0x50,
    INTERRUPT_STATUS = 0x60, INTERRUPT_ACK = 0x64, STATUS = 0x70,
    QUEUE_DESC = 0x80, QUEUE_DRIVER = 0x90, QUEUE_DEVICE = 0xa0,
    CONFIG_GENERATION = 0xfc, CAPACITY_LOW = 0x100, CAPACITY_HIGH = 0x104,
};

enum {
    ACKNOWLEDGE = 1, DRIVER = 2, DRIVER_OK = 4, FEATURES_OK = 8, NEEDS_RESET = 64,
    FEATURE_READ_ONLY = 1 << 5, FEATURE_VERSION_1_HIGH = 1,
    DESC_NEXT = 1, DESC_WRITE = 2, QUEUE_SIZE = 8, MAX_READ_SECTORS = 8,
};

typedef struct {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
} Descriptor;

typedef struct {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[QUEUE_SIZE];
} AvailableRing;

typedef struct {
    uint32_t head;
    uint32_t length;
} UsedEntry;

typedef struct {
    uint16_t flags;
    uint16_t index;
    UsedEntry ring[QUEUE_SIZE];
} UsedRing;

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} Request;

static_assert(sizeof(Descriptor) == 16 && sizeof(Request) == 16);
static_assert(offsetof(AvailableRing, ring) == 4 && offsetof(UsedRing, ring) == 4);
static_assert(SECTOR_BYTES % RS_CACHE_BLOCK_BYTES == 0);

/* 每个成员从单独的缓存块开始，结构体尾部亦有填充。双方不在同一块里写不同字段。 */
typedef struct {
    alignas(RS_CACHE_BLOCK_BYTES) Descriptor descriptors[QUEUE_SIZE];
    alignas(RS_CACHE_BLOCK_BYTES) Request request;
    alignas(RS_CACHE_BLOCK_BYTES) volatile AvailableRing available;
    alignas(RS_CACHE_BLOCK_BYTES) volatile UsedRing used;
    alignas(RS_CACHE_BLOCK_BYTES) volatile uint8_t status;
} DmaQueue;

static DmaQueue queue;
static volatile uint32_t *device;
static uint64_t device_sectors;
static DiskStats stats;

DiskStats disk_stats(void)
{
    return stats;
}

static uint32_t read_reg(unsigned offset)
{
    return device[offset / sizeof(uint32_t)];
}

static void write_reg(unsigned offset, uint32_t value)
{
    device[offset / sizeof(uint32_t)] = value;
}

static void write_address(unsigned offset, uintptr_t value)
{
    write_reg(offset, (uint32_t)value);
    write_reg(offset + 4, (uint32_t)(value >> 32));
}

static bool find_disk(void)
{
    for (unsigned slot = 0; slot < 8; ++slot) {
        device = (volatile uint32_t *)(UINT64_C(0x10001000) + slot * 0x1000);
        if (read_reg(MAGIC) == UINT32_C(0x74726976) && read_reg(DEVICE_ID) == 2
            && read_reg(VERSION) == 2) {
            return true;
        }
    }
    return false;
}

static void reset_device(void)
{
    device_sectors = 0;
    write_reg(STATUS, 0);
    while (read_reg(STATUS) != 0) {
    }
}

bool disk_open(uint64_t *sectors)
{
    device_sectors = 0;
    if (!find_disk()) {
        return false;
    }
    reset_device();
    /* S 内核接管同一设备时重建自己的队列，重置后设备从索引 0 开始。 */
    queue.available.index = 0;
    queue.used.index = 0;
    write_reg(STATUS, ACKNOWLEDGE);
    write_reg(STATUS, ACKNOWLEDGE | DRIVER);

    write_reg(DEVICE_FEATURES_SEL, 1);
    if ((read_reg(DEVICE_FEATURES) & FEATURE_VERSION_1_HIGH) == 0) {
        return false;
    }
    write_reg(DRIVER_FEATURES_SEL, 1);
    write_reg(DRIVER_FEATURES, FEATURE_VERSION_1_HIGH);
    write_reg(DEVICE_FEATURES_SEL, 0);
    uint32_t low = read_reg(DEVICE_FEATURES) & FEATURE_READ_ONLY;
    write_reg(DRIVER_FEATURES_SEL, 0);
    write_reg(DRIVER_FEATURES, low);
    write_reg(STATUS, ACKNOWLEDGE | DRIVER | FEATURES_OK);
    if ((read_reg(STATUS) & FEATURES_OK) == 0) {
        return false;
    }

    write_reg(QUEUE_SEL, 0);
    if (read_reg(QUEUE_READY) != 0 || read_reg(QUEUE_NUM_MAX) < QUEUE_SIZE) {
        return false;
    }
    write_reg(QUEUE_NUM, QUEUE_SIZE);
    write_address(QUEUE_DESC, (uintptr_t)queue.descriptors);
    write_address(QUEUE_DRIVER, (uintptr_t)&queue.available);
    write_address(QUEUE_DEVICE, (uintptr_t)&queue.used);
    dma_clean(&queue.available, sizeof(queue.available));
    dma_flush(&queue.used, sizeof(queue.used));
    io_fence();
    write_reg(QUEUE_READY, 1);
    if (read_reg(QUEUE_READY) != 1) {
        return false;
    }
    write_reg(STATUS, ACKNOWLEDGE | DRIVER | FEATURES_OK | DRIVER_OK);

    uint32_t generation;
    do {
        generation = read_reg(CONFIG_GENERATION);
        uint32_t capacity_low = read_reg(CAPACITY_LOW);
        uint32_t capacity_high = read_reg(CAPACITY_HIGH);
        *sectors = ((uint64_t)capacity_high << 32) | capacity_low;
    } while (generation != read_reg(CONFIG_GENERATION));
    device_sectors = *sectors;
    return true;
}

static bool read_batch(uint64_t sector, uint32_t bytes, uint8_t *destination)
{
    /* 只发布一个请求。设备的写权限位按设备视角定义。 */
    queue.request.sector = sector;  /* type=0 表示 VIRTIO_BLK_T_IN。 */
    queue.status = 0xff;
    queue.descriptors[0] = (Descriptor){(uintptr_t)&queue.request, sizeof(Request), DESC_NEXT, 1};
    queue.descriptors[1] = (Descriptor){(uintptr_t)destination, bytes, DESC_NEXT | DESC_WRITE, 2};
    queue.descriptors[2] = (Descriptor){(uintptr_t)&queue.status, 1, DESC_WRITE, 0};

    uint16_t previous = queue.available.index;
    uint16_t next = (uint16_t)(previous + 1);
    unsigned slot = previous % QUEUE_SIZE;
    queue.available.ring[slot] = 0;
    dma_clean(&queue.request, sizeof(Request));
    dma_clean(queue.descriptors, 3 * sizeof(Descriptor));
    dma_clean(&queue.available, sizeof(queue.available));
    dma_flush(&queue.status, sizeof(queue.status));
    dma_flush(destination, bytes);
    io_fence();
    queue.available.index = next;
    dma_clean(&queue.available, sizeof(queue.available));
    io_fence();
    write_reg(QUEUE_NOTIFY, 0);

    for (;;) {
        dma_inval(&queue.used.index, sizeof(queue.used.index));
        io_fence();
        if (queue.used.index == next) {
            break;
        }
        if (read_reg(STATUS) & NEEDS_RESET) {
            reset_device(); /* 等设备停止 DMA，再把缓冲区交还给调用者。 */
            io_fence();
            dma_inval(destination, bytes);
            io_fence();
            return false;
        }
    }
    io_fence();
    dma_inval(&queue.used.ring[slot], sizeof(UsedEntry));
    dma_inval(&queue.status, sizeof(queue.status));
    dma_inval(destination, bytes);
    io_fence();
    uint32_t pending = read_reg(INTERRUPT_STATUS);
    if (pending) {
        write_reg(INTERRUPT_ACK, pending);
    }
    return queue.used.ring[slot].head == 0 && queue.used.ring[slot].length == bytes + 1
        && queue.status == 0;
}

bool disk_read_sectors(uint64_t sector, uint32_t count, void *destination)
{
    if (!count) {
        return true;
    }
    if (!destination || (uintptr_t)destination % RS_CACHE_BLOCK_BYTES ||
        sector >= device_sectors || count > device_sectors - sector) {
        return false;
    }
    uint8_t *out = destination;
    while (count) {
        uint32_t batch = count < MAX_READ_SECTORS ? count : MAX_READ_SECTORS;
        uint32_t bytes = batch * SECTOR_BYTES;
        ++stats.read_requests;
        if (!read_batch(sector, bytes, out)) {
            ++stats.read_errors;
            return false;
        }
        stats.read_sectors += batch;
        count -= batch;
        sector += batch;
        out += bytes;
    }
    return true;
}
