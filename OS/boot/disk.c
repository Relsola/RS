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
    DESC_NEXT = 1, DESC_WRITE = 2, QUEUE_SIZE = 8,
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

static volatile uint32_t *device;
alignas(16) static Descriptor descriptors[QUEUE_SIZE];
alignas(2) static volatile AvailableRing available;
alignas(4) static volatile UsedRing used;
static Request request;
static volatile uint8_t completion_status;

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

bool disk_open(uint64_t *sectors)
{
    if (!find_disk()) {
        return false;
    }
    write_reg(STATUS, 0);
    while (read_reg(STATUS) != 0) {
    }
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
    write_address(QUEUE_DESC, (uintptr_t)descriptors);
    write_address(QUEUE_DRIVER, (uintptr_t)&available);
    write_address(QUEUE_DEVICE, (uintptr_t)&used);
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
    return true;
}

bool disk_read_sector(uint64_t sector, void *destination)
{
    /* 只发布一个请求。设备的写权限位按设备视角定义。 */
    request.sector = sector;  /* type=0 表示 VIRTIO_BLK_T_IN。 */
    completion_status = 0xff;
    descriptors[0] = (Descriptor){(uintptr_t)&request, sizeof(request), DESC_NEXT, 1};
    descriptors[1] = (Descriptor){(uintptr_t)destination, SECTOR_BYTES, DESC_NEXT | DESC_WRITE, 2};
    descriptors[2] = (Descriptor){(uintptr_t)&completion_status, 1, DESC_WRITE, 0};

    uint16_t previous = available.index;
    uint16_t next = (uint16_t)(previous + 1);
    unsigned slot = previous % QUEUE_SIZE;
    available.ring[slot] = 0;
    io_fence();
    available.index = next;
    io_fence();
    write_reg(QUEUE_NOTIFY, 0);

    while (used.index != next) {
        if (read_reg(STATUS) & NEEDS_RESET) {
            return false;
        }
    }
    io_fence();
    uint32_t pending = read_reg(INTERRUPT_STATUS);
    if (pending) {
        write_reg(INTERRUPT_ACK, pending);
    }
    return used.ring[slot].head == 0 && used.ring[slot].length == SECTOR_BYTES + 1
        && completion_status == 0;
}
