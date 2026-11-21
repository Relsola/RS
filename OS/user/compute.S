    .section .text.user_checksum, "ax"
    .globl user_checksum
    .type user_checksum, @function
user_checksum:
    li a0, 0x31415926
    li t0, 1
    li t1, 524289
    li t2, 1664525

    # state = (state * 1664525 + i) mod 2^32，i = 1..524288。
    # 这两个符号只供观察，调度器无需知道用户代码所在的位置。
    .globl rs_compute_begin, rs_compute_end
rs_compute_begin:
.Lcompute_loop:
    mulw a0, a0, t2
    addw a0, a0, t0
    addi t0, t0, 1
    bne t0, t1, .Lcompute_loop
rs_compute_end:
    ret
    .size user_checksum, . - user_checksum
