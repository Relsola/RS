# 构建、复现与调试

一次构建有两个目标架构。宿主运行解释器。来宾从磁盘中取出的每条指令都是 RISC-V 机器码。两者共用 `clang` 驱动，链接环境各自独立。

| 产物 | 编译目标 | 链接与运行环境 |
| --- | --- | --- |
| Windows `rs-vm.exe` | 本机 x64，C++23，`CMAKE_CXX_COMPILER=clang` | CMake 指定系统链接器。`clang -fuse-ld=link` 调用微软 `link.exe`，使用 MSVC 标准库、CRT 和 Windows SDK |
| `firmware.elf` | `riscv64-unknown-elf`，RV64IM + Zicsr + Zifencei + Sstc，LP64 | `clang` 调用 `ld.lld`，由链接脚本决定入口和地址 |
| `kernel.elf` | 同一目标，另外启用 A 原子指令和 C 压缩指令，LP64 | freestanding C23，使用自编入口、串口和内核函数 |
| `user.elf` | RV64IMAC，LP64，入口在用户虚拟地址 `0x40000000` | 两个任务执行同一用户镜像。代码和常量嵌入内核的只读页，内核分别分配并清零各任务的栈和数据页 |
| `disk.img` | 512 字节扇区的原始磁盘 | `llvm-objcopy` 生成裸二进制，Python 标准库写入磁盘头、CRC 和内核 |

在两端之间传输数据时，磁盘头、描述符和寄存器都有显式位宽与小端格式。来宾的汇编文件实现启动、特权切换、陷阱现场和系统调用入口。宿主键盘和终端的系统差异集中在 `console.cpp` 的平台条件分支。[宿主配置](../CMakeLists.txt) · [来宾配置](../OS/CMakeLists.txt) · [平台终端](../VM/console.cpp)

## 构建环境与产物

Windows 入口通过 Visual Studio 安装器的 `vswhere` 定位 x64 C++ 工具集，调用 `vcvars64.bat` 后执行 CMake。项目使用本机 PATH 上的 LLVM、CMake、Ninja 和 Python。本次验证环境为 x64 Windows、LLVM 23.1.2、MSVC 14.52、CMake 4.5.0、Ninja 1.13.2、Python 3.12.10。CMake 使用 3.29 引入的链接器选择功能。[Windows 入口](../build.bat) · [CMake Presets](../CMakePresets.json)

```powershell
.\build.bat
.\build\debug\bin\rs-vm.exe --firmware .\build\debug\images\firmware.bin --disk .\build\debug\images\disk.img
```

Debug 和 Release 分别写入 `build/debug` 和 `build/release`。每套配置的 `bin/` 保存宿主可执行文件，`images/` 保存固件和内核的 ELF、BIN 及最终启动盘。ELF 保留符号，BIN 只携带装入内存所需的初始字节。来宾在两种配置下均以 `-Os -g` 构建，便于读反汇编和复现磁盘格式。

在已配置的开发者终端，可直接使用 `cmake --preset debug`、`cmake --build --preset debug` 和 `ctest --preset debug`。源代码提供 POSIX 终端分支，工程的本次运行验证覆盖 Windows x64。

## 哪个测试覆盖哪个边界

| CTest 名称 | 验证内容 |
| --- | --- |
| `boot.console` | 用真固件和真磁盘启动。检查命令结果、CRLF、退格、错误命令、超长行恢复及关机 |
| `boot.user` | 在同一次启动中运行两次用户程序，中间触发一次受保护设备读取故障。核对定时器计数前进、正常退出和 shell 恢复 |
| `boot.tasks` | 观察双方主动让出和同地址私有数据，验证定时器打断纯计算、恢复后的校验值及单任务故障后的运行顺序。用 PATH 中的 `llvm-nm` 读取本次 `user.elf` 的循环边界 |
| `boot.ring-wrap` | 为内核载荷附加内容不同的扇区，让 8 项 virtqueue 重用槽位，核对整段 CRC 并启动 |
| `boot.bad-header`、`boot.bad-crc`、`boot.bad-size`、`boot.short-disk` | 分别破坏魔数、内核载荷、内核长度和扇区容量。确认固件报告对应原因，退出前未进入 shell |
| `vm.instructions` | 整数边界、CSR、特权切换、PMP、Sv39、跨页取指及 Sstc。双根表用例执行真实的 `satp` 切换，分别验证同一虚拟地址的取指、数据和权限 |
| `vm.atomics` | 测试 LR/SC、九类 AMO 的两种宽度、符号扩展、页表别名、访问权限、碰撞后的重新尝试 |
| `vm.bitmanip`、`vm.compressed` | 用已知编码检查位操作、压缩展开、提示编码及保留编码 |
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

脚本选择 `virt` 板和 `rva23s64,pmp=on`。此板的固件在把控制权交给 S 模式前会设置 PMP，因此显式打开该机器级设施。块设备设为现代 virtio-mmio、只读原始镜像，固件自行扫描设备插槽。测试通过本机回环串口等待提示符，检查 `info`、回显，并执行与自研 VM 相同的用户态和双任务会话。向 CMake 提供 `-DRS_REFERENCE_QEMU=<可执行文件绝对路径>` 会额外注册 `reference.rva23s64`。[参考机测试](../tests/reference.py) · [QEMU virt 板文档](https://www.qemu.org/docs/master/system/riscv/virt.html)

参考脚本用 `-icount shift=6,align=off,sleep=off` 让 QEMU 的虚拟时间随来宾指令数推进。若按宿主墙钟计时，较快的主机可能让 A 在第一个时间片内算完整个循环。两台机器的回归均要求中断实际落在循环内，允许中断 PC 和总 tick 数因机器而异。[双任务验收](../tests/task_session.py) · [QEMU 指令计时](https://www.qemu.org/docs/master/devel/tcg-icount.html)

## 把一条日志对应到一条指令

见到 `M trap` 或 `S trap` 时，记下串口给出的 `epc`、`cause` 和 `tval`。用与该地址对应的固件或内核 ELF 反汇编。下面的命令会显示内核入口和建立栈的代码。

```powershell
llvm-objdump -d --start-address=0x80200000 --stop-address=0x80200060 build/debug/images/kernel.elf
```

要区分 CPU 执行问题和来宾逻辑问题，可在 Windows 调试器中打开 `rs-vm.exe` 并带上平时的两个镜像参数。在 `cpu_step` 观察 `cpu->pc` 与 `cpu->privilege`。在 `cpu_trap` 观察负责接收异常的特权级。命令行选项 `--max-steps N` 可以限制执行步数，达到上限时宿主输出当前 PC。输入由管道提供时，以 `halt` 收尾，便于验证正常关机。[CPU 执行入口](../VM/cpu.cpp) · [特权实现](../VM/cpu_privilege.cpp)
