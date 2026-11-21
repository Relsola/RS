# RS · 一台从磁盘启动的 RISC-V 计算机

RS 把一台计算机的两侧放在一个工程里。`VM/` 是用 C 风格 C++23 写的 64 位 RISC-V 解释器，`OS/` 是这台机器执行的启动固件和 C23 内核。输入 `echo hello` 时，从键盘读取、串口寄存器、页表翻译到命令解析，都能在源码里找到。项目以 **RVA23S64** 为长期标准目标，当前交付的是能运行自制 OS 的教学子集。已实现的能力和标准差距见[规范覆盖](docs/rva23s64.md)。

## 在 Windows 上运行

准备 LLVM（构建用 `clang`、`ld.lld`、`llvm-objcopy`，抢占测试还用到 `llvm-nm`）、Visual Studio 的 x64 C++ 构建工具和 Windows SDK、CMake 3.29 或更新版、Ninja、Python 3。让 LLVM、CMake、Ninja 和 Python 进入 PATH。在工程根目录打开 PowerShell，用下面的命令启动。

```powershell
.\build.bat run
```

首次运行会构建虚拟机、固件和启动盘。下面是一段实际的串口会话。

```text
rs> cat readme.txt
Welcome to RS.
The kernel read this file from the virtual disk.
rs> run hello.elf
hello from disk ELF
user exit: 0
rs> halt
System halted.
```

`help` 列出命令，退格可编辑当前行。`halt` 从内核发起关机，返回宿主终端。Shell 和内核运行在 S 模式，使用只读启动盘。下面的命令可以观察用户程序进入内核，以及两个用户任务共用一颗 CPU 的过程。

| 在 `rs>` 输入 | 可以观察什么 |
| --- | --- |
| `ls /`、`cat /readme.txt` | 通过虚拟文件系统列出根目录，读取只读磁盘上的文件 |
| `cat /proc/meminfo` | 打开一个由内核即时生成的文件，查看匿名页预留数、实际驻留数和累计处理的用户缺页数 |
| `run hello.elf` | 内核从磁盘装入独立编译的 RISC-V ELF，设置代码和数据页的权限，在 U 模式执行，程序退出后回到 shell |
| `run /memory.elf` | 用户程序申请内存，首次访问时触发缺页，再把磁盘文件读入跨页缓冲区，输出并释放内存 |
| `run /family.elf` | 父进程派生子进程。观察同一地址上父读到 `P`、子读到 `C`，继承的文件描述符则从同一游标接着读 |
| `run /launch.elf` | 子进程执行 `exec` 换入磁盘上的 `hello.elf`。父进程继续执行原程序，等到子进程退出后打印完成消息 |
| `ticks` | 查看内核实际处理过的监管定时器中断次数 |
| `user` | 进入 U 模式，等待一次计时进展，通过系统调用打印 `hello from user mode`，随后返回 shell |
| `user fault` | 用户程序读取只允许监管态访问的串口页，内核报告页故障。回到提示符后可以继续输入命令 |
| `tasks yield` | A、B 主动交出 CPU。它们使用同一虚拟地址，切换后各自读回 11 和 22 |
| `tasks preempt` | A 进行纯计算时被时钟中断，B 先完成，A 恢复后输出校验值 `0x0ed55926` |
| `tasks fault` | A 读取受保护的串口页后终止，B 仍能完成并回到 shell |
| `tasks memory` | 两个任务各自持有匿名页和文件描述符 3。切换后，各自读回原来的字节和文件位置 |

| Windows 入口 | 作用 |
| --- | --- |
| `.\build.bat` | 构建 Debug，生成宿主程序和来宾镜像 |
| `.\build.bat run` | 构建并启动交互系统 |
| `.\build.bat test` | 构建并运行 CPU、设备、启动及错误路径测试 |
| `.\build.bat release` | 生成独立的 Release 构建 |

日常运行使用自研解释器。QEMU 仅供可选的交叉验证。宿主和来宾分别调用 `clang`，Windows 宿主用微软的 `link.exe` 和运行库，RISC-V 来宾用 `ld.lld` 和自己的启动代码。构建产物、调试方式和复现命令见[构建与验证](docs/build.md)。

## 从哪里开始读

1. [整机架构与指令执行](docs/architecture.md)给出物理地址图。跟随 `cpu_step` 看一条指令如何改变机器状态。
2. [从虚拟磁盘启动到第一条命令](docs/boot.md)跟踪固件读盘、内核接管和串口交互。
3. [特权与地址翻译](docs/privilege-memory.md)解释一次内存访问如何经过 Sv39 和 PMP，以及异常发生时可以看哪些寄存器。
4. [用户程序、系统调用与定时器](docs/user-timer.md)追踪同一个程序进入内核的三条路径，分别对应系统调用、时钟中断和页故障。
5. [单核上的两个用户任务](docs/tasks.md)从串口输出找到 A 被抢占的那一条指令，再看 B 如何取得自己的寄存器和内存。
6. [原子指令与保留集](docs/atomics.md)解释 LR/SC、设备写入与任务切换之间的关系。
7. [从磁盘文件到用户指令](docs/disk-programs.md)跟踪 `run hello.elf`，包含给磁盘添加文件和替换程序的步骤。
8. [从反汇编看 Zcb 和 Zicond](docs/compact-integer.md)解释上述程序实际执行的压缩访存和条件清零指令。
9. [虚拟文件系统](docs/vfs.md)解释磁盘文件和内核生成的 `/proc/meminfo` 如何共用路径、打开、读取接口。
10. [匿名虚拟内存](docs/virtual-memory.md)追踪预留地址、按需分配物理页、重试指令与释放映射。
11. [跟踪一次 fork 和 wait](docs/fork.md)把同一段串口输出对应到陷阱帧、共享页框和文件游标。
12. [从 fork 到 exec](docs/exec.md)跟踪父子进程各自更换程序时，代码页、寄存器和打开文件的去向。
13. [Svnapot 的 64 KiB 映射](docs/svnapot.md)与 [Svinval 的批量失效](docs/svinval.md)分别对应磁盘缓存和写时复制的内核代码。
14. [RVA23S64 规范覆盖](docs/rva23s64.md)列出已实现的能力及标准差距。
