#pragma once

#include <stdint.h>

// 位运算接口也供向量浮点逐元素复用。调用者负责标量寄存器的 NaN-boxing。
struct FloatFormat {
    unsigned fraction, exponent;
    constexpr int bias() const { return (1 << (exponent - 1)) - 1; }
    constexpr uint64_t unit() const { return uint64_t{1} << fraction; }
    constexpr uint64_t sign() const { return unit() << exponent; }
    constexpr uint64_t inf() const { return sign() - unit(); }
    constexpr uint64_t nan() const { return inf() | (unit() >> 1); }
};
constexpr FloatFormat BINARY16{10, 5}, BINARY32{23, 8}, BINARY64{52, 11};
enum FloatFlag : unsigned { FP_NX = 1, FP_UF = 2, FP_OF = 4, FP_DZ = 8, FP_NV = 16 };
enum class FloatOp { Add, Sub, Mul, Div, Min, Max, Eq, Lt, Le };
struct FloatResult {
    uint64_t bits;
    unsigned flags;
};

uint64_t fp_unbox(FloatFormat f, uint64_t reg);
uint64_t fp_box(FloatFormat f, uint64_t bits);
unsigned fp_class(FloatFormat f, uint64_t bits);
FloatResult fp_binary(FloatFormat f, FloatOp op, uint64_t a, uint64_t b, unsigned rm);
FloatResult fp_fma(FloatFormat f, uint64_t a, uint64_t b, uint64_t c, unsigned rm);
FloatResult fp_sqrt(FloatFormat f, uint64_t a, unsigned rm);
uint64_t fp_immediate(FloatFormat f, unsigned index);
FloatResult fp_round_integral(FloatFormat f, uint64_t a, unsigned rm, bool exact);
FloatResult fp_mod_word(uint64_t a);
FloatResult fp_convert(FloatFormat to, FloatFormat from, uint64_t a, unsigned rm);
FloatResult fp_from_int(FloatFormat to, uint64_t a, unsigned width, bool unsign, unsigned rm);
FloatResult fp_to_int(FloatFormat from, uint64_t a, unsigned width, bool unsign, unsigned rm);
