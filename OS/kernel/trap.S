#include "trap.h"

    .section .text.supervisor_trap, "ax"
    .balign 4

    # x2(sp) 与 x5(t0) 在入口最先保存，退出最后恢复。
    .macro frame_regs op
    \op ra,   1*8(sp)
    \op gp,   3*8(sp)
    \op tp,   4*8(sp)
    \op t1,   6*8(sp)
    \op t2,   7*8(sp)
    \op s0,   8*8(sp)
    \op s1,   9*8(sp)
    \op a0,  10*8(sp)
    \op a1,  11*8(sp)
    \op a2,  12*8(sp)
    \op a3,  13*8(sp)
    \op a4,  14*8(sp)
    \op a5,  15*8(sp)
    \op a6,  16*8(sp)
    \op a7,  17*8(sp)
    \op s2,  18*8(sp)
    \op s3,  19*8(sp)
    \op s4,  20*8(sp)
    \op s5,  21*8(sp)
    \op s6,  22*8(sp)
    \op s7,  23*8(sp)
    \op s8,  24*8(sp)
    \op s9,  25*8(sp)
    \op s10, 26*8(sp)
    \op s11, 27*8(sp)
    \op t3,  28*8(sp)
    \op t4,  29*8(sp)
    \op t5,  30*8(sp)
    \op t6,  31*8(sp)
    .endm

    # enter_tasks 最后像普通 C 函数一样返回 shell。整个任务组只有一个调用方。
    .equ CALL_BYTES, 128
    .equ CALL_GP, 8
    .equ CALL_TP, 16
    .equ CALL_STATUS, 120
    .macro call_regs op
    \op ra,    0(sp)
    \op gp,   CALL_GP(sp)
    \op tp,   CALL_TP(sp)
    \op s0,   24(sp)
    \op s1,   32(sp)
    \op s2,   40(sp)
    \op s3,   48(sp)
    \op s4,   56(sp)
    \op s5,   64(sp)
    \op s6,   72(sp)
    \op s7,   80(sp)
    \op s8,   88(sp)
    \op s9,   96(sp)
    \op s10, 104(sp)
    \op s11, 112(sp)
    .endm

    .globl supervisor_trap_entry
supervisor_trap_entry:
    # S 正在运行时 sscratch=0。U 正在运行时它保存可信的内核栈指针。
    csrrw sp, sscratch, sp
    bnez sp, .Lfrom_u
    csrrw sp, sscratch, sp
    addi sp, sp, -RS_TRAP_FRAME_BYTES
    sd t0, 5*8(sp)
    addi t0, sp, RS_TRAP_FRAME_BYTES
    j .Lsave
.Lfrom_u:
    addi sp, sp, -RS_TRAP_FRAME_BYTES
    sd t0, 5*8(sp)
    csrrw t0, sscratch, zero
.Lsave:
    sd t0, 2*8(sp)
    sd zero, 0(sp)
    frame_regs sd
    csrr t0, sepc
    sd t0, RS_TRAP_EPC_OFFSET(sp)
    csrr t0, sstatus
    sd t0, RS_TRAP_STATUS_OFFSET(sp)
    andi t0, t0, 1 << 8
    bnez t0, .Ldispatch
    # 用户可以自由修改 gp/tp。调用内核 C 代码前恢复内核 ABI 的值。
    lla t0, .Lshell_sp
    ld t0, 0(t0)
    ld gp, CALL_GP(t0)
    ld tp, CALL_TP(t0)
.Ldispatch:
    mv a0, sp
    csrr a1, scause
    csrr a2, stval
    call supervisor_dispatch
    beqz a0, .Lfinish_tasks

    # C 返回将要运行的现场，它可以位于另一任务的内核栈顶。
.Lresume:
    mv sp, a0
    ld t0, RS_TRAP_EPC_OFFSET(sp)
    csrw sepc, t0
    ld t0, RS_TRAP_STATUS_OFFSET(sp)
    csrw sstatus, t0
    andi t0, t0, 1 << 8
    bnez t0, .Lrestore
    addi t0, sp, RS_TRAP_FRAME_BYTES
    csrw sscratch, t0
.Lrestore:
    frame_regs ld
    ld t0, 5*8(sp)
    ld sp, 2*8(sp)
    sret

    .globl enter_tasks
enter_tasks:
    # 切换页表、内核栈和 sscratch 时关闭 S 级中断。
    csrrci t0, sstatus, 2
    addi sp, sp, -CALL_BYTES
    call_regs sd
    sd t0, CALL_STATUS(sp)
    lla t0, .Lshell_sp
    sd sp, 0(t0)
    call first_task_frame
    j .Lresume

.Lfinish_tasks:
    lla t0, .Lshell_sp
    ld sp, 0(t0)
    # 返回目标是内核 C 调用点。以 sret 原子地恢复 S 级与调用前的中断使能。
    ld t0, CALL_STATUS(sp)
    andi t1, t0, 2
    slli t1, t1, 4          # 原 SIE -> SPIE
    andi t0, t0, -35        # 清除 SIE 和 SPIE
    or t0, t0, t1
    ori t0, t0, 1 << 8
    csrw sstatus, t0
    call_regs ld
    addi sp, sp, CALL_BYTES
    csrw sepc, ra
    sret

    .globl discard_lr_reservation
discard_lr_reservation:
    # 一次 SC 会清除本 hart 的旧保留。即使成功，也只写本函数的临时字。
    addi sp, sp, -16
    sc.d zero, zero, (sp)
    addi sp, sp, 16
    ret

    .globl timer_read_time
timer_read_time:
    rdtime a0
    ret

    .globl timer_set_deadline
timer_set_deadline:
    csrw stimecmp, a0
    ret

    .globl timer_enable_interrupt
timer_enable_interrupt:
    li t0, 1 << 5
    csrs sie, t0
    csrsi sstatus, 2
    ret

    .bss
    .balign 8
.Lshell_sp:
    .dword 0
