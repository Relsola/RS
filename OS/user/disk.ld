OUTPUT_ARCH(riscv)
ENTRY(program_entry)

PHDRS {
    text PT_LOAD FLAGS(5); /* R X */
    data PT_LOAD FLAGS(6); /* R W */
}

SECTIONS {
    . = APP_BASE;
    .text : { *(.text .text.*) *(.rodata .rodata.* .srodata .srodata.*) } :text

    /* 首段结束后另起权限页。让消息横跨两个可写页，实际检验 write 的逐页复制。 */
    . = ALIGN(4096) + 4096 - 8;
    .data : { *(.data .data.* .sdata .sdata.*) } :data
    .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.* COMMON) } :data
    ASSERT(. <= APP_BASE + 16 * 4096, "disk program exceeds its user address window")
    /DISCARD/ : { *(.eh_frame*) *(.comment) }
}
