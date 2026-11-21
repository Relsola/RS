#include "user/abi.h"
#include "user/api.h"
#include "user/layout.h"

unsigned user_yield_reserved(volatile uint32_t *word);
uint32_t user_checksum(void);

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

static void write_hex(uintptr_t value, unsigned width)
{
    static const char digits[] = "0123456789abcdef";
    char hex[sizeof(value) * 2];
    for (unsigned i = width; i > 0; --i) {
        hex[i - 1] = digits[value & 15];
        value >>= 4;
    }
    write_bytes(hex, width);
}

static unsigned preempt_demo(void)
{
    uint32_t result = user_checksum();
    write_text("task A checksum: 0x");
    write_hex(result, 8);
    write_text("\n");
    return result == UINT32_C(0x0ed55926) ? 0 : 7;
}

static unsigned memory_demo(bool task_b)
{
    static const char path[] = "/proc/meminfo";
    volatile unsigned char *page = user_map(1);
    long fd = user_open(path, sizeof(path) - 1);
    if (!page || fd != 3 || *page != 0) {
        return 10;
    }
    *page = task_b ? 29 : 17;
    char prefix[4];
    size_t count = task_b ? 4 : 1;
    if (user_read(fd, prefix, count) != (long)count || prefix[0] != 'p') {
        return 11;
    }
    user_yield();
    char next = 0;
    unsigned observed = *page;
    if (user_read(fd, &next, 1) != 1 || next != (task_b ? '_' : 'o') ||
        observed != (task_b ? 29u : 17u)) {
        return 12;
    }
    write_text(task_b ? "task B va=0x" : "task A va=0x");
    write_hex((uintptr_t)page, sizeof(uintptr_t) * 2);
    write_text(" fd=");
    char descriptor = (char)('0' + fd);
    write_bytes(&descriptor, 1);
    write_text(" value=");
    char value[] = {(char)('0' + observed / 10), (char)('0' + observed % 10)};
    write_bytes(value, sizeof(value));
    write_text(" next=");
    write_bytes(&next, 1);
    write_text("\n");
    /* 让 exit 同时回收本任务的匿名页与描述符，另一任务将继续使用自己的。 */
    return 0;
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
    case RS_USER_MEMORY_A:
    case RS_USER_MEMORY_B: status = memory_demo(mode == RS_USER_MEMORY_B); break;
    default: status = 8; break;
    }
    user_exit(status);
}
