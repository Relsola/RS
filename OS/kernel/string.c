#include <stddef.h>

/* 即使是 freestanding，Clang 也可把结构体赋值和清零生成为这两个 C 调用。 */
void *memcpy(void *restrict destination, const void *restrict source, size_t count)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < count; ++i) {
        out[i] = in[i];
    }
    return destination;
}

void *memset(void *destination, int byte, size_t count)
{
    unsigned char *out = destination;
    for (size_t i = 0; i < count; ++i) {
        out[i] = (unsigned char)byte;
    }
    return destination;
}
