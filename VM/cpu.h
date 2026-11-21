#pragma once

#include <stdint.h>

struct Bus;

// width 为字节数。失败的总线访问不得改变寄存器或设备。
bool bus_load(Bus *bus, uint64_t address, unsigned width, uint64_t *value);
bool bus_store(Bus *bus, uint64_t address, unsigned width, uint64_t value);
bool bus_is_ram(const Bus *bus, uint64_t address, unsigned width);

enum Privilege : unsigned { USER = 0, SUPERVISOR = 1, MACHINE = 3 };

// 原生解释器的单个 hart。复位时用 Cpu{}，由平台指定复位向量 pc。
struct Cpu
{
    uint64_t pc;
    uint64_t x[32];
    Privilege privilege = MACHINE;
    uint64_t mstatus;
    uint64_t medeleg;
    uint64_t mideleg;
    uint64_t mie;
    uint64_t menvcfg;
    uint64_t senvcfg;
    uint64_t mstateen[4];
    uint64_t software_stip; // 关闭 Sstc 时由 M 模式设置的挂起位。
    uint64_t mtvec;
    uint64_t mscratch;
    uint64_t mepc;
    uint64_t mcause;
    uint64_t mtval;
    uint64_t mcycle;
    uint64_t minstret;
    uint64_t counter_time;
    uint64_t stimecmp = UINT64_MAX;
    uint64_t mcounteren;
    uint64_t scounteren;
    uint64_t stvec, sscratch, sepc, scause, stval, satp;
    uint8_t pmpcfg[16];
    uint64_t pmpaddr[16];
    bool trapped;
};

void cpu_step(Cpu *cpu, Bus *bus);
