#include "user/abi.h"

    .text
    .globl vector_dot
vector_dot:
    vsetivli zero, 1, e32, m1, tu, mu
    fmv.w.x ft0, zero
    vfmv.s.f v10, ft0
1:  beqz a2, 2f
    vsetvli t0, a2, e32, m1, tu, mu
    vle32.v v8, (a0)
    vle32.v v9, (a1)
    vfmul.vv v8, v8, v9
    vfredosum.vs v10, v8, v10
    sub a2, a2, t0
    slli t0, t0, 2
    add a0, a0, t0
    add a1, a1, t0
    j 1b
2:  vfmv.f.s ft0, v10
    fcvt.d.s fa0, ft0
    ret

    .globl numeric_pages
numeric_pages:
    # 首次向匿名区写入时，4 个元素分居两张尚未分配的页。
    vsetivli zero, 4, e32, m1, tu, mu
    vid.v v8
    vadd.vi v9, v8, 10
    vse32.v v9, (a0)
    # 分段读后用步长 8 分别写奇、偶元素。
    vsetivli zero, 2, e32, m1, tu, mu
    vlseg2e32.v v12, (a0)
    li t0, 8
    vsse32.v v12, (a1), t0
    addi t1, a1, 4
    vsse32.v v13, (t1), t0
    # 用索引倒序读，写到 scratch[4..7]。
    vsetivli zero, 4, e32, m1, tu, mu
    vrsub.vi v8, v8, 3
    vsll.vi v8, v8, 2
    vluxei32.v v12, (a0), v8
    addi t0, a1, 16
    vse32.v v12, (t0)
    # 末页最后一个字可读，下一个元素落在预留区外。硬件以 vl=1 返回。
    vle32ff.v v12, (a2)
    csrr t0, vl
    addi a0, t0, -1
    ret

    .globl numeric_initial
numeric_initial:
    frcsr t0
    csrr t1, vl
    or t0, t0, t1
    csrr t1, vstart
    or t0, t0, t1
    csrr t1, vcsr
    or t0, t0, t1
    csrr t1, vtype
    bgez t1, .Lnonzero
    fmv.x.d t1, ft11
    or t0, t0, t1
    vsetivli zero, 1, e64, m1, tu, mu
    vmv.x.s t1, v31
    or t0, t0, t1
    snez a0, t0
    ret
.Lnonzero:
    li a0, 1
    ret

    .macro mark value, flags, fixed
    li t0, \value
    fmv.d.x ft11, t0
    vsetivli zero, 3, e32, m1, tu, mu
    vmv.v.x v31, t0
    li t0, \flags
    fscsr t0
    csrwi vcsr, \fixed
    csrwi vstart, 1
    .endm

    .macro verify value, flags, fixed, failure
    csrr t0, vstart
    li t1, 1
    bne t0, t1, \failure
    csrr t0, vl
    li t1, 3
    bne t0, t1, \failure
    csrr t0, vtype
    li t1, 16                       # e32,m1,tu,mu
    bne t0, t1, \failure
    csrr t0, vcsr
    li t1, \fixed
    bne t0, t1, \failure
    frcsr t0
    li t1, \flags
    bne t0, t1, \failure
    fmv.x.d t0, ft11
    li t1, \value
    bne t0, t1, \failure
    csrwi vstart, 0
    vmv.x.s t0, v31
    bne t0, t1, \failure
    csrwi vstart, 1
    .endm

    .globl numeric_context
numeric_context:
    addi sp, sp, -48
    sd s0, 0(sp)
    sd s1, 8(sp)
    sd s2, 24(sp)
    mv s0, a0
    mv s2, a1
    mark 101, 0x45, 5
    li a7, RS_SYS_FORK
    ecall
    bltz a0, .Lbad
    beqz a0, .Lchild
    mv s1, a0
    mark 202, 0x62, 2
    # 等定时器经过四拍，期间父进程从不 yield 或 wait。
    li a7, RS_SYS_TICKS
    ecall
    addi t3, a0, 4
1:  ecall
    bltu a0, t3, 1b
    verify 202, 0x62, 2, .Lbad
    mv a0, s0
    addi a1, sp, 16
    li a2, 1
    li a7, RS_SYS_READ
    ecall
    li t0, 1
    bne a0, t0, .Lbad
    lbu t0, 16(sp)
    li t1, 'e'                      # 子进程已在抢占时间片中读走 W。
    bne t0, t1, .Lbad
    mv a0, s1
    li a7, RS_SYS_WAIT
    ecall
    bnez a0, .Lbad
    verify 202, 0x62, 2, .Lbad
    li a0, 0
    j .Lreturn
.Lbad:
    li a0, 4
.Lreturn:
    csrwi vstart, 0
    fscsr zero
    ld s0, 0(sp)
    ld s1, 8(sp)
    ld s2, 24(sp)
    addi sp, sp, 48
    ret
.Lchild:
    verify 101, 0x45, 5, .Lchild_bad
    vsetivli zero, 4, e32, m1, tu, mu
    vmv.v.i v8, -1
    vse32.v v8, (s2)               # 两个 COW 页各自拆分，父进程保留 10..13。
    lw t0, 12(s2)
    li t1, -1
    bne t0, t1, .Lchild_bad
    mv a0, s0
    addi a1, sp, 16
    li a2, 1
    li a7, RS_SYS_READ
    ecall
    li t0, 1
    bne a0, t0, .Lchild_bad
    lbu t0, 16(sp)
    li t1, 'W'
    bne t0, t1, .Lchild_bad
    mark 303, 0x24, 7
    li a7, RS_SYS_YIELD
    ecall
    verify 303, 0x24, 7, .Lchild_bad
    la a0, .Lself
    li a1, 12
    la a2, .Largs
    li a7, RS_SYS_EXECV
    ecall
.Lchild_bad:
    li a0, 5
    li a7, RS_SYS_EXIT
    ecall

    .section .rodata
.Lself: .asciz "/numeric.elf"
.Lfresh: .asciz "fresh"
    .balign 8
.Largs: .dword .Lself, .Lfresh, 0
