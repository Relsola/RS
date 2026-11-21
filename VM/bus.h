#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <vector>

#include "console.h"
#include "reservation.h"

inline constexpr uint64_t RESET_PC = 0x1000;
inline constexpr uint64_t RAM_BASE = 0x80000000;
inline constexpr size_t RAM_SIZE = 128 * 1024 * 1024;
inline constexpr uint64_t UART_BASE = 0x10000000;
inline constexpr uint64_t VIRTIO_BASE = 0x10001000;
inline constexpr uint64_t FINISHER_BASE = 0x100000;

struct BlockDevice
{
    uint32_t feature_select, driver_select, driver_features[2];
    uint32_t queue_select, queue_size, status, interrupts;
    bool ready;
    uint16_t available_index, used_index;
    uint64_t descriptors, available, used;
};

struct Bus
{
    std::vector<uint8_t> ram;
    Reservation reservation;
    std::ifstream disk;
    uint64_t sectors;
    BlockDevice block;
    Console console;
    uint8_t uart_lcr, uart_ier, uart_dll, uart_dlm, uart_scratch;
    bool stopped;
    int exit_code;
};

// 返回的区域完全位于 RAM；设备的 DMA 不经过 MMIO 分发。
uint8_t *ram_bytes(Bus *bus, uint64_t address, size_t size);
bool bus_is_ram(const Bus *bus, uint64_t address, unsigned width);
uint64_t read_le(const uint8_t *p, unsigned width);
void write_le(uint8_t *p, unsigned width, uint64_t value);
bool bus_load(Bus *bus, uint64_t address, unsigned width, uint64_t *value);
bool bus_store(Bus *bus, uint64_t address, unsigned width, uint64_t value);
uint32_t block_read(const Bus *bus, uint32_t offset);
void block_write(Bus *bus, uint32_t offset, uint32_t value);
