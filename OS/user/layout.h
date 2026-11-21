#ifndef RS_USER_LAYOUT_H
#define RS_USER_LAYOUT_H

/* RS_USER_BASE 由来宾构建传入，同时设为 user.elf 的链接基址。 */
#define USER_PAGE_SIZE 4096
#define USER_ENTRY     RS_USER_BASE
#define USER_STACK_TOP (USER_ENTRY + 4 * USER_PAGE_SIZE)

#endif
