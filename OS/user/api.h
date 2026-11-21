#ifndef RS_USER_API_H
#define RS_USER_API_H

#include <stddef.h>
#include <stdint.h>

long user_write(const void *buffer, size_t length);
uint64_t user_ticks(void);
void user_yield(void);
[[noreturn]] void user_exit(unsigned status);
long user_open(const char *path, size_t length);
long user_read(long fd, void *buffer, size_t count);
long user_close(long fd);
void *user_map(size_t bytes);
long user_unmap(void *base);
long user_fork(void);
long user_wait(long child);
long user_exec(const char *path, size_t length);

#endif
