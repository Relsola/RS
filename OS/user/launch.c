#include "user/api.h"

[[noreturn]] void program_entry(void)
{
    static const char path[] = "/hello.elf";
    long child = user_fork();
    if (child < 0) {
        user_exit(1);
    }
    if (child == 0) {
        user_exec(path, sizeof(path) - 1);
        /* 成功时，CPU 从 hello.elf 的入口继续；只有加载失败才会走到这里。 */
        static const char error[] = "launch: could not load /hello.elf\n";
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
