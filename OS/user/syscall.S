#include "user/abi.h"

    .text
    .globl user_write
user_write:
    li a7, RS_SYS_WRITE
    ecall
    ret

    .globl user_ticks
user_ticks:
    li a7, RS_SYS_TICKS
    ecall
    ret

    .globl user_yield
user_yield:
    li a7, RS_SYS_YIELD
    ecall
    ret

    .globl user_yield_reserved
user_yield_reserved:
    # 地址与原值随 A 的寄存器现场恢复，调用结果会使用 a0。
    mv t0, a0
    lr.w t1, (t0)
    li a7, RS_SYS_YIELD
    ecall
    sc.w a0, t1, (t0)
    ret

    .globl user_exit
user_exit:
    li a7, RS_SYS_EXIT
    ecall
    unimp
