#include "trap.h"
#include "paging.h"
#include "platform.h"
#include "user/abi.h"

enum {
    TIMER_PERIOD = 32768,
    STATUS_SPIE = 1 << 5,
    STATUS_SPP = 1 << 8,
    U_ECALL = 8,
    REG_A0 = 10, REG_A1 = 11, REG_A7 = 17,
};

/* 这台板有一个 hart，仅在 S 中断关闭时写入。shell 和系统调用可在其余时间读取。 */
static volatile uint64_t tick_count;
static uint64_t next_deadline;

uint64_t timer_read_time(void);
void timer_set_deadline(uint64_t deadline);
void timer_enable_interrupt(void);
void enter_tasks(void);
void discard_lr_reservation(void);
void sync_user_instructions(void);

typedef struct {
    bool runnable;
    /* U 陷阱从 frame 顶端向下增长，C 分派函数使用其下方私有的内核栈。 */
    alignas(16) uint8_t kernel_stack[8192];
    TrapFrame frame;
} Task;

static_assert(USER_TASK_COUNT == 2);
static_assert(offsetof(Task, frame) % 16 == 0);
static Task tasks[USER_TASK_COUNT];
static unsigned current;
static bool running_pair;
static bool time_sliced;

void timer_start(void)
{
    next_deadline = timer_read_time() + TIMER_PERIOD;
    timer_set_deadline(next_deadline);
    timer_enable_interrupt();
}

uint64_t timer_ticks(void)
{
    return tick_count;
}

static void prepare_frame(unsigned slot, uintptr_t entry, unsigned program_mode)
{
    Task *task = &tasks[slot];
    for (unsigned i = 0; i < 32; ++i) {
        task->frame.x[i] = 0;
    }
    task->frame.x[2] = USER_STACK_TOP;
    task->frame.x[REG_A0] = program_mode;
    task->frame.epc = entry;
    task->frame.status = STATUS_SPIE; /* SPP=U，sret 后恢复 S 级中断使能。 */
    task->runnable = true;
}

static void enter_single_task(uintptr_t entry, unsigned program_mode)
{
    running_pair = false;
    time_sliced = false;
    prepare_frame(0, entry, program_mode);
    tasks[1].runnable = false;
    enter_tasks();
}

void run_user(bool fault)
{
    user_prepare(0);
    enter_single_task(USER_ENTRY, fault ? RS_USER_FAULT : RS_USER_HELLO);
}

const char *run_elf(const uint8_t *file, size_t size)
{
    ElfImage image;
    const char *error = elf_parse(file, size, &image);
    if (error) {
        return error;
    }
    user_prepare_elf(&image);
    sync_user_instructions();
    enter_single_task(image.entry, 0);
    return nullptr;
}

void run_tasks(TaskDemo demo)
{
    static const unsigned modes[][2] = {
        [TASKS_YIELD]   = {RS_USER_YIELD_A, RS_USER_YIELD_B},
        [TASKS_PREEMPT] = {RS_USER_PREEMPT_A, RS_USER_PREEMPT_B},
        [TASKS_FAULT]   = {RS_USER_FAULT_A, RS_USER_SURVIVE_B},
    };
    running_pair = true;
    time_sliced = demo == TASKS_PREEMPT;
    for (unsigned slot = 0; slot < USER_TASK_COUNT; ++slot) {
        user_prepare(slot);
        prepare_frame(slot, USER_ENTRY, modes[demo][slot]);
    }
    enter_tasks();
}

static TrapFrame *activate_task(unsigned slot)
{
    discard_lr_reservation();
    user_activate(slot);
    current = slot;
    return &tasks[slot].frame;
}

/* 汇编已保存 shell 现场并关中断，首次 sret 也走正常恢复路径。 */
TrapFrame *first_task_frame(void)
{
    if (time_sliced) {
        next_deadline = timer_read_time() + TIMER_PERIOD;
        timer_set_deadline(next_deadline);
    }
    return activate_task(0);
}

/* 调度点只出现在 U 陷阱中。切换后各根页表仍以同一物理地址映射内核栈。 */
static TrapFrame *schedule(const char *reason, uint64_t epc)
{
    unsigned next = current ^ 1;
    if (tasks[next].runnable) {
        if (reason) {
            uart_puts(current == 0 ? "switch A -> B: " : "switch B -> A: ");
            uart_puts(reason);
            uart_puts(" epc=");
            uart_put_hex(epc);
            uart_puts("\n");
        }
        return activate_task(next);
    }
    if (tasks[current].runnable) {
        return &tasks[current].frame;
    }
    discard_lr_reservation();
    kernel_activate();
    return nullptr; /* 本组结束，让汇编恢复 shell。 */
}

static const char *task_label(void)
{
    if (!running_pair) {
        return "user";
    }
    return current == 0 ? "task A" : "task B";
}

static uint64_t sys_write(uintptr_t address, uint64_t length)
{
    uint8_t text[256];
    if (length > sizeof(text)) {
        return UINT64_MAX;
    }
    if (!length) {
        return 0;
    }
    if (!user_copy_in(current, text, address, (size_t)length)) {
        return UINT64_MAX;
    }
    for (size_t i = 0; i < (size_t)length; ++i) {
        if (text[i] == '\n') {
            uart_putc('\r');
        }
        uart_putc((char)text[i]);
    }
    return length;
}

TrapFrame *supervisor_dispatch(TrapFrame *frame, uint64_t cause, uint64_t value)
{
    const uint64_t interrupt = UINT64_C(1) << 63;
    if (cause == (interrupt | 5)) {
        uint64_t now = timer_read_time();
        /* 更新比较值后，硬件仍可能短暂保留旧的 pending 信号。 */
        if (now >= next_deadline) {
            ++tick_count;
            next_deadline = now + TIMER_PERIOD;
            timer_set_deadline(next_deadline);
            if (time_sliced && !(frame->status & STATUS_SPP)) {
                return schedule("timer", frame->epc);
            }
        }
        return frame;
    }
    if ((cause & interrupt) || (frame->status & STATUS_SPP)) {
        handle_trap(cause, frame->epc, value);
    }
    if (cause != U_ECALL) {
        uart_puts(task_label());
        uart_puts(" fault: cause=");
        uart_put_u64(cause);
        uart_puts(" epc=");
        uart_put_hex(frame->epc);
        uart_puts(" tval=");
        uart_put_hex(value);
        uart_puts("\n");
        tasks[current].runnable = false;
        return schedule(nullptr, 0);
    }

    /* ECALL 是 32 位指令；保存的 EPC 指向 ECALL 本身。 */
    frame->epc += 4;
    switch (frame->x[REG_A7]) {
    case RS_SYS_WRITE:
        frame->x[REG_A0] = sys_write(frame->x[REG_A0], frame->x[REG_A1]);
        break;
    case RS_SYS_TICKS:
        frame->x[REG_A0] = timer_ticks();
        break;
    case RS_SYS_YIELD:
        frame->x[REG_A0] = 0;
        return schedule("yield", frame->epc - 4);
    case RS_SYS_EXIT:
        uart_puts(task_label());
        uart_puts(" exit: ");
        uart_put_u64(frame->x[REG_A0]);
        uart_puts("\n");
        tasks[current].runnable = false;
        return schedule(nullptr, 0);
    default:
        frame->x[REG_A0] = UINT64_MAX;
        break;
    }
    return frame;
}
