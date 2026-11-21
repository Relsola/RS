#pragma once

#include <cstdint>

#if !defined(_WIN32)
#include <termios.h>
#endif

struct Console
{
    int pending;
    unsigned empty_polls;
    bool eof;
#if defined(_WIN32)
    bool tty, pipe;
#else
    bool restore_terminal;
    termios saved;
#endif
};

bool console_start(Console *console);
void console_stop(Console *console);
bool console_ready(Console *console);
uint8_t console_read(Console *console);
void console_write(Console *console, uint8_t value);
bool console_interrupted();
