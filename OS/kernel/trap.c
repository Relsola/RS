#include "trap.h"
#include "context.h"
#include "paging.h"
#include "platform.h"
#include "user/abi.h"
#include "user_files.h"
#include "vfs.h"

enum {
    TIMER_PERIOD = 32768,
    STATUS_SPIE = 1 << 5,
    STATUS_SPP = 1 << 8,
    U_ECALL = 8,
    REG_A0 = 10, REG_A1 = 11, REG_A2 = 12, REG_A7 = 17,
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

typedef enum { SLOT_FREE, RUNNABLE, WAITING_FOR_CHILD, ZOMBIE } TaskState;
typedef struct {
    TaskState state;
    /* U 陷阱从 frame 顶端向下增长，C 分派函数使用其下方私有的内核栈。 */
    alignas(16) uint8_t kernel_stack[8192];
    TrapFrame frame;
} Task;

static_assert(USER_TASK_COUNT == 2);
static_assert(offsetof(Task, frame) % 16 == 0);
static Task tasks[USER_TASK_COUNT];
static unsigned current;
static bool fixed_pair;
static bool time_sliced;
/* 前台程序使用槽 0，子进程使用槽 1。槽可复用，用户所见的 ID 单调递增。 */
static uint64_t last_child_id;
static uint64_t child_result;

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

static void prepare_frame(unsigned slot, uintptr_t entry, unsigned a0, uintptr_t argv)
{
    Task *task = &tasks[slot];
    for (unsigned i = 0; i < 32; ++i) {
        task->frame.x[i] = 0;
    }
    task->frame.x[2] = argv ? argv : USER_STACK_TOP;
    task->frame.x[REG_A0] = a0;
    task->frame.x[REG_A1] = argv;
    task->frame.epc = entry;
    task->frame.status = STATUS_SPIE | context_reset(slot);
    task->state = RUNNABLE;
}

static void enter_single_task(void)
{
    fixed_pair = false;
    time_sliced = false;
    tasks[1].state = SLOT_FREE;
    enter_tasks();
}

void run_user(bool fault)
{
    user_prepare(0);
    prepare_frame(0, USER_ENTRY, fault ? RS_USER_FAULT : RS_USER_HELLO, 0);
    enter_single_task();
}

static const char *load_program(unsigned slot, const char *path, const ExecArgs *args)
{
    VfsFile file;
    const char *error = vfs_open(path, &file);
    if (error) {
        return error;
    }
    const uint8_t *bytes;
    error = vfs_contents(&file, &bytes);
    if (error) {
        return error;
    }
    ElfImage image;
    error = elf_parse(bytes, file.size, &image);
    if (error) {
        return error;
    }
    /* 固定的 ELF 页缓冲已经预留。通过完整验证后，下面的替换无需再次分配资源。 */
    uintptr_t argv = user_prepare_elf(slot, &image, args);
    sync_user_instructions();
    prepare_frame(slot, image.entry, args ? args->count : 0, argv);
    return nullptr;
}

const char *run_elf(const char *line)
{
    ExecArgs args;
    const char *error = exec_args_parse(&args, line);
    if (error) {
        return error;
    }
    /* 单独运行文件保留原来的空栈入口。带参数时 argv[0] 是 shell 中的文件名。 */
    error = load_program(0, args.text, args.count > 1 ? &args : nullptr);
    if (error) {
        return error;
    }
    enter_single_task();
    return nullptr;
}

static void exec_current(TrapFrame *frame, bool with_args)
{
    char path[VFS_PATH_BYTES];
    ExecArgs args;
    if (!user_copy_path(current, path, sizeof(path), frame->x[REG_A0], frame->x[REG_A1]) ||
        (with_args && !exec_args_read(&args, current, frame->x[REG_A2])) ||
        load_program(current, path, with_args ? &args : nullptr)) {
        frame->x[REG_A0] = UINT64_MAX;
        return;
    }
    /* 下一次 sret 使用刚准备的新现场。ASID 和根页表地址都没变，也要同步旧翻译。 */
    discard_lr_reservation();
    user_activate(current);
    context_select(current);
}

void run_tasks(TaskDemo demo)
{
    static const unsigned modes[][2] = {
        [TASKS_YIELD]   = {RS_USER_YIELD_A, RS_USER_YIELD_B},
        [TASKS_PREEMPT] = {RS_USER_PREEMPT_A, RS_USER_PREEMPT_B},
        [TASKS_FAULT]   = {RS_USER_FAULT_A, RS_USER_SURVIVE_B},
        [TASKS_MEMORY]  = {RS_USER_MEMORY_A, RS_USER_MEMORY_B},
    };
    fixed_pair = true;
    time_sliced = demo == TASKS_PREEMPT;
    for (unsigned slot = 0; slot < USER_TASK_COUNT; ++slot) {
        user_prepare(slot);
        prepare_frame(slot, USER_ENTRY, modes[demo][slot], 0);
    }
    enter_tasks();
}

static TrapFrame *activate_task(unsigned slot)
{
    discard_lr_reservation();
    user_activate(slot);
    context_select(slot);
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
    if (tasks[next].state == RUNNABLE) {
        if (fixed_pair && reason) {
            uart_puts(current == 0 ? "switch A -> B: " : "switch B -> A: ");
            uart_puts(reason);
            uart_puts(" epc=");
            uart_put_hex(epc);
            uart_puts("\n");
        }
        return activate_task(next);
    }
    if (tasks[current].state == RUNNABLE) {
        return &tasks[current].frame;
    }
    discard_lr_reservation();
    kernel_activate();
    return nullptr; /* 本组结束，让汇编恢复 shell。 */
}

static const char *task_label(void)
{
    if (fixed_pair) {
        return current == 0 ? "task A" : "task B";
    }
    return current == 0 ? "user" : "child";
}

static uint64_t fork_child(const TrapFrame *parent_frame)
{
    if (fixed_pair || current != 0 || tasks[1].state != SLOT_FREE || last_child_id == INT64_MAX) {
        return UINT64_MAX;
    }
    /* 克隆的 EPC 已跨过 ecall。复制用户现场，子进程下次陷入时使用自己的内核栈。 */
    discard_lr_reservation();
    user_clone(0, 1);
    user_files_clone(0, 1);
    context_fork(0, 1);
    tasks[1].frame = *parent_frame;
    tasks[1].frame.x[REG_A0] = 0;
    tasks[1].state = RUNNABLE;
    time_sliced = true;
    return ++last_child_id;
}

static TrapFrame *wait_child(TrapFrame *frame)
{
    uint64_t id = frame->x[REG_A0];
    if (fixed_pair || current != 0 || id != last_child_id || tasks[1].state == SLOT_FREE) {
        frame->x[REG_A0] = UINT64_MAX;
        return frame;
    }
    if (tasks[1].state == ZOMBIE) {
        frame->x[REG_A0] = child_result;
        tasks[1].state = SLOT_FREE;
        return frame;
    }
    tasks[0].state = WAITING_FOR_CHILD;
    return schedule(nullptr, 0);
}

static TrapFrame *finish_task(uint64_t result)
{
    context_drop(current);
    user_files_release(current);
    user_release(current);
    tasks[current].state = SLOT_FREE;
    if (!fixed_pair && current == 1) {
        if (tasks[0].state == WAITING_FOR_CHILD) {
            /* wait 的返回值写进父进程保存的 a0，恢复时从 ecall 后继续。 */
            tasks[0].frame.x[REG_A0] = result;
            tasks[0].state = RUNNABLE;
        } else if (tasks[0].state == RUNNABLE) {
            child_result = result;
            tasks[1].state = ZOMBIE;
        }
    } else if (tasks[1].state == ZOMBIE) {
        tasks[1].state = SLOT_FREE; /* 父进程已退出，无人再领取这个结果。 */
    }
    return schedule(nullptr, 0);
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
        if (user_page_fault(current, value, cause)) {
            return frame; /* 补页成功，从原 sepc 重试产生缺页的指令。 */
        }
        uart_puts(task_label());
        uart_puts(" fault: cause=");
        uart_put_u64(cause);
        uart_puts(" epc=");
        uart_put_hex(frame->epc);
        uart_puts(" tval=");
        uart_put_hex(value);
        uart_puts("\n");
        return finish_task(UINT64_C(0) - (256 + cause));
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
    case RS_SYS_OPEN:
        frame->x[REG_A0] = (uint64_t)user_file_open(current, frame->x[REG_A0], frame->x[REG_A1]);
        break;
    case RS_SYS_READ:
        frame->x[REG_A0] = (uint64_t)user_file_read(current, frame->x[REG_A0],
                                                   frame->x[REG_A1], frame->x[REG_A2]);
        break;
    case RS_SYS_CLOSE:
        frame->x[REG_A0] = (uint64_t)user_file_close(current, frame->x[REG_A0]);
        break;
    case RS_SYS_MAP:
        frame->x[REG_A0] = user_map(current, frame->x[REG_A0]);
        break;
    case RS_SYS_UNMAP:
        frame->x[REG_A0] = user_unmap(current, frame->x[REG_A0]) ? 0 : UINT64_MAX;
        break;
    case RS_SYS_FORK:
        frame->x[REG_A0] = fork_child(frame);
        break;
    case RS_SYS_WAIT:
        return wait_child(frame);
    case RS_SYS_EXEC:
    case RS_SYS_EXECV:
        exec_current(frame, frame->x[REG_A7] == RS_SYS_EXECV);
        break;
    case RS_SYS_YIELD:
        frame->x[REG_A0] = 0;
        return schedule("yield", frame->epc - 4);
    case RS_SYS_EXIT:
        uart_puts(task_label());
        uart_puts(" exit: ");
        uart_put_u64((uint32_t)frame->x[REG_A0]);
        uart_puts("\n");
        return finish_task((uint32_t)frame->x[REG_A0]);
    default:
        frame->x[REG_A0] = UINT64_MAX;
        break;
    }
    return frame;
}
