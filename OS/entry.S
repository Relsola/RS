    .section .text.entry, "ax"
    .balign 4
    .globl _start
_start:
    la sp, __stack_top
#ifdef RS_SUPERVISOR
    csrw sscratch, zero
    la t0, supervisor_trap_entry
    csrw stvec, t0
#ifdef __riscv_zicboz
    li t0, 1 << 7
    csrs senvcfg, t0       # 允许 U 模式在有写权限的内存上执行 cbo.zero。
#endif
#else
    la t0, .Ltrap
    csrw mtvec, t0
#endif

    # 镜像只包含有初值的字节，清零范围由链接器保证八字节对齐。
    la t0, __bss_start
    la t1, __bss_end
.Lclear:
    beq t0, t1, .Lmain
    sd zero, 0(t0)
    addi t0, t0, 8
    j .Lclear
.Lmain:
    tail guest_main

#ifndef RS_SUPERVISOR
    .balign 4
.Ltrap:
    csrr a0, mcause
    csrr a1, mepc
    csrr a2, mtval
    tail handle_trap
#endif

    .section .text.io_fence, "ax"
    .globl io_fence
io_fence:
    fence iorw, iorw
    ret

    .section .text.enter_kernel, "ax"
    .globl enter_kernel
enter_kernel:
    # PMP 第一项的 TOR 区间自 0 开始；给 S 模式访问本板 RAM 与设备的权限。
    li t0, -1
    csrw pmpaddr0, t0
    li t0, 0xf
    csrw pmpcfg0, t0

    # 将常见同步异常交给内核。S 模式 ecall 仍进入固件的 M 模式处理器。
    li t0, 0xb1ff
    csrw medeleg, t0
    # 允许 S 模式读取 time、设置 stimecmp，并将定时器中断交给 S 处理。
    csrsi mcounteren, 2
    li t0, (1 << 63) | (1 << 7)  # menvcfg.STCE 和 CBZE
    csrs menvcfg, t0
    # S 还须获准访问 senvcfg，才能自行授予 U 模式块清零权限。
    li t0, 1 << 62
    csrs mstateen0, t0
    csrr t1, misa
    andi t1, t1, (1 << 7)
    beqz t1, .Lstate_ready
    csrw hstateen0, zero   # 有 H 的参考机上，初始化下一级状态使能。
.Lstate_ready:
    li t0, 1 << 5
    csrs mideleg, t0
    csrw satp, zero
    li t0, (3 << 11)
    csrc mstatus, t0
    li t0, (1 << 11)
    csrs mstatus, t0
    csrw mepc, a0
    fence.i
    mret

    .section .text.install_page_table, "ax"
    .globl install_page_table
install_page_table:
    sfence.vma zero, zero
    csrw satp, a0
    sfence.vma zero, zero
    csrr a0, satp
    ret

#ifdef RS_SUPERVISOR
#include "user/layout.h"
    .section .text.zero_page, "ax"
    .globl zero_page
zero_page:
    # 调用者传入一个完整且按页对齐的 4 KiB 页。RVA23 的清零块为 64 字节。
    li t0, USER_PAGE_SIZE
    add t0, a0, t0
.Lzero_page:
#ifdef __riscv_zicboz
    cbo.zero (a0)
    addi a0, a0, 64
#else
    sd zero, 0(a0)
    addi a0, a0, 8
#endif
    bne a0, t0, .Lzero_page
    ret

    .section .text.invalidate_user_pages, "ax"
    .globl invalidate_user_page
invalidate_user_page:
    li a1, 1
    # 单页调用沿用下方的区间路径。
    .globl invalidate_user_pages
invalidate_user_pages:
    beqz a1, .Linval_done
    li t0, USER_PAGE_SIZE
    sfence.w.inval
.Linval_page:
    sinval.vma a0, zero
    add a0, a0, t0
    addi a1, a1, -1
    bnez a1, .Linval_page
    sfence.inval.ir
.Linval_done:
    ret
#endif
