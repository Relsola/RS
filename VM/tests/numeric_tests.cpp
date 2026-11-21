#include "bus.h"
#include "float_bits.h"
#include "vector_state.h"

#include <cstdio>
#include <cstdlib>

static void check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "numeric: %s\n", what);
        std::exit(1);
    }
}

static void expect(FloatResult r, uint64_t bits, unsigned flags, const char *what)
{
    if (r.bits != bits || r.flags != flags) {
        std::fprintf(stderr, "%s: bits=%llx flags=%u, expected %llx %u\n", what,
                     static_cast<unsigned long long>(r.bits), r.flags,
                     static_cast<unsigned long long>(bits), flags);
        std::exit(1);
    }
}

static void rounding()
{
    auto f = BINARY32, d = BINARY64;
    for (unsigned rm = 0; rm < 5; ++rm) {
        expect(fp_binary(f, FloatOp::Add, 0x3f800000, 0x33800000, rm), 0x3f800000 + (rm == 3 || rm == 4),
               FP_NX, "halfway above 1");
        expect(fp_binary(f, FloatOp::Sub, 0xbf800000, 0x33800000, rm), 0xbf800000 + (rm == 2 || rm == 4),
               FP_NX, "halfway below -1");
    }
    expect(fp_binary(f, FloatOp::Div, 1, 0x40000000, 0), 0, FP_UF | FP_NX, "subnormal midpoint");
    expect(fp_binary(f, FloatOp::Div, 2, 0x40000000, 0), 1, 0, "exact subnormal");
    expect(fp_binary(f, FloatOp::Mul, 0x7f7fffff, 0x40000000, 1), 0x7f7fffff, FP_OF | FP_NX,
           "directed overflow");
    expect(fp_binary(d, FloatOp::Div, 0xbff0000000000000, 0, 0), d.sign() | d.inf(), FP_DZ,
           "divide by zero");
    expect(fp_sqrt(d, 0x4000000000000000, 0), 0x3ff6a09e667f3bcd, FP_NX, "sqrt(2)");
    expect(fp_sqrt(f, 0x80000000, 4), 0x80000000, 0, "sqrt(-0)");
    expect(fp_fma(d, 0x3ff0000000000001, 0x3feffffffffffffe, 0xbff0000000000000, 0), 0xb970000000000000,
           0, "fused cancellation retains the exact product");
    expect(fp_fma(d, 0, d.inf(), d.nan(), 0), d.nan(), FP_NV, "0 * inf + qNaN");
    expect(fp_fma(d, 1, 1, 0x7fefffffffffffff, 3), d.inf(), FP_NX | FP_OF, "largest FMA exponent gap");
    expect(fp_binary(f, FloatOp::Min, 0x80000000, 0, 0), 0x80000000, 0, "signed zero minimum");
    expect(fp_binary(f, FloatOp::Eq, 0x7f800001, 0, 0), 0, FP_NV, "signaling NaN comparison");
    expect(fp_to_int(d, d.nan(), 32, false, 0), 0x7fffffff, FP_NV, "NaN integer saturation");
    expect(fp_to_int(f, 0xbe800000, 64, true, 1), 0, FP_NX, "negative fraction truncates to zero");
    expect(fp_from_int(d, UINT64_MAX, 64, true, 0), 0x43f0000000000000, FP_NX, "uint64 to double");
    expect(fp_convert(BINARY16, f, 0x3f800000, 0), 0x3c00, 0, "narrow to binary16");
}

static void step(Cpu &c, Bus &b, uint32_t insn)
{
    c.pc = RAM_BASE;
    check(bus_store(&b, c.pc, 4, insn), "install instruction");
    cpu_step(&c, &b);
}

static void access_and_restart()
{
    Bus b{};
    b.ram.resize(4096);
    Cpu c{};
    step(c, b, 0x301022f3); // csrr t0,misa
    constexpr uint64_t numeric = (1u << 3) | (1u << 5) | (1u << 21);
    check((c.x[5] & numeric) == numeric, "misa reports D, F and V");
    constexpr uint32_t add_s = 0x002081d3; // fadd.s f3,f1,f2,rne
    step(c, b, add_s);
    check(c.trapped && c.mcause == ILLEGAL_INSTRUCTION && c.mtval == add_s, "FS off");
    c.mstatus = 1u << 13;
    c.f[1] = 0x3f800000; // improperly boxed
    c.f[2] = fp_box(BINARY32, 0x40000000);
    step(c, b, add_s);
    check(!c.trapped && c.f[3] == fp_box(BINARY32, BINARY32.nan()) && (c.mstatus & FS) == FS,
          "NaN boxing and FS dirty");
    step(c, b, 0x100022f3); // csrr t0,sstatus
    check((c.x[5] >> 63) == 1, "SD reports dirty state");
    c.fcsr = 0xa0;
    step(c, b, add_s | 7 << 12);
    check(c.trapped && c.mcause == ILLEGAL_INSTRUCTION, "reserved dynamic rounding mode");

    constexpr uint32_t config = 0x0100f157; // vsetvli x2,x1,e32,m1,tu,mu
    c.x[1] = 4;
    step(c, b, config);
    check(c.trapped && c.mtval == config, "VS off");
    c.mstatus |= VS;
    step(c, b, config);
    check(!c.trapped && c.vl == 4 && c.x[2] == 4 && c.vtype == 16, "vsetvli e32,m1");
    c.x[1] = RAM_BASE + b.ram.size() - 8;
    check(bus_store(&b, c.x[1], 8, 0x2222222211111111), "last two mapped elements");
    uint32_t load = 1u << 25 | 1u << 15 | 6u << 12 | 8u << 7 | 7;
    step(c, b, load);
    check(c.trapped && c.mcause == LOAD_FAULT && c.mtval == RAM_BASE + 4096 && c.vstart == 2 &&
              vector_read(&c, 8, 1, 4) == 0x22222222,
          "precise vector element fault");
    b.ram.resize(8192);
    check(bus_store(&b, c.x[1], 4, 99) && bus_store(&b, c.x[1] + 8, 4, 33), "repair backing memory");
    step(c, b, load);
    check(!c.trapped && !c.vstart && vector_read(&c, 8, 0, 4) == 0x11111111 &&
              vector_read(&c, 8, 2, 4) == 33,
          "resume without replaying prefix");
    b.ram.resize(4096);
    step(c, b, load | (16u << 20));
    check(!c.trapped && c.vl == 2 && !c.vstart, "fault-only-first shortens vl");
    c.x[1] += 8;
    step(c, b, load | (16u << 20));
    check(c.trapped && c.mcause == LOAD_FAULT && c.vl == 2, "first element still traps");
    c.x[1] = 4;
    step(c, b, config);
    c.v[0][0] = 1;
    c.x[1] = RAM_BASE + 4092;
    step(c, b, load & ~(1u << 25));
    check(!c.trapped && vector_read(&c, 8, 1, 4) == 0x22222222,
          "masked-off unmapped addresses are not touched");
    c.x[1] = 1000;
    step(c, b, 0x00d0f157); // e16,mf8: unsupported SEW/LMUL.
    check(!c.trapped && c.vtype == VILL && !c.vl, "illegal vtype produces vill");
    step(c, b, 0x02410457);
    check(c.trapped, "vill rejects dependent arithmetic");
    c.x[1] = RAM_BASE + 512;
    step(c, b, (7u << 29) | (1u << 25) | (8u << 20) | (1u << 15) | (24u << 7) | 0x27);
    check(!c.trapped, "whole-register stores ignore vill");
}

static void register_groups()
{
    Bus b{};
    b.ram.resize(4096);
    Cpu c{};
    c.mstatus = VS;
    c.x[1] = UINT64_MAX;
    constexpr uint32_t set = 1u << 15 | 7u << 12 | 2u << 7 | 0x57;
    step(c, b, set | (3u << 20)); // e8,m8
    check(c.vl == 128 && c.x[2] == 128, "largest register group");
    for (unsigned i = 0; i < 128; ++i) {
        vector_write(&c, 8, i, 1, i);
    }
    c.vstart = 13;
    uint32_t add = 1u << 25 | 8u << 20 | 1u << 15 | 3u << 12 | 24u << 7 | 0x57; // vadd.vi v24,v8,1
    step(c, b, add);
    check(!c.trapped && !c.vstart && c.v[24][12] == 0 && c.v[24][13] == 14 && c.v[31][15] == 128,
          "LMUL=8 restart and last physical register");
    step(c, b, add | 1u << 7);
    check(c.trapped && c.mcause == ILLEGAL_INSTRUCTION, "unaligned destination group");
    step(c, b, set | (5u << 20)); // e8,mf8
    check(c.vl == 2 && c.vtype == 5, "fractional group");
    c.v[24][2] = 0x75;
    step(c, b, add);
    check(!c.trapped && c.v[24][1] == 2 && c.v[24][2] == 0x75, "fractional tail remains undisturbed");
    step(c, b, 0xc22f9073); // csrw vlenb,t6
    check(c.trapped && c.mcause == ILLEGAL_INSTRUCTION, "vector length in bytes is read-only");
}

int main()
{
    rounding();
    access_and_restart();
    register_groups();
}
