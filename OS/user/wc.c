#include "user/api.h"

/* 按 ASCII 空白分词。in_word 随文件游标保留，单词可以横跨两次 read。 */
static bool whitespace(unsigned char ch)
{
    return ch == ' ' || (ch >= '\t' && ch <= '\r');
}

static void write_count(size_t count)
{
    char digits[3 * sizeof(count) + 1];
    size_t at = sizeof(digits);
    digits[--at] = ' ';
    do {
        digits[--at] = (char)('0' + count % 10);
        count /= 10;
    } while (count);
    user_write(digits + at, sizeof(digits) - at);
}

[[noreturn]] void program_entry(int argc, char **argv)
{
    if (argc != 2) {
        static const char usage[] = "usage: wc.elf PATH\n";
        user_write(usage, sizeof(usage) - 1);
        user_exit(2);
    }
    const char *path = argv[1];
    size_t length = 0;
    while (path[length]) {
        ++length;
    }
    long fd = user_open(path, length);
    if (fd < 0) {
        static const char error[] = "wc: cannot open file\n";
        user_write(error, sizeof(error) - 1);
        user_exit(1);
    }
    size_t lines = 0, words = 0, bytes = 0;
    bool in_word = false;
    unsigned char buffer[64];
    long count;
    while ((count = user_read(fd, buffer, sizeof(buffer))) > 0) {
        bytes += (size_t)count;
        for (long i = 0; i < count; ++i) {
            bool word_byte = !whitespace(buffer[i]);
            lines += buffer[i] == '\n';
            words += word_byte && !in_word;
            in_word = word_byte;
        }
    }
    user_close(fd);
    if (count < 0) {
        static const char error[] = "wc: read error\n";
        user_write(error, sizeof(error) - 1);
        user_exit(1);
    }
    write_count(lines);
    write_count(words);
    write_count(bytes);
    user_write(path, length);
    user_write("\n", 1);
    user_exit(0);
}
