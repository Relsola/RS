#ifndef RS_EXEC_ARGS_H
#define RS_EXEC_ARGS_H

#include "user/abi.h"
#include <stdint.h>

/* 暂存旧映像或 shell 的字符串。偏移量在新用户栈就绪后才转换成用户地址。 */
typedef struct {
    unsigned count;
    unsigned used;
    unsigned offsets[RS_ARG_MAX_COUNT];
    char text[RS_ARG_MAX_BYTES];
} ExecArgs;

const char *exec_args_parse(ExecArgs *args, const char *line);
bool exec_args_read(ExecArgs *args, unsigned slot, uintptr_t vector);
uintptr_t exec_args_install(const ExecArgs *args, uint8_t *stack_page);

#endif
