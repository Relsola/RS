#ifndef RS_TRAP_H
#define RS_TRAP_H

/* trap.S 按 x 寄存器的架构编号排列现场。帧大小是 16 的倍数，满足 C ABI。 */
#define RS_TRAP_EPC_OFFSET    256
#define RS_TRAP_STATUS_OFFSET 264
#define RS_TRAP_FRAME_BYTES   272

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t x[32];
    uint64_t epc;
    uint64_t status;
} TrapFrame;

static_assert(offsetof(TrapFrame, epc) == RS_TRAP_EPC_OFFSET);
static_assert(offsetof(TrapFrame, status) == RS_TRAP_STATUS_OFFSET);
static_assert(sizeof(TrapFrame) == RS_TRAP_FRAME_BYTES);

void timer_start(void);
uint64_t timer_ticks(void);
void run_user(bool fault);
const char *run_elf(const uint8_t *file, size_t size);
typedef enum { TASKS_YIELD, TASKS_PREEMPT, TASKS_FAULT } TaskDemo;
void run_tasks(TaskDemo demo);
#endif
#endif
