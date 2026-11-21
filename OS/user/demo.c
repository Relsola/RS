#include "user/abi.h"
#include "user/layout.h"

#include <stddef.h>
#include <stdint.h>

long user_write(const void *buffer, size_t length);
uint64_t user_ticks(void);
void user_yield(void);
unsigned user_yield_reserved(volatile uint32_t *word);
uint32_t user_checksum(void);
[[noreturn]] void user_exit(unsigned status);

/* 每个实例在同一虚拟地址读写这些 .bss 对象。volatile 强制切换后重新读取。 */
static volatile uint32_t private_value;
static char value_line[3];

static void write_bytes(const void *buffer, size_t length)
{
    if (user_write(buffer, length) != (long)length) {
        user_exit(1);
    }
}

static void write_text(const char *text)
{
    size_t length = 0;
    while (text[length]) {
        ++length;
    }
    write_bytes(text, length);
}

static unsigned fault_demo(void)
{
    uintptr_t last_byte = USER_ENTRY + USER_PAGE_SIZE - 1;
    if (user_write((const void *)last_byte, 2) != -1) {
        return 2;
    }
    if (user_write((const void *)UINTPTR_MAX, 2) != -1) {
        return 3;
    }
    /* UART 页只允许监管态访问，这次普通读取应触发 load page fault。 */
    (void)*(const volatile uint8_t *)(uintptr_t)0x10000000;
    return 4;
}

static void hello(void)
{
    uint64_t before = user_ticks();
    do {
        for (volatile unsigned i = 0; i < 128; ++i) {
        }
    } while (user_ticks() == before);

    static const char greeting[] = "hello from user mode";
    char newline = '\n';
    write_bytes(greeting, sizeof(greeting) - 1);
    write_bytes(&newline, 1);
}

static unsigned yield_demo(bool task_b)
{
    if (private_value != 0) {
        return 5;
    }
    uint32_t expected = task_b ? 22 : 11;
    private_value = expected;
    if (task_b) {
        user_yield();
    } else if (user_yield_reserved(&private_value) == 0) {
        return 9; /* 上一次运行 A 时建立的保留意外存活，SC 写回了原值。 */
    }
    uint32_t observed = private_value;
    if (observed != expected) {
        return 6;
    }
    value_line[0] = (char)('0' + observed / 10);
    value_line[1] = (char)('0' + observed % 10);
    value_line[2] = '\n';
    write_text(task_b ? "task B private value: " : "task A private value: ");
    write_bytes(value_line, sizeof(value_line));
    return 0;
}

static unsigned preempt_demo(void)
{
    uint32_t result = user_checksum();
    static const char digits[] = "0123456789abcdef";
    char hex[9];
    for (unsigned i = 0; i < 8; ++i) {
        hex[i] = digits[(result >> (28 - 4 * i)) & 15];
    }
    hex[8] = '\n';
    write_text("task A checksum: 0x");
    write_bytes(hex, sizeof(hex));
    return result == UINT32_C(0x0ed55926) ? 0 : 7;
}

/* 内核以 sret 进入这里。sp 指向本任务的私有栈，a0 选择同一份代码的执行路径。 */
[[noreturn]] void user_main(unsigned mode)
{
    unsigned status = 0;
    switch (mode) {
    case RS_USER_HELLO: hello(); break;
    case RS_USER_FAULT:
    case RS_USER_FAULT_A: status = fault_demo(); break;
    case RS_USER_YIELD_A:
    case RS_USER_YIELD_B: status = yield_demo(mode == RS_USER_YIELD_B); break;
    case RS_USER_PREEMPT_A: status = preempt_demo(); break;
    case RS_USER_PREEMPT_B: write_text("task B ran\n"); break;
    case RS_USER_SURVIVE_B: write_text("task B survived\n"); break;
    default: status = 8; break;
    }
    user_exit(status);
}
