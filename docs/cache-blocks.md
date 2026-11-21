# 一条 cbo.zero 怎样清零用户页

内核每次把旧页框交给新的匿名映射，都要先把 4096 字节清零。RVA23 的 `Zic64b` 约定缓存块按 64 字节自然对齐，`Zicboz` 提供 `cbo.zero (rs1)`，一次清零操作数所在的整块。`zero_page` 的输入已经按页对齐，循环从页首走到页尾，每次前进 64 字节，共执行 64 条清零指令。编译目标没有声明 `__riscv_zicboz` 时，同一个函数以 8 字节为步长执行 `sd`。[内核页分配](../OS/kernel/paging.c) · [两种清零循环](../OS/entry.S) · [RVA23 Profile](https://docs.riscv.org/reference/rva23/v1.0/rva23-profiles.html)

## 从操作数到总线上的写入

假设用户传入 `0x4004005f`。它落在 `0x40040040` 到 `0x4004007f` 这 64 个字节里，左右相邻的两个字节应保留原值。解释器分三步执行。

1. **检查能否执行指令。** M 模式可以直接执行。S 模式需要 `menvcfg.CBZE`，U 模式还需要 `senvcfg.CBZE`。许可关闭时触发非法指令，原因号为 2。
2. **翻译并检查权限。** 按写访问翻译操作数，Sv39 叶项需要 W、A、D，并通过常规特权检查。访问只读用户页时产生写页故障，`stval` 记录 `0x4004005f`。保留操作数的原始地址，内核才能按实际访存位置处理缺页。
3. **写入物理块。** 翻译成功后再对物理地址向下对齐，检查整块的 PMP 和 RAM 范围。RS 的设备寄存器和 ROM 不支持块清零。通过检查后，以八次 8 字节总线写完成清零，同时撤销重叠的 LR 保留。遇到 PMP 权限在块内变化的情形，规范留给实现决定，RS 选择整块拒绝。[内存访问实现](../VM/cpu_mmu.cpp) · [缓存块操作规范](https://docs.riscv.org/reference/isa/v20240411/unpriv/cmo.html)

`Zicbop` 的 `prefetch.i`、`prefetch.r` 和 `prefetch.w` 使用以 `x0` 为目的寄存器的 `ori` 编码。RS 按序解释指令，允许这些提示直接完成。即使提示地址未映射，CPU 也会继续执行下一条指令。[指令级测试](../VM/tests/cache_block_tests.cpp) · [规范覆盖](rva23s64.md)

## 固件与内核各打开哪一层权限

M 固件先置 `menvcfg.CBZE`，并置 `mstateen0.ENVCFG`（位 62），授权 S 访问 `senvcfg`。S 内核启动时再置 `senvcfg.CBZE`（位 7），用户程序由此获得块清零指令。这里有两个独立的问题，一是 S 能否执行指令，二是 S 能否访问用来给 U 授权的 CSR。只设前一个位，内核写 `senvcfg` 时仍会陷入非法指令。与此同时，各级页表的写权限继续有效。[固件与内核入口](../OS/entry.S) · [VM 的 CSR 读写](../VM/cpu_privilege.cpp) · [Smstateen / Ssstateen](https://docs.riscv.org/reference/isa/v20240411/priv/smstateen.html)

## 为什么只读代码页可以执行 cbo.clean

`Zicbom` 定义了三种管理操作。`cbo.clean` 把脏数据写回，`cbo.flush` 写回再使缓存副本失效，`cbo.inval` 根据环境设置使副本失效或执行 flush。在 RS 上，CPU 与设备共享 RAM，通过全部权限检查后即可完成这些操作。对整块可读的 virtio 寄存器窗口执行管理操作时，解释器只检查地址范围，设备不会收到寄存器读写。[VM 的物理访问检查](../VM/bus.cpp) · [缓存块操作规范](https://docs.riscv.org/reference/isa/v20240411/unpriv/cmo.html)

与 `cbo.zero` 一样，管理操作的内存异常使用写页故障或写访问错误，`tval` 保留原始操作数。权限判定却不同。管理操作允许作用于可读或可写的页，检查 A 位，忽略 D 位。因此一张 U/R/X、A=1、D=0 的代码页可以执行 `cbo.clean`，对它执行 `cbo.zero` 则触发写页故障。RS 对仅 X 的叶项按 MXR 决定能否读取，同时仍检查 SUM 和整块 PMP 权限。[Sv39 遍历与检查](../VM/cpu_mmu.cpp) · [指令边界测试](../VM/tests/cache_block_tests.cpp)

`CBCFE` 单独控制 clean 和 flush。`CBIE` 的 `00` 禁止 inval，`01` 将它转换为 flush，`11` 允许真正的失效。RS 将保留值 `10` 写成 `00`。固件允许 S 驱动执行三种操作，内核给 U 设置 `CBIE=01`。用户程序请求失效时，可能存在的脏数据会先写回。按此编译的固件和内核都在 virtio 磁盘请求中执行缓存维护；未启用 `__riscv_zicbom` 的构建保留原有 I/O 屏障，适用于本项目的 DMA 一致性平台。[磁盘缓冲区交接过程](block-io.md) · [固件和内核的使能位](../OS/entry.S)

## 写时复制如何重试整块清零

回归用例先把匿名页的一段写成 `P`，再 `fork`。子进程以块中间的字节为操作数执行 `cbo.zero`。父子仍共用只读页时，CPU 产生原因 15 的写页故障。内核替子进程复制整张旧页，恢复写权限并同步翻译，从原 `sepc` 重试。子进程读回 64 个零和左右两个 `P`，随后执行 `execv`。父进程等到子进程退出，仍能读回整段 `P`。这也解释了为什么分配新匿名页时可以调用 `zero_page`，拆分已有 COW 页时必须调用 `copy_page`。[来宾回归程序](../tests/argv_session.py) · [写时复制](virtual-memory.md)
