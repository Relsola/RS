#include "user/abi.h"

    .text
    .macro syscall name, number
    .section .text.\name, "ax"
    .globl \name
\name:
    li a7, \number
    ecall
    ret
    .endm

    syscall user_write, RS_SYS_WRITE
    syscall user_ticks, RS_SYS_TICKS
    syscall user_yield, RS_SYS_YIELD
    syscall user_open,  RS_SYS_OPEN
    syscall user_read,  RS_SYS_READ
    syscall user_close, RS_SYS_CLOSE
    syscall user_map,   RS_SYS_MAP
    syscall user_unmap, RS_SYS_UNMAP
    syscall user_fork,  RS_SYS_FORK
    syscall user_wait,  RS_SYS_WAIT
    syscall user_exec,  RS_SYS_EXEC

    .section .text.user_yield_reserved, "ax"
    .globl user_yield_reserved
user_yield_reserved:
    # 地址与原值随 A 的寄存器现场恢复，调用结果会使用 a0。
    mv t0, a0
    lr.w t1, (t0)
    li a7, RS_SYS_YIELD
    ecall
    sc.w a0, t1, (t0)
    ret

    .section .text.user_exit, "ax"
    .globl user_exit
user_exit:
    li a7, RS_SYS_EXIT
    ecall
    unimp
