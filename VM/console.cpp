#include "console.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

static volatile std::sig_atomic_t interrupted;

static void on_signal(int)
{
    interrupted = 1;
}

bool console_interrupted()
{
    return interrupted != 0;
}

bool console_start(Console *console)
{
    console->pending = -1;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#if defined(_WIN32)
    DWORD mode = 0;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    console->tty = GetConsoleMode(input, &mode) != 0;
    console->pipe = GetFileType(input) == FILE_TYPE_PIPE;
    // 保留来宾写出的 CR/LF 字节，避免 Windows CRT 再补一个 CR。
    return _setmode(_fileno(stdout), _O_BINARY) != -1;
#else
    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &console->saved) == -1) {
            return false;
        }
        termios mode = console->saved;
        mode.c_lflag &= ~(ICANON | ECHO);
        mode.c_iflag &= ~(ICRNL | IXON);
        mode.c_cc[VMIN] = 1;
        mode.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &mode) == -1) {
            return false;
        }
        console->restore_terminal = true;
    }
    return true;
#endif
}

void console_stop(Console *console)
{
#if !defined(_WIN32)
    if (console->restore_terminal) {
        tcsetattr(STDIN_FILENO, TCSANOW, &console->saved);
    }
#else
    (void)console;
#endif
}

static void poll_byte(Console *console)
{
#if defined(_WIN32)
    if (console->tty) {
        if (_kbhit()) {
            int byte = _getch();
            if (byte == 0 || byte == 0xe0) {
                (void)_getch(); // 功能键由控制台编码为两个字节。
            } else {
                console->pending = byte;
            }
        }
        return;
    }
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (console->pipe) {
        DWORD count = 0;
        if (!PeekNamedPipe(input, nullptr, 0, nullptr, &count, nullptr)) {
            console->eof = true;
            return;
        }
        if (count == 0) {
            return;
        }
    }
    uint8_t byte;
    DWORD count = 0;
    if (ReadFile(input, &byte, 1, &count, nullptr) && count != 0) {
        console->pending = byte;
    } else {
        console->eof = true;
    }
#else
    pollfd fd{STDIN_FILENO, POLLIN, 0};
    if (poll(&fd, 1, 0) <= 0) {
        return;
    }
    uint8_t byte;
    ssize_t count = read(STDIN_FILENO, &byte, 1);
    if (count == 1) {
        console->pending = byte;
    } else if (count == 0 || (errno != EINTR && errno != EAGAIN)) {
        console->eof = true;
    }
#endif
}

bool console_ready(Console *console)
{
    if (console->pending < 0 && !console->eof) {
        poll_byte(console);
    }
    if (console->pending >= 0) {
        console->empty_polls = 0;
        return true;
    }
    ++console->empty_polls;
    if (!console->eof && console->empty_polls % 128 == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

uint8_t console_read(Console *console)
{
    if (!console_ready(console)) {
        return 0;
    }
    uint8_t byte = static_cast<uint8_t>(console->pending);
    console->pending = -1;
    return byte;
}

void console_write(Console *console, uint8_t value)
{
    std::putchar(value);
    std::fflush(stdout);
    console->empty_polls = 0;
}
