#include "user/api.h"

[[noreturn]] void program_entry(int argc, char **argv)
{
    long child = user_fork();
    if (child < 0) {
        user_exit(1);
    }
    if (child == 0) {
        if (argc > 1) {
            size_t length = 0;
            while (argv[1][length]) {
                ++length;
            }
            user_execv(argv[1], length, argv + 1);
        } else {
            static const char path[] = "/hello.elf";
            user_exec(path, sizeof(path) - 1);
        }
        /* 替换成功时，执行从目标 ELF 的入口继续。 */
        static const char error[] = "launch: could not execute program\n";
        user_write(error, sizeof(error) - 1);
        user_exit(127);
    }
    long status = user_wait(child);
    if (status == 0) {
        static const char done[] = "launch: child completed\n";
        user_write(done, sizeof(done) - 1);
    }
    user_exit(status < 0 ? 1 : (unsigned)status);
}
