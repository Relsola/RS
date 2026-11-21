#include "user/api.h"

static volatile unsigned data_mark = 11;

static void print(const char *text)
{
    size_t length = 0;
    while (text[length]) {
        ++length;
    }
    if (user_write(text, length) != (long)length) {
        user_exit(1);
    }
}

static void print_values(char page, char next)
{
    char byte[] = {page, '\0'};
    print(byte);
    print(" file=");
    byte[0] = next;
    print(byte);
}

[[noreturn]] void program_entry(void)
{
    static const char path[] = "/proc/meminfo";
    volatile char *page = user_map(1);
    volatile unsigned stack_mark = 22;
    long fd = user_open(path, sizeof(path) - 1);
    char prefix[4];
    if (!page || fd < 0 || user_read(fd, prefix, sizeof(prefix)) != 4 ||
        prefix[0] != 'p' || prefix[1] != 'o' || prefix[2] != 'o' || prefix[3] != 'l') {
        user_exit(2);
    }
    *page = 'P';
    long child = user_fork();
    if (child < 0) {
        user_exit(3);
    }

    char next;
    if (child == 0) {
        if (*page != 'P' || data_mark != 11 || stack_mark != 22) {
            user_exit(4);
        }
        *page = 'C'; /* 这条用户 store 将共享的匿名页拆开。 */
        data_mark = 33;
        stack_mark = 44;
        if (user_read(fd, &next, 1) != 1 || next != '_' || user_close(fd) != 0) {
            user_exit(5);
        }
        print("child: page=");
        print_values(*page, next);
        print("\n");
        user_exit(37);
    }

    long status = user_wait(child);
    if (status != 37 || *page != 'P' || data_mark != 11 || stack_mark != 22 ||
        user_read(fd, &next, 1) != 1 || next != 'p' || user_close(fd) != 0) {
        user_exit(6);
    }
    print("parent: page=");
    print_values(*page, next);
    char digits[] = {(char)('0' + status / 10), (char)('0' + status % 10), '\n', '\0'};
    print(" wait=");
    print(digits);
    user_exit(0);
}
