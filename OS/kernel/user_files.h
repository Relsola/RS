#ifndef RS_USER_FILES_H
#define RS_USER_FILES_H

#include <stddef.h>
#include <stdint.h>

void user_files_release(unsigned slot);
void user_files_clone(unsigned parent, unsigned child);
long user_file_open(unsigned slot, uintptr_t path, size_t length);
long user_file_read(unsigned slot, uint64_t fd, uintptr_t destination, size_t count);
long user_file_close(unsigned slot, uint64_t fd);

#endif
