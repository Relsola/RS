#include "exec_args.h"
#include "paging.h"
#include "string.h"

static bool begin_arg(ExecArgs *args)
{
    if (args->count == RS_ARG_MAX_COUNT) {
        return false;
    }
    args->offsets[args->count++] = args->used;
    return true;
}

static bool append_byte(ExecArgs *args, char byte)
{
    if (args->used == sizeof(args->text)) {
        return false;
    }
    args->text[args->used++] = byte;
    return true;
}

static bool separator(char ch)
{
    return ch == ' ' || ch == '\t';
}

const char *exec_args_parse(ExecArgs *args, const char *line)
{
    args->count = args->used = 0;
    for (;;) {
        while (separator(*line)) {
            ++line;
        }
        if (!*line) {
            return args->count && args->text[0] ? nullptr : "usage: run PATH [ARG ...]";
        }
        if (!begin_arg(args)) {
            return "too many arguments";
        }
        char quote = 0;
        while (*line && (quote || !separator(*line))) {
            char ch = *line++;
            if (quote && ch == quote) {
                quote = 0;
            } else if (!quote && (ch == '\'' || ch == '"')) {
                quote = ch;
            } else if (!append_byte(args, ch)) {
                return "arguments too long";
            }
        }
        if (quote) {
            return "unclosed quote";
        }
        if (!append_byte(args, '\0')) {
            return "arguments too long";
        }
    }
}

bool exec_args_read(ExecArgs *args, unsigned slot, uintptr_t vector)
{
    args->count = args->used = 0;
    for (;;) {
        uintptr_t address;
        if (!user_peek(slot, &address, vector, sizeof(address))) {
            return false;
        }
        if (!address) {
            return true;
        }
        if (!begin_arg(args)) {
            return false;
        }
        char ch;
        do {
            if (!user_peek(slot, &ch, address, 1) || !append_byte(args, ch)) {
                return false;
            }
            ++address;
        } while (ch);
        vector += sizeof(address);
    }
}

uintptr_t exec_args_install(const ExecArgs *args, uint8_t *stack_page)
{
    uintptr_t argv[RS_ARG_MAX_COUNT + 1];
    static_assert(RS_ARG_MAX_BYTES + sizeof(argv) + 15 < USER_PAGE_SIZE);
    const uintptr_t stack_base = USER_STACK_TOP - USER_PAGE_SIZE;
    size_t strings = USER_PAGE_SIZE - args->used;
    size_t vector_bytes = (args->count + 1) * sizeof(uintptr_t);
    size_t vector = (strings - vector_bytes) & ~(size_t)15;
    for (unsigned i = 0; i < args->count; ++i) {
        argv[i] = stack_base + strings + args->offsets[i];
    }
    argv[args->count] = 0;
    memcpy(stack_page + strings, args->text, args->used);
    memcpy(stack_page + vector, argv, vector_bytes);
    return stack_base + vector;
}
