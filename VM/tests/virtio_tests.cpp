#include "bus.h"

#include <algorithm>
#include <array>
#include <cstdio>

static constexpr unsigned QUEUE_SIZE = 8;
static constexpr uint64_t TABLE = RAM_BASE + 0x100;
static constexpr uint64_t AVAILABLE = RAM_BASE + 0x200;
static constexpr uint64_t USED = RAM_BASE + 0x240;
static constexpr uint64_t HEADER = RAM_BASE + 0x300;
static constexpr uint64_t DATA = RAM_BASE + 0x400;
static constexpr uint64_t STATUS = RAM_BASE + 0x600;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (false)

static void write_descriptor(Bus *bus, unsigned index, uint64_t address, uint32_t length,
                             uint16_t flags, uint16_t next)
{
    uint8_t *p = ram_bytes(bus, TABLE + index * 16, 16);
    write_le(p, 8, address);
    write_le(p + 8, 4, length);
    write_le(p + 12, 2, flags);
    write_le(p + 14, 2, next);
}

static bool setup_queue(Bus *bus)
{
    auto reg = [bus](uint32_t offset, uint32_t value) {
        return bus_store(bus, VIRTIO_BASE + offset, 4, value);
    };
    CHECK(reg(0x070, 0)); // RESET / ACKNOWLEDGE / DRIVER
    CHECK(reg(0x070, 1));
    CHECK(reg(0x070, 3));
    CHECK(reg(0x024, 0));
    CHECK(reg(0x020, 1u << 5)); // VIRTIO_BLK_F_RO
    CHECK(reg(0x024, 1));
    CHECK(reg(0x020, 1));       // VIRTIO_F_VERSION_1
    CHECK(reg(0x070, 11));
    CHECK(reg(0x030, 0));
    CHECK(reg(0x038, QUEUE_SIZE));
    CHECK(reg(0x080, static_cast<uint32_t>(TABLE)));
    CHECK(reg(0x090, static_cast<uint32_t>(AVAILABLE)));
    CHECK(reg(0x0a0, static_cast<uint32_t>(USED)));
    CHECK(reg(0x044, 1));
    CHECK(reg(0x070, 15));
    uint64_t value;
    CHECK(bus_load(bus, VIRTIO_BASE + 0x070, 4, &value) && value == 15);
    return true;
}

static bool submit(Bus *bus, uint16_t sequence, uint16_t head, uint32_t expected_bytes)
{
    unsigned slot = (sequence - 1) % QUEUE_SIZE;
    write_le(ram_bytes(bus, AVAILABLE + 4 + 2 * slot, 2), 2, head);
    write_le(ram_bytes(bus, AVAILABLE + 2, 2), 2, sequence);
    CHECK(bus_store(bus, VIRTIO_BASE + 0x050, 4, 0));
    CHECK(read_le(ram_bytes(bus, USED + 2, 2), 2) == sequence);
    const uint8_t *entry = ram_bytes(bus, USED + 4 + 8 * slot, 8);
    CHECK(read_le(entry, 4) == head);
    CHECK(read_le(entry + 4, 4) == expected_bytes);
    uint64_t interrupts;
    CHECK(bus_load(bus, VIRTIO_BASE + 0x060, 4, &interrupts) && interrupts == 1);
    CHECK(bus_store(bus, VIRTIO_BASE + 0x064, 4, 1));
    CHECK(bus_load(bus, VIRTIO_BASE + 0x060, 4, &interrupts) && interrupts == 0);
    return true;
}

static bool exercise(const char *disk_path)
{
    Bus bus{};
    bus.ram.resize(0x1000);
    bus.disk.open(disk_path, std::ios::binary | std::ios::ate);
    std::streamoff length = bus.disk.tellg();
    CHECK(length >= 1024);
    bus.sectors = static_cast<uint64_t>(length) / 512;
    std::array<uint8_t, 512> expected;
    bus.disk.seekg(512);
    CHECK(bus.disk.read(reinterpret_cast<char *>(expected.data()), expected.size()));
    CHECK(setup_queue(&bus));

    uint8_t *data = ram_bytes(&bus, DATA, 512);
    uint8_t *header = ram_bytes(&bus, HEADER, 16);
    uint8_t *status = ram_bytes(&bus, STATUS, 1);
    write_descriptor(&bus, 0, HEADER, 16, 1, 1);
    write_descriptor(&bus, 1, DATA, 512, 3, 2);
    write_descriptor(&bus, 2, STATUS, 1, 2, 0);
    write_le(header + 8, 8, 1);
    *status = 0xff;
    bus.reservation = {DATA + 508, 4};
    CHECK(submit(&bus, 1, 0, 513));
    CHECK(*status == 0 && std::equal(expected.begin(), expected.end(), data));
    CHECK(bus.reservation.width == 0); // 最后一个字也被 DMA 写入。

    // 正好等于容量的 LBA 已在盘外。只更新状态，不触碰数据区。
    std::fill_n(data, 512, 0xa5);
    write_le(header + 8, 8, bus.sectors);
    bus.reservation = {DATA, 8};
    CHECK(submit(&bus, 2, 0, 1));
    CHECK(*status == 1 && std::all_of(data, data + 512, [](uint8_t b) { return b == 0xa5; }));
    CHECK(bus.reservation.width == 8); // 错误请求的状态字节与该保留无重叠。

    write_le(header, 4, 1); // OUT
    write_le(header + 8, 8, 1);
    write_descriptor(&bus, 1, DATA, 512, 1, 2);
    *status = 0xff;
    bus.reservation = {STATUS, 4};
    CHECK(submit(&bus, 3, 0, 1));
    CHECK(*status == 1 && bus.reservation.width == 0);
    write_le(header, 4, 0); // 随后读回同一扇区，验证只读语义。
    write_descriptor(&bus, 1, DATA, 512, 3, 2);
    bus.reservation = {USED, 4}; // used.idx 占用此字中的后两个字节。
    CHECK(submit(&bus, 4, 0, 513));
    CHECK(std::equal(expected.begin(), expected.end(), data) && bus.reservation.width == 0);

    write_descriptor(&bus, 1, UINT64_MAX - 10, 512, 3, 2);
    *status = 0xff;
    CHECK(submit(&bus, 5, 0, 1));
    CHECK(*status == 1);

    // 循环链没有状态缓冲区可写，仍应归还队列头；越界头应同样处理。
    write_descriptor(&bus, 1, DATA, 512, 3, 1);
    *status = 0xff;
    CHECK(submit(&bus, 6, 0, 0));
    CHECK(*status == 0xff);
    bus.reservation = {USED + 4 + 6 * 8, 4};
    CHECK(submit(&bus, 7, QUEUE_SIZE, 0));
    CHECK(bus.reservation.width == 0);

    // 模拟挂载后文件缩短：设备报告的末尾比实际文件多一个扇区。
    // 读两个扇区时只有前半段被 DMA 写入，完成长度也必须包含这些字节。
    write_le(header + 8, 8, bus.sectors - 1);
    ++bus.sectors;
    write_descriptor(&bus, 1, DATA, 1024, 3, 2);
    write_descriptor(&bus, 2, RAM_BASE + 0x900, 1, 2, 0);
    bus.reservation = {DATA + 512, 4};
    CHECK(submit(&bus, 8, 0, 513));
    CHECK(bus.reservation.width == 4 && *ram_bytes(&bus, RAM_BASE + 0x900, 1) == 1);
    bus.reservation = {DATA + 508, 4};
    CHECK(submit(&bus, 9, 0, 513));
    CHECK(bus.reservation.width == 0);

    // 队列本身指向 MMIO 时没有可归还的环，此时 DEVICE_NEEDS_RESET 可见。
    CHECK(bus_store(&bus, VIRTIO_BASE + 0x044, 4, 0));
    CHECK(bus_store(&bus, VIRTIO_BASE + 0x080, 4, UART_BASE));
    CHECK(bus_store(&bus, VIRTIO_BASE + 0x044, 4, 1));
    CHECK(bus_store(&bus, VIRTIO_BASE + 0x050, 4, 0));
    uint64_t device_status;
    CHECK(bus_load(&bus, VIRTIO_BASE + 0x070, 4, &device_status) && (device_status & 64));
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fputs("Usage: rs-virtio-tests disk.img\n", stderr);
        return 1;
    }
    return exercise(argv[1]) ? 0 : 1;
}
