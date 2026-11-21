#include "cpu_integer.h"

#include <cinttypes>
#include <cstdio>

struct Example
{
    uint32_t encoding;
    uint64_t lhs, rhs, expected;
};

// 编码取自 Clang 集成汇编器，目的寄存器 a0、输入 a1/a2。
// 独立的机器码可以查出执行器和测试误用同一套位字段常量的错误。
static constexpr Example EXAMPLES[] = {
    {0x20c5a533, 0x100000001, 3, 0x200000005}, // sh1add
    {0x20c5c533, 0x100000001, 3, 0x400000007}, // sh2add
    {0x20c5e533, 0x100000001, 3, 0x80000000b}, // sh3add
    {0x20c5a53b, 0x100000001, 3, 5}, // sh1add.uw
    {0x20c5c53b, 0x100000001, 3, 7}, // sh2add.uw
    {0x20c5e53b, 0x100000001, 3, 11}, // sh3add.uw
    {0x08c5853b, 0xffffffffffffffff, 1, 0x100000000}, // add.uw
    {0x0bf5951b, 0xffff00000001, 0, 0x8000000000000000}, // slli.uw
    {0x40c5f533, 0xf0, 0xc0, 0x30}, // andn
    {0x40c5e533, 0x0c, 0xff, 0xffffffffffffff0c}, // orn
    {0x40c5c533, 0xf0, 0xc0, 0xffffffffffffffcf}, // xnor
    {0x60059513, 0, 0, 64}, // clz
    {0x60159513, 0, 0, 64}, // ctz
    {0x60259513, 0xf0000000ff, 0, 12}, // cpop
    {0x6005951b, 0xf000000000, 0, 32}, // clzw
    {0x6015951b, 0xf000000000, 0, 32}, // ctzw
    {0x6025951b, 0xf0000000ff, 0, 8}, // cpopw
    {0x0ac5e533, UINT64_MAX, 9, 9}, // max
    {0x0ac5f533, UINT64_MAX, 9, UINT64_MAX}, // maxu
    {0x0ac5c533, UINT64_MAX, 9, UINT64_MAX}, // min
    {0x0ac5d533, UINT64_MAX, 9, 9}, // minu
    {0x60459513, 0x80, 0, 0xffffffffffffff80}, // sext.b
    {0x60559513, 0x18001, 0, 0xffffffffffff8001}, // sext.h
    {0x0805c53b, UINT64_MAX, 0, 0xffff}, // zext.h
    {0x60c59533, 1, 127, 0x8000000000000000}, // rol
    {0x60c5d533, 1, 65, 0x8000000000000000}, // ror
    {0x63f5d513, 0x8000000000000001, 0, 3}, // rori
    {0x60c5953b, 1, 63, 0xffffffff80000000}, // rolw
    {0x60c5d53b, 1, 33, 0xffffffff80000000}, // rorw
    {0x61f5d51b, 0x80000001, 0, 3}, // roriw
    {0x2875d513, 0x010000200300FF00, 0, 0xff0000ffff00ff00}, // orc.b
    {0x6b85d513, 0x0123456789abcdef, 0, 0xefcdab8967452301}, // rev8
    {0x48c59533, UINT64_MAX, 127, 0x7fffffffffffffff}, // bclr
    {0x4bf59513, UINT64_MAX, 0, 0x7fffffffffffffff}, // bclri
    {0x48c5d533, 0x8000000000000000, 127, 1}, // bext
    {0x4bf5d513, 0x8000000000000000, 0, 1}, // bexti
    {0x68c59533, UINT64_MAX, 127, 0x7fffffffffffffff}, // binv
    {0x6bf59513, 0, 0, 0x8000000000000000}, // binvi
    {0x28c59533, 0, 127, 0x8000000000000000}, // bset
    {0x2bf59513, 0, 0, 0x8000000000000000}, // bseti
};

int main()
{
    for (const Example &e : EXAMPLES) {
        uint64_t actual = 0;
        if (!cpu_integer(e.encoding, e.lhs, e.rhs, &actual) || actual != e.expected) {
            std::fprintf(stderr, "encoding %08" PRIx32 ": expected %016" PRIx64
                         ", got %016" PRIx64 "\n", e.encoding, e.expected, actual);
            return 1;
        }
    }
    // ZEXT.H 在 RV64 的 rs2 必须为 0；RORIW 的 shamt[5] 保留。
    constexpr uint32_t reserved[] = {0x0815c53b, 0x63f5d51b};
    for (uint32_t encoding : reserved) {
        uint64_t result;
        if (cpu_integer(encoding, 1, 2, &result)) {
            std::fprintf(stderr, "accepted reserved encoding %08" PRIx32 "\n", encoding);
            return 1;
        }
    }
    return 0;
}
