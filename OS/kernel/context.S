#include "context.h"

    .text
    .option push
    .option arch, +d, +v
    .globl enable_extensions
enable_extensions:
    li t0, RS_FS_INITIAL | RS_VS_INITIAL
    csrs sstatus, t0
    csrr t1, sstatus
    and t0, t0, t1
    andi t1, t0, RS_VS_INITIAL
    beqz t1, 1f
    csrr t1, vlenb
    sd t1, 0(a0)
1:  mv a0, t0
    ret

    .macro float_regs op
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        \op f\n, (\n*8)(a0)
    .endr
    .endm

    .globl save_float
save_float:
    float_regs fsd
    frcsr t0
    sd t0, RS_FP_CSR_OFFSET(a0)
    ret

    .globl restore_float
restore_float:
    float_regs fld
    ld t0, RS_FP_CSR_OFFSET(a0)
    fscsr t0
    ret

    # 整组传输不依赖 vl 或 vtype。它仍遵守 vstart，必须先保存并清零它。
    .macro vector_regs op
    csrr t0, vlenb
    slli t0, t0, 3
    \op v0, (a1)
    add a1, a1, t0
    \op v8, (a1)
    add a1, a1, t0
    \op v16, (a1)
    add a1, a1, t0
    \op v24, (a1)
    .endm

    .globl save_vector
save_vector:
    csrr t0, vl
    sd t0, 0(a0)
    csrr t0, vtype
    sd t0, 8(a0)
    csrrw t2, vstart, zero
    sd t2, 16(a0)
    csrr t0, vcsr
    sd t0, 24(a0)
    vector_regs vs8r.v
    csrw vstart, t2        # fork 快照之后可能仍由父进程继续运行。
    ret

    .globl restore_vector
restore_vector:
    csrw vstart, zero
    vector_regs vl8re8.v
    ld t0, 0(a0)
    ld t1, 8(a0)
    vsetvl t2, t0, t1
    ld t0, 24(a0)
    csrw vcsr, t0
    ld t0, 16(a0)
    csrw vstart, t0
    ret
    .option pop
