#include <stddef.h>
#include <stdint.h>

long user_write(const void *buffer, size_t length);
unsigned selected_byte(const char *address, unsigned include);
[[noreturn]] void user_exit(unsigned status);

static char message[] = "Hello from disk ELF\n";
static volatile uint32_t starts;

[[noreturn]] void program_entry(void)
{
    /* 每次 run 都从 ELF 重建 .data，p_memsz 超出 p_filesz 的部分由内核清零。 */
    if (starts++ != 0 || selected_byte(message, 1) != 'H') {
        user_exit(1);
    }
    unsigned checksum = 0;
    for (size_t i = 0; i < sizeof(message) - 1; ++i) {
        checksum += selected_byte(message + i, (unsigned)(i & 1));
    }
    if (checksum != 797) {
        user_exit(2);
    }
    message[0] = 'h';
    const size_t length = sizeof(message) - 1;
    user_exit(user_write(message, length) == (long)length ? 0 : 3);
}
