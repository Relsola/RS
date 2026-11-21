# 构建、复现与调试

一次构建有两个目标架构。宿主运行解释器。来宾从磁盘中取出的每条指令都是 RISC-V 机器码。两者共用 `clang` 驱动，链接环境各自独立。

| 产物 | 编译目标 | 链接与运行环境 |
| --- | --- | --- |
| Windows `rs-vm.exe` | 本机 x64，C++23，`CMAKE_CXX_COMPILER=clang` | CMake 指定系统链接器。`clang -fuse-ld=link` 调用微软 `link.exe`，使用 MSVC 标准库、CRT 和 Windows SDK |
| `firmware.elf` | `riscv64-unknown-elf`，RV64IM + Zicbom、Zicsr、Zifencei、Smstateen、Sstc，LP64 | `clang` 调用 `ld.lld`，由链接脚本决定入口和地址 |
| `kernel.elf` | C 代码采用 RV64IMAC + Zicbom、Zicboz、Zicsr、Zifencei、Sstc、Svinval、Svnapot，LP64 | freestanding C23。任务现场汇编在局部启用 D/V，按运行时探测结果调用 |
| `user.elf` | RV64IMAC，LP64，入口在用户虚拟地址 `0x40000000` | 两个任务执行同一用户镜像。代码和常量嵌入内核的只读页，内核分别分配并清零各任务的栈和数据页 |
| `hello.elf` | RV64IMAC + Zba、Zbb、Zcb、Zicond，LP64，用户装载区从 `0x40010000` 开始 | C23 程序与汇编辅助函数静态链接为 ELF64，可执行段和可写段分别记录在程序头中，整份 ELF 作为文件写入 RSFS |
| `memory.elf` | RV64IMAC，LP64，与 `hello.elf` 使用同一用户装载区 | 独立的 C23 程序，调用 `open/read/close` 和 `map/unmap`，观察匿名页的按需分配与跨页读文件 |
| `family.elf` | RV64IMAC，LP64，同一用户装载区 | 独立的 C23 程序，派生子进程并观察匿名页、文件偏移和退出码 |
| `launch.elf` | RV64IMAC，LP64，同一用户装载区 | 子进程默认运行 `hello.elf`，指定程序和参数时调用 `execv`，父进程收取退出结果 |
| `wc.elf` | RV64IMAC，LP64，同一用户装载区 | 从用户入口取得文件路径，通过 VFS 分块统计换行符、词和字节 |
| `numeric.elf` | RV64GCV、LP64D | C23 调用向量汇编，用双精度浮点 ABI 返回点积，随后执行派生与现场隔离实验 |
| `disk.img` | 512 字节扇区的原始磁盘 | `llvm-objcopy` 生成裸内核，Python 写入启动头、内核、RSFS 目录、`readme.txt` 和上述六个磁盘程序 |

在两端之间传输数据时，磁盘头、描述符和寄存器都有显式位宽与小端格式。来宾的汇编文件实现启动、特权切换、陷阱现场和系统调用入口。宿主键盘和终端的系统差异集中在 `console.cpp` 的平台条件分支。[宿主配置](../CMakeLists.txt) · [来宾配置](../OS/CMakeLists.txt) · [平台终端](../VM/console.cpp)

## 构建环境与产物

Windows 入口通过 Visual Studio 安装器的 `vswhere` 定位 x64 C++ 工具集，调用 `vcvars64.bat` 后执行 CMake。项目使用本机 PATH 上的 LLVM、CMake、Ninja 和 Python。本次验证环境为 x64 Windows、LLVM 23.1.2、MSVC 14.52、CMake 4.5.0、Ninja 1.13.2、Python 3.12.10。CMake 使用 3.29 引入的链接器选择功能。[Windows 入口](../build.bat) · [CMake Presets](../CMakePresets.json)

```powershell
.\build.bat
.\build\debug\bin\rs-vm.exe --firmware .\build\debug\images\firmware.bin --disk .\build\debug\images\disk.img
```

Debug 和 Release 分别写入 `build/debug` 和 `build/release`。每套配置的 `bin/` 保存宿主可执行文件，`images/` 保存带符号的来宾 ELF、裸镜像和最终启动盘。固件、内核和内置演示另生成 BIN。磁盘程序直接使用 ELF 的入口和装载信息。来宾在两种配置下均以 `-Os -g` 构建，便于读反汇编和复现磁盘格式。

可以单独替换盘上的文件。例如以下命令使用现有 Debug 内核另建一张盘。用上例的 VM 启动命令把 `--disk` 参数改成 `build/debug/images/alternate.img`，然后在 `rs>` 输入 `ls` 和 `run hello.elf`。制盘工具把每个 `--file` 的等号左侧作为来宾文件名，右侧作为本机输入路径。[制盘工具](../tools/mkdisk.py) · [磁盘文件与程序装载](disk-programs.md)

```powershell
python tools/mkdisk.py build/debug/images/kernel.bin build/debug/images/alternate.img --file hello.elf=build/debug/images/hello.elf
```

在已配置的开发者终端，可直接使用 `cmake --preset debug`、`cmake --build --preset debug` 和 `ctest --preset debug`。源代码提供 POSIX 终端分支，工程的本次运行验证覆盖 Windows x64。

## 哪个测试覆盖哪个边界

| CTest 名称 | 验证内容 |
| --- | --- |
| `boot.console` | 用真固件和真磁盘启动。检查命令结果、CRLF、退格、错误命令、超长行恢复及关机 |
| `boot.user` | 在同一次启动中运行两次用户程序，中间触发一次受保护设备读取故障。核对定时器计数前进、正常退出和 shell 恢复 |
| `boot.tasks` | 观察双方主动让出和同地址私有数据，验证定时器打断纯计算、恢复后的校验值及单任务故障后的运行顺序。用 PATH 中的 `llvm-nm` 读取本次 `user.elf` 的循环边界 |
| `boot.files` | 列目录、解析含 `.`、`..` 的路径，运行六个磁盘 ELF。检查 `wc.elf` 计数、`numeric.elf` 点积、跨页访问及父子扩展现场 |
| `disk.vfs_vm` | 在标准盘和替换盘运行真实用户程序，检查按需补页、按打开时间生成的内存快照、跨页读取、页池耗尽和任务回收 |
| `disk.fork` | 用替换盘检查父子页框计数、原子指令缺页重试、跨页读取失败时文件偏移与缓冲区的原子性、满池写入、退出顺序与定时器抢占 |
| `disk.rsfs` | 检查目录 CRC、重名、相交、容量越界、截断、独立文件校验和旧盘兼容。另用满 64 KiB 的文件覆盖缓存的所有子页，并执行文件末尾的入口 |
| `disk.block_io` | 在 shell 中核对批量磁盘读取、缓存命中和统计快照。启动后缩短一次性测试盘，检查部分完成的批次与失败后的缓存恢复 |
| `disk.exec` | 检查父子换入 ELF 后的入口、BSS、文件偏移、旧代码与回收。覆盖带引号和空参数、跨页字符串和指针表、数量和长度边界、失败回退，以及 U 模式块清零触发的写时复制 |
| `disk.elf` | 给同一内核换一张盘，观察新程序以 37 退出、向只读代码写入时触发页故障，以及多种无效 ELF 被拒绝。重复装载同一程序还会检查数据和 BSS 是否恢复初值 |
| `boot.ring-wrap` | 为内核载荷附加内容不同的扇区，让 8 项 virtqueue 重用槽位，核对整段 CRC 并启动 |
| `boot.bad-header`、`boot.bad-crc`、`boot.bad-size`、`boot.short-disk` | 分别破坏魔数、内核载荷、内核长度和扇区容量。确认固件报告对应原因，退出前未进入 shell |
| `vm.instructions` | 整数边界、Zicond、CSR、特权切换、PMP、Sv39、跨页取指及 Sstc。双根表用例执行真实的 `satp` 切换，分别验证同一虚拟地址的取指、数据和权限 |
| `vm.atomics` | 测试 LR/SC、九类 AMO 的两种宽度、符号扩展、页表别名、访问权限、碰撞后的重新尝试 |
| `vm.numeric` | 检查融合乘加、五种舍入、NaN、次正规数、FS/VS/SD、分数组、掩码访存、精确重试与按前缀完成的读取 |
| `vm.svnapot` | 用非恒等映射逐页取指，检查 64 KiB 的两端、跨页数据读写、保留编码、Svade 与 PMP |
| `vm.cache-blocks` | 检查清零和三种管理指令的块边界、M/S/U 授权、只读映射与 A/D 位、PMP、MMIO、LR 保留，以及预取提示 |
| `vm.bitmanip`、`vm.compressed` | 用已知编码检查位操作、压缩展开、Zcb、提示编码及保留编码 |
| `vm.virtio` | 比对磁盘和 DMA 缓冲区，检查完成长度、越界地址、只读请求、异常描述符链，以及设备改写 RAM 后 LR 保留是否失效 |

```powershell
.\build.bat test
.\build.bat release
ctest --preset release
```

自研 VM 的测试使用具体状态做断言。例如 CRC 用例修改第 1 扇区中的内核字节，并要求固件报告 `kernel CRC mismatch`。这样即使宿主意外绕过读盘、直接装载了旁边的 ELF，这条测试也会失败。[端到端测试](../tests/boot.py) · [设备测试](../VM/tests/virtio_tests.cpp)

### 可选的独立参考机

若已安装带 `rva23s64` CPU 型号的 `qemu-system-riscv64`，可以让它运行刚构建的**同一份固件与磁盘**。以下命令在工程根目录执行。本次验证使用 QEMU 11.1.0。

```powershell
python tests/reference.py (Get-Command qemu-system-riscv64).Source build/debug/images
```

脚本选择 `virt` 板和 `rva23s64,pmp=on`。此板的固件在进入 S 模式前设置 PMP。块设备使用现代 virtio-mmio 和只读原始镜像，固件与内核各自扫描设备插槽。向 CMake 提供 `-DRS_REFERENCE_QEMU=<可执行文件绝对路径>` 会额外注册四项对照。`reference.rva23s64` 检查启动与交互，`reference.exec` 检查更换映像，`reference.block_io` 检查批量读盘与快照，`reference.numeric` 比较浮点和向量探针的结果位及标志。[参考机](../tests/reference.py) · [映像替换](../tests/exec_tests.py) · [批量读盘](../tests/block_io_tests.py) · [数值指令](../tests/numeric_reference.py) · [QEMU virt 板](https://www.qemu.org/docs/master/system/riscv/virt.html)

参考脚本用 `-icount shift=6,align=off,sleep=off` 让 QEMU 的虚拟时间随来宾指令数推进。若按宿主墙钟计时，较快的主机可能让 A 在第一个时间片内算完整个循环。两台机器的回归均要求中断实际落在循环内，允许中断 PC 和总 tick 数因机器而异。[双任务验收](../tests/task_session.py) · [QEMU 指令计时](https://www.qemu.org/docs/master/devel/tcg-icount.html)

## 把一条日志对应到一条指令

见到 `M trap` 或 `S trap` 时，记下串口给出的 `epc`、`cause` 和 `tval`。用与该地址对应的固件或内核 ELF 反汇编。下面的命令会显示内核入口和建立栈的代码。

```powershell
llvm-objdump -d --start-address=0x80200000 --stop-address=0x80200060 build/debug/images/kernel.elf
```

要区分 CPU 执行问题和来宾逻辑问题，可在 Windows 调试器中打开 `rs-vm.exe` 并带上平时的两个镜像参数。在 `cpu_step` 观察 `cpu->pc` 与 `cpu->privilege`。在 `cpu_trap` 观察负责接收异常的特权级。命令行选项 `--max-steps N` 可以限制执行步数，达到上限时宿主输出当前 PC。输入由管道提供时，以 `halt` 收尾，便于验证正常关机。[CPU 执行入口](../VM/cpu.cpp) · [特权实现](../VM/cpu_privilege.cpp)
