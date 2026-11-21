# 从磁盘程序读两条整数指令

查看 `hello.elf` 的反汇编时，可以留意两条指令。`c.lbu` 以两个字节编码一次内存读取。`czero.eqz` 根据条件决定保留哪个寄存器值。它们各执行一次，PC 分别前进 2 和 4。下面的命令让反汇编器显示压缩助记符。

```powershell
llvm-objdump -d -M no-aliases build/debug/images/hello.elf | Select-String 'c\.lbu|czero\.eqz'
```

## 一次压缩访存

例如半字 `0x81c8`（内存中的字节为 `c8 81`）表示 `c.lbu a0, 1(a1)`。源和目标寄存器各用三位编码，依次映射到 x8 至 x15。因此解释器给三位号加 8，得到基址寄存器 x11 和目标寄存器 x10。偏移量的低位次序容易读反。原始指令的 bit 6 是偏移 bit 0，bit 5 是偏移 bit 1。把 `0x81c8` 的两位交换位置，得到 `0x81a8`，读取地址就从 `a1+1` 变成 `a1+2`。

译码器把第一条指令展开为 `lbu a0, 1(a1)`，接下来复用普通的地址翻译、权限检查和零扩展。PC 的推进量仍取自原始半字。在 `c.lh` 和 `c.lhu` 的编码中，bit 6 改为选择有符号或无符号读取。这些半字指令可编码的偏移是 0 或 2。[压缩展开](../VM/cpu_compressed.cpp) · [取指和访存分派](../VM/cpu.cpp)

| RV64 Zcb 指令 | 展开后使用的操作 |
| --- | --- |
| `c.lbu`、`c.lh`、`c.lhu`、`c.sb`、`c.sh` | RV64I 的字节或半字访存 |
| `c.zext.b`、`c.not` | `andi rd,rd,255`、`xori rd,rd,-1` |
| `c.sext.b`、`c.sext.h`、`c.zext.h` | 本板已支持的 Zbb 扩展操作 |
| `c.zext.w` | Zba 的 `add.uw rd,rd,x0`，保留低 32 位并零扩展 |
| `c.mul` | M 扩展的整数乘法，结果取低 64 位 |

汇编器还认识 `c.sext.w rd`，会生成已有的 `c.addiw rd,0`。例如输入为 `0x0000000080000000` 时，`c.sext.w` 的结果是 `0xffffffff80000000`，`c.zext.w` 的结果是 `0x0000000080000000`。[Zc 规范](https://docs.riscv.org/reference/isa/v20260120/unpriv/zc.html)

## 条件成立时清零

设 `a0` 是待累加的字节，`a1` 表示是否选中该字节。执行 `czero.eqz t0,a0,a1` 后，若 `a1` 为零，`t0` 就是零。若 `a1` 非零，`t0` 就是 `a0`。下一条加法总能执行。条件检查使用完整的 64 位，即使仅 bit 63 为 1 也算非零。`czero.nez` 将上述清零条件反过来。[Zicond 规范](https://docs.riscv.org/reference/isa/v20260120/unpriv/zicond.html)

目的寄存器也可以和条件寄存器相同。执行 `czero.eqz a1,a0,a1` 时，CPU 先取出原来的 `a0` 和 `a1`，再把选好的结果写回 `a1`。指令测试覆盖了这个重叠关系，以及压缩存储的实际字节和访问故障时的 PC。展开测试的两列机器码分别来自压缩和普通形式的 Clang 汇编结果。[整数执行](../VM/cpu_integer.cpp) · [CPU 语义测试](../VM/tests/cpu_tests.cpp) · [机器码对照](../VM/tests/compressed_tests.cpp)
