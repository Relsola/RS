OUTPUT_ARCH(riscv)
ENTRY(user_main)

SECTIONS {
    . = USER_BASE;
    .text : { KEEP(*(.text.user_main)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    ASSERT(user_main == USER_BASE, "user entry must start the image")
    ASSERT(. <= USER_BASE + 4096, "user program exceeds its RX page")

    /* 每个任务独有一张零初始化的用户数据页；磁盘镜像仅携带共享只读页。 */
    .data : { *(.data .data.* .sdata .sdata.*) }
    ASSERT(SIZEOF(.data) == 0, "user image does not load initialized writable data")
    .bss USER_DATA_BASE (NOLOAD) : { *(.bss .bss.* .sbss .sbss.* COMMON) }
    ASSERT(SIZEOF(.bss) <= 4096, "user BSS exceeds its private page")
    /DISCARD/ : { *(.eh_frame*) *(.comment) }
}
