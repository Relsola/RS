    .section .text.entry, "ax"
    .balign 4
    .globl _start
_start:
    la sp, __stack_top
#ifdef RS_SUPERVISOR
    csrw sscratch, zero
    la t0, supervisor_trap_entry
    csrw stvec, t0
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
    li t0, 1 << 63
    csrs menvcfg, t0
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

    .section .text.invalidate_user_page, "ax"
    .globl invalidate_user_page
invalidate_user_page:
    sfence.vma a0, zero
    ret
