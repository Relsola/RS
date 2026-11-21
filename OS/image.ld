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
    /DISCARD/ : { *(.eh_frame*) *(.comment) }
}
