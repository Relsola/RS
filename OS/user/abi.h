#ifndef RS_USER_ABI_H
#define RS_USER_ABI_H

/* ecall: a7 为系统调用号，a0 至 a2 为参数，a0 接收返回值。 */
#define RS_SYS_WRITE 1
#define RS_SYS_TICKS 2
#define RS_SYS_EXIT  3
#define RS_SYS_YIELD 4
#define RS_SYS_OPEN  5
#define RS_SYS_READ  6
#define RS_SYS_CLOSE 7
#define RS_SYS_MAP   8
#define RS_SYS_UNMAP 9
#define RS_SYS_FORK  10
#define RS_SYS_WAIT  11
#define RS_SYS_EXEC  12
#define RS_SYS_EXECV 13

#define RS_ARG_MAX_COUNT 8
#define RS_ARG_MAX_BYTES 256  /* 包括每个字符串末尾的 NUL。 */

/* 进入 user_main 时，a0 选择这次运行的用户入口行为。 */
#define RS_USER_HELLO     0
#define RS_USER_FAULT     1
#define RS_USER_YIELD_A   2
#define RS_USER_YIELD_B   3
#define RS_USER_PREEMPT_A 4
#define RS_USER_PREEMPT_B 5
#define RS_USER_FAULT_A   6
#define RS_USER_SURVIVE_B 7
#define RS_USER_MEMORY_A  8
#define RS_USER_MEMORY_B  9

#endif
