#include "user/layout.h"

    # 内核通过 RAM 中的物理别名安装、读取这一页。页尾补零，避免同页出现其他内核数据。
    .section .rodata.user_image, "a", @progbits
    .balign USER_PAGE_SIZE
    .globl user_image_page
    .type user_image_page, @object
user_image_page:
    .incbin "user.bin"
    .zero USER_PAGE_SIZE - (. - user_image_page)
    .size user_image_page, . - user_image_page
