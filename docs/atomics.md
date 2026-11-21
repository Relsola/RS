# 原子指令与任务切换

假设一个用户任务读出计数器 10，准备写回 11，定时器却在两条指令之间切走了任务。另一任务把共享计数器加到 11 后，第一个任务的普通写回会覆盖这次更新。RV64A 给出两种写法。一步能表达的更新可用 AMO，例如 `amoadd.d`。需要先计算或判断时，可用 LR 读并建立保留，再由 SC 尝试提交。两组指令分别属于 Zaamo 和 Zalrsc。[RISC-V A 2.1](https://docs.riscv.org/reference/isa/v20260120/unpriv/a-st-ext.html)

## 跟一遍失败后重试的 LR/SC

```asm
# a0 指向自然对齐的 64 位共享计数器
.Lretry:
    lr.d.aq t0, (a0)
    addi    t0, t0, 1
    sc.d.rl t1, t0, (a0)
    bnez    t1, .Lretry
```

1. LR 读出 10，并记录这次读取覆盖的物理字节。加法在寄存器里完成，内存仍为 10。
2. 若有写操作把计数器改成 20，对应的保留随即失效。SC 对地址完成写权限检查，向 `t1` 返回 1，内存仍为 20。
3. 分支回到 LR，任务重新读出 20。若这次没有冲突，SC 把 21 写入内存，并向 `t1` 返回 0。可移植的程序以“非零”判断失败。这个四指令循环满足规范对受约束 LR/SC 循环的形式要求，解释器在没有干扰时允许它完成。[循环测试](../VM/tests/atomic_tests.cpp)

本板的 hart 是一个独立的指令执行单元，只保留最近一次 LR 读取的自然对齐 4 或 8 个物理字节。普通写或 DMA 写只要碰到其中一个字节就清除保留，即使写回的位模式相同。若 LR.W 读了地址 `p` 的 4 字节，写相邻字 `p+4` 不会影响它。SC 必须完全落在保留范围内，无论提交结果如何，本次 SC 都会消耗旧保留。[保留范围](../VM/reservation.h) · [总线写入口](../VM/bus.cpp)

## 从指令编码走到物理内存

`cpu_step` 首先取得源寄存器的值。原子执行器检查编码和对齐，随后走与普通访问共用的地址翻译及 PMP 检查，在确认目标位于 RAM 后才读写数据。如果两个 Sv39 虚拟页映射同一物理页，本机允许 LR 通过第一个映射建立保留，SC 通过第二个**可写**映射提交。规范允许实现对别名采取更保守的策略，可移植的重试循环应使用相同地址和宽度。[原子执行器](../VM/cpu_atomic.cpp) · [地址翻译](../VM/cpu_mmu.cpp)

| 情况 | 本机的处理 |
| --- | --- |
| 读取旧值 | LR.W 和所有 AMO.W 都把旧的 32 位值符号扩展到 64 位。例如旧值 `0x80000005` 返回 `0xffffffff80000005`。AMO.W 仅更新所选 4 字节。 |
| AMO 运算 | SWAP、ADD、XOR、AND、OR、MIN、MAX、MINU、MAXU 都支持 W/D。MIN/MAX 按对应位宽解释符号，MINU/MAXU 按无符号数比较。即使 `rd=x0`，内存更新仍需执行。 |
| 地址未对齐 | LR 报 load address misaligned（4），SC 与 AMO 报 store/AMO address misaligned（6）。 |
| 页权限不足、A/D 未就绪 | 本机采用 Svade，由软件处理缺失的 A/D 位。LR 报 load page fault（13），SC 与 AMO 报 store/AMO page fault（15）。没有有效保留的 SC 也要通过写权限检查才能正常返回失败码。 |
| PMP 拒绝、目标为 ROM 或 MMIO | LR 报 load access fault（5），SC 与 AMO 报 store/AMO access fault（7）。AMO 同时要求读和写权限，在读取可能有副作用的设备寄存器前就会陷入。 |

同一条 AMO 的读、计算和写在一个 `cpu_step` 中完成。定时器在指令边界交付，virtio 通知触发的 DMA 也在处理该通知时同步结束。这个顺序执行模型已满足本板 `aq` / `rl` 的要求。就内存域而言，`aq` 约束后续访问，`rl` 约束先前访问。跨内存域和 I/O 域的排序使用相应的 `fence`。以后若增加并行 hart 或异步 DMA，必须重新实现这些顺序和不可分割性保证。[CPU 步进](../VM/cpu.cpp) · [RISC-V 原子顺序规则](https://docs.riscv.org/reference/isa/v20260120/unpriv/a-st-ext.html)

## 为什么磁盘读也会打断保留

从磁盘读扇区会写来宾内存。virtio 块设备直接向描述符指定的数据缓冲区读入字节。宿主短读时，已经传输的前缀同样构成写操作，所以设备按实际传输长度更新保留。随后，设备还要写状态字节、used ring 的表项和索引，这些写都经过总线。只监视扇区缓冲区，会漏掉正在观察完成状态的 CPU。[块设备实现](../VM/virtio.cpp) · [DMA 重叠和短读测试](../VM/tests/virtio_tests.cpp)

## 为什么内核栈上有一条 SC

保留属于 hart。两个任务轮流使用同一个 hart 时，保存通用寄存器和切换 `satp` 都无法表达“前一个任务的 LR 到此失效”。内核的 `discard_lr_reservation` 在私有内核栈上临时分配 16 字节，对其中自然对齐且可写的 8 字节执行 `sc.d zero, zero, (sp)`，随后收回栈空间。若 SC 失败，旧保留已经丢弃。若 SC 恰好成功，零只会写入刚分配的临时槽。即使预期失败，该地址也必须允许写入。[陷阱汇编](../OS/kernel/trap.S) · [任务切换点](../OS/kernel/trap.c) · [双任务调度](tasks.md)

`tasks yield` 会在真实的任务切换前后检查这个约定。同一份用户程序在自研 VM 的正常内核上运行时，A、B 均以 0 退出。临时将内核的清理指令换成 `nop` 后，A 的 `sc.w` 意外成功，A 以 9 退出，B 仍以 0 退出。这里 A 尝试写回原值，因此程序必须检查 SC 的返回码，单看内存里的 11 发现不了差别。[用户侧检查](../OS/user/demo.c)

## 复现

在工程根目录构建 Debug 后执行以下命令。第一组用真实指令编码验证全部 W/D 运算、顺序位组合、物理别名和异常现场，还逐步执行上面的重试循环。第二组检查数据、状态和完成环写入是否按实际覆盖范围撤销保留。

```powershell
ctest --preset debug -R '^vm\.(atomics|virtio)$' --output-on-failure
```
