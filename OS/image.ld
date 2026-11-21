OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS {
    . = IMAGE_BASE;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    .data : { *(.data .data.* .sdata .sdata.*) }
    .bss (NOLOAD) : {
        . = ALIGN(8);
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
    .stack (NOLOAD) : {
        . = ALIGN(16);
        . += 32K;
        __stack_top = .;
    }
    ASSERT(__stack_top <= IMAGE_LIMIT, "image exceeds its RAM region")
    /* 运行时按 vlenb 从栈顶上方的空闲内核 RAM 划出各任务的向量现场。 */
    PROVIDE(__context_begin = __stack_top);
    PROVIDE(__context_limit = IMAGE_LIMIT);
    /DISCARD/ : { *(.eh_frame*) *(.comment) }
}
