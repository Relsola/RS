    .section .text.selected_byte, "ax"
    .globl selected_byte
    .type selected_byte, @function
selected_byte:
    # Zcb 读取一个无符号字节，Zicond 按 include 标志决定是否把它计入校验和。
    c.lbu a0, 0(a0)
    czero.eqz a0, a0, a1
    ret
    .size selected_byte, . - selected_byte
