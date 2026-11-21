#include "user/api.h"
#include "user/layout.h"

double vector_dot(const float *left, const float *right, size_t count);
int numeric_context(long fd, uint32_t *edge);
int numeric_pages(uint32_t *edge, uint32_t *scratch, uint32_t *last);
int numeric_initial(void);

void program_entry(int argc, char **argv)
{
    (void)argv;
    if (numeric_initial() != 0) {
        user_exit(1);
    }
    if (argc == 2) {
        static const char fresh[] = "numeric: exec cleared registers\n";
        user_write(fresh, sizeof(fresh) - 1);
        user_exit(0);
    }
    static const float a[] = {1, 2, 3, 4, 5, 6, 7, 8, 9}, b[] = {9, 8, 7, 6, 5, 4, 3, 2, 1};
    if (vector_dot(a, b, 9) != 165.0) {
        user_exit(2);
    }
    static const char result[] = "numeric: dot = 165\n";
    user_write(result, sizeof(result) - 1);
    uint32_t *pages = user_map(2 * USER_PAGE_SIZE);
    if (!pages) {
        user_exit(6);
    }
    uint32_t *edge = pages + USER_PAGE_SIZE / sizeof(*pages) - 2;
    if (numeric_pages(edge, pages, pages + 2 * USER_PAGE_SIZE / sizeof(*pages) - 1)) {
        user_exit(7);
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (edge[i] != 10 + i || pages[i] != 10 + i || pages[4 + i] != 13 - i) {
            user_exit(8);
        }
    }
    long fd = user_open("/readme.txt", 11);
    if (fd < 0) {
        user_exit(3);
    }
    int error = numeric_context(fd, edge);
    user_close(fd);
    if (error) {
        user_exit((unsigned)error);
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (edge[i] != 10 + i) {
            user_exit(9);
        }
    }
    if (user_unmap(pages)) {
        user_exit(10);
    }
    static const char done[] = "numeric: fork, timer and wait preserved registers\n";
    user_write(done, sizeof(done) - 1);
    user_exit(0);
}
