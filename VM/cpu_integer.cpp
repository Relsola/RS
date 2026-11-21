#include "cpu_integer.h"
#include "cpu_bitmanip.h"

#include <bit>
#include <limits>

static int64_t signed_value(uint64_t value)
{
    return std::bit_cast<int64_t>(value);
}

// 将两个 64 位数拆成 32 位的“数位”。先积累中间两列的进位，
// 再计算高半部，因此不依赖 __int128 或某个平台的乘法 intrinsic。
uint64_t mul_high_unsigned(uint64_t a, uint64_t b)
{
    const uint64_t a0 = static_cast<uint32_t>(a);
    const uint64_t b0 = static_cast<uint32_t>(b);
    const uint64_t a1 = a >> 32;
    const uint64_t b1 = b >> 32;
    const uint64_t carry0 = (a0 * b0) >> 32;
    const uint64_t column = a1 * b0 + carry0;
    const uint64_t carry1 = (a0 * b1 + static_cast<uint32_t>(column)) >> 32;
    return a1 * b1 + (column >> 32) + carry1;
}

static uint64_t mul_div(uint64_t a, uint64_t b, unsigned operation)
{
    switch (operation) {
    case 0: return a * b; // MUL
    case 1: // MULH
        return mul_high_unsigned(a, b) - (a >> 63 ? b : 0) - (b >> 63 ? a : 0);
    case 2: // MULHSU
        return mul_high_unsigned(a, b) - (a >> 63 ? b : 0);
    case 3: return mul_high_unsigned(a, b);
    case 4: // DIV
    case 6: { // REM
        const bool remainder = operation == 6;
        if (b == 0) {
            return remainder ? a : UINT64_MAX;
        }
        const int64_t dividend = signed_value(a);
        const int64_t divisor = signed_value(b);
        if (dividend == std::numeric_limits<int64_t>::min() && divisor == -1) {
            return remainder ? 0 : a;
        }
        return static_cast<uint64_t>(remainder ? dividend % divisor : dividend / divisor);
    }
    case 5: return b == 0 ? UINT64_MAX : a / b;
    default: return b == 0 ? a : a % b; // REMU
    }
}

bool cpu_integer(uint32_t instruction, uint64_t lhs, uint64_t rhs, uint64_t *result)
{
    if (cpu_bitmanip(instruction, lhs, rhs, result)) {
        return true;
    }
    const bool word = (instruction & 0x08) != 0;
    const bool immediate = (instruction & 0x20) == 0;
    const unsigned operation = (instruction >> 12) & 7;
    const unsigned funct7 = instruction >> 25;
    const unsigned shift_mask = word ? 31 : 63;
    uint64_t value = 0;

    if ((instruction & 0x7f) == 0x33 && funct7 == 7) { // Zicond，仅 XLEN 的寄存器形式。
        switch (operation) {
        case 5: *result = rhs == 0 ? 0 : lhs; return true; // CZERO.EQZ
        case 7: *result = rhs != 0 ? 0 : lhs; return true; // CZERO.NEZ
        default: return false;
        }
    }
    if (!immediate && funct7 == 1) {
        if (word) {
            if (operation >= 1 && operation <= 3) {
                return false; // MULH 没有 W 变种。
            }
            if (operation == 4 || operation == 6) {
                lhs = sign_extend(lhs, 32);
                rhs = sign_extend(rhs, 32);
            } else {
                lhs = static_cast<uint32_t>(lhs);
                rhs = static_cast<uint32_t>(rhs);
            }
        }
        value = mul_div(lhs, rhs, operation);
    } else {
        // R 型 funct7 的非零合法值只有 SUB/SRA 的 0x20。
        if (!immediate && funct7 != 0 &&
            (funct7 != 0x20 || (operation != 0 && operation != 5))) {
            return false;
        }
        if (word && operation != 0 && operation != 1 && operation != 5) {
            return false;
        }
        if (immediate) {
            rhs = sign_extend(instruction >> 20, 12);
        }
        // RV64 移位立即数有六位，W 指令只有五位。
        const unsigned shift_function = word || !immediate ? funct7 : funct7 & ~1u;
        const unsigned count = static_cast<unsigned>(rhs) & shift_mask;
        switch (operation) {
        case 0:
            value = !immediate && funct7 == 0x20 ? lhs - rhs : lhs + rhs;
            break;
        case 1:
            if (shift_function != 0) {
                return false;
            }
            value = lhs << count;
            break;
        case 2: value = signed_value(lhs) < signed_value(rhs); break;
        case 3: value = lhs < rhs; break;
        case 4: value = lhs ^ rhs; break;
        case 5:
            if (shift_function == 0) {
                value = (word ? static_cast<uint32_t>(lhs) : lhs) >> count;
            } else if (shift_function == 0x20) {
                value = static_cast<uint64_t>(signed_value(word ? sign_extend(lhs, 32) : lhs) >> count);
            } else {
                return false;
            }
            break;
        case 6: value = lhs | rhs; break;
        case 7: value = lhs & rhs; break;
        }
    }
    // RV64 的全部 W 指令都把结果的第 31 位扩展至第 63 位，包括 DIVUW。
    *result = word ? sign_extend(value, 32) : value;
    return true;
}
