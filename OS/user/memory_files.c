#include "user/api.h"
#include "user/layout.h"

#include <stddef.h>
#include <stdint.h>

/* 两次 open 有各自的偏移。一次失败的 read 之后，仍应能从头读到同一批字节。 */
static void copy_readme(uint8_t *buffer)
{
    static const char path[] = "/readme.txt";
    long file = user_open(path, sizeof(path) - 1);
    long probe = user_open(path, sizeof(path) - 1);
    if (file < 0 || probe < 0 || file == probe) {
        user_exit(1);
    }
    uint8_t first[8];
    long prefix = user_read(probe, first, sizeof(first));
    if (prefix < 0 || user_close(probe) != 0 || user_close(probe) != -1) {
        user_exit(2);
    }
    if (prefix > 0 && user_read(file, nullptr, 1) != -1) {
        user_exit(3);
    }

    bool first_read = true;
    uint8_t last = '\n';
    for (;;) {
        long count = user_read(file, buffer, 64);
        if (count < 0 || (first_read && count < prefix)) {
            user_exit(4);
        }
        if (first_read) {
            for (long i = 0; i < prefix; ++i) {
                if (buffer[i] != first[i]) {
                    user_exit(5);
                }
            }
            first_read = false;
        }
        if (count == 0) {
            break;
        }
        last = buffer[count - 1];
        if (user_write(buffer, (size_t)count) != count) {
            user_exit(6);
        }
    }
    if (user_read(file, buffer, 1) != 0 || user_close(file) != 0) {
        user_exit(7);
    }
    if (last != '\n' && user_write("\n", 1) != 1) {
        user_exit(6);
    }
}

[[noreturn]] void program_entry(void)
{
    uint8_t *region = user_map(2 * USER_PAGE_SIZE);
    if (!region) {
        user_exit(8);
    }
    /* 这次用户态 load 触发缺页。read 写到页尾时还可以由内核兑现相邻的页。 */
    volatile uint8_t *first = region;
    if (*first != 0) {
        user_exit(9);
    }
    *first = 0x5a;
    copy_readme(region + USER_PAGE_SIZE - 8);
    if (*first != 0x5a || user_unmap(region) != 0) {
        user_exit(10);
    }
    uint8_t *fresh = user_map(USER_PAGE_SIZE);
    if (!fresh || *(volatile uint8_t *)fresh != 0 || user_unmap(fresh) != 0) {
        user_exit(11);
    }
    static const char done[] = "memory.elf: file copied, pages released\n";
    user_exit(user_write(done, sizeof(done) - 1) == (long)(sizeof(done) - 1) ? 0 : 12);
}
