#pragma once

#include <stdint.h>

// bits 取 1..63；先截取字段，避免相邻指令位污染符号扩展。
constexpr uint64_t sign_extend(uint64_t value, unsigned bits)
{
    const uint64_t sign = uint64_t{1} << (bits - 1);
    const uint64_t mask = (sign << 1) - 1;
    return ((value & mask) ^ sign) - sign;
}

// 解码 OP / OP-IMM 及其 W 变种；保留编码返回 false。
bool cpu_integer(uint32_t instruction, uint64_t lhs, uint64_t rhs, uint64_t *result);
