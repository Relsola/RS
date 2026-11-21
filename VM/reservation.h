#pragma once

#include <stdint.h>

// 这块单 hart 板只需监视最近一次 LR 读到的物理字节。width=0 表示没有保留。
struct Reservation
{
    uint64_t address;
    unsigned width;
};

struct Bus;
Reservation *bus_reservation(Bus *bus);

// 输入均位于已验证的物理 RAM 中，两端点在该板的地址范围内。
inline void reservation_after_write(Reservation *reserved, uint64_t address, uint64_t size)
{
    if (size && reserved->width && address < reserved->address + reserved->width &&
        reserved->address < address + size) {
        reserved->width = 0;
    }
}
