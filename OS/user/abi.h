#ifndef RS_USER_ABI_H
#define RS_USER_ABI_H

/* ecall: a7 为系统调用号，a0、a1 为参数，a0 接收返回值。 */
#define RS_SYS_WRITE 1
#define RS_SYS_TICKS 2
#define RS_SYS_EXIT  3
#define RS_SYS_YIELD 4

/* 进入 user_main 时，a0 选择这次运行的用户入口行为。 */
#define RS_USER_HELLO     0
#define RS_USER_FAULT     1
#define RS_USER_YIELD_A   2
#define RS_USER_YIELD_B   3
#define RS_USER_PREEMPT_A 4
#define RS_USER_PREEMPT_B 5
#define RS_USER_FAULT_A   6
#define RS_USER_SURVIVE_B 7

#endif
