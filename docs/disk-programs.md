# 从磁盘文件到第一条用户指令

输入 `run hello.elf` 时，启动固件已经交出了磁盘。此时读文件的是 S 模式内核。它从目录项找到文件所在扇区，核对文件 CRC，然后从 ELF 程序头找出要映射的内容。最后，`sret` 把 CPU 交给 U 模式的入口。屏幕上的 `hello from disk ELF` 来自刚刚装入的可执行文件。下面这几步可以把一次串口输出对应到源码。

## 先用同一份内核换一张盘

先在工程根目录执行 `.\build.bat`。以下命令生成另一张启动盘，其中的内核和 ELF 字节完全相同，文件名和文本内容则由你指定。

```powershell
Set-Content -Encoding ascii build/debug/note.txt 'This file lives on my second disk.'
python tools/mkdisk.py build/debug/images/kernel.bin build/debug/second.img --file note.txt=build/debug/note.txt --file demo=build/debug/images/hello.elf
.\build\debug\bin\rs-vm.exe --firmware build/debug/images/firmware.bin --disk build/debug/second.img
```

在提示符输入 `ls`、`cat note.txt`、`run demo`、`halt`。能以名称 `demo` 运行，是因为 shell 按目录名称找文件，加载器读取文件里的 ELF 头来决定如何执行。替换成自己编译的兼容 ELF 时，也只需要重新制盘。源码中的回归用另一个很小的 ELF 演示了这一点，它能让原内核打印 `user exit: 37`。[制盘工具](../tools/mkdisk.py) · [换盘验收](../tests/elf_tests.py)

## 内核怎样找到文件

RSFS 使用单层只读目录，支持最多 15 个文件、每个最多 64 KiB。每个文件占连续扇区。启动扇区记录内核的有效字节数，内核据此跨过已经装入的载荷，找到目录所在的扇区。

1. **挂载。** 内核重新初始化 virtio 队列，用自己的描述符和缓冲区接管设备。它先检查目录魔数与 CRC，再检查所有名称和扇区范围。只有整个目录检查成功，`ls` 才能看到其中的文件。仅包含内核的旧盘没有磁盘文件，VFS 根目录仍会列出 `proc/`，内核生成的 `/proc/meminfo` 仍可读。
2. **打开。** `cat` 和 `run` 通过 VFS 打开路径。对于根目录的磁盘文件，RSFS 按目录项读取并缓存文件字节。末扇区可以有填充，文件 CRC 只计算目录记录的有效字节。一个文件损坏时，shell 仍可读取同盘其他文件。
3. **分派。** `cat` 输出文件字节。`run` 把同一缓冲区交给 ELF 加载器。磁盘扇区号在文件系统层已经用完，加载器处理的是文件偏移和用户虚拟地址。

目录与文件校验覆盖不同的数据。改动文件后，目录仍可以列出原来的名字和大小，读取该文件才会报告 `file CRC mismatch`。两个条目即使具有正确的目录 CRC，只要占用扇区相交，也会在挂载时被拒绝。[磁盘布局](boot.md) · [文件系统](../OS/kernel/fs.c) · [整盘回归](../tests/rsfs_tests.py)

## 读懂 hello.elf 的两个 LOAD 段

```powershell
llvm-objdump -p build/debug/images/hello.elf
llvm-objdump -d -M no-aliases build/debug/images/hello.elf | Select-String 'c\.lbu|czero\.eqz'
```

当前链接脚本产生如下布局。段长度和地址可用第一条命令核对，字段来自 ELF 的程序头。[链接脚本](../OS/user/disk.ld) · [ELF 程序头规范](https://gabi.xinuos.com/elf/07-pheader.html)

| 段 | 文件里的起点 | 运行时的虚拟地址 | 加载后能做什么 |
| --- | --- | --- | --- |
| RX | `0x1000` | `0x40010000` | 读常量、执行程序。用户对这页写入会触发页故障 |
| RW | `0x1ff8` | `0x40011ff8` | 初始数据从文件复制，内存尾部多出的字节清零。用户可以修改自己的数据 |

第二个段从页尾前 8 字节开始。当前样例的文件大小字段 `p_filesz` 是 21，运行所需大小 `p_memsz` 是 28。内核先把涉及的物理页清零，再只复制这 21 字节。这样既初始化了 `.bss`，也清掉了页首、页尾因页粒度映射而对用户可见的旧数据。

程序检查零初值计数器和字符串首字母，用 `c.lbu` 读字符，用 `czero.eqz` 筛选参与求和的字节。通过检查后，它把首字母改为小写，再把横跨两页的字符串交给 `write`。再次执行 `run hello.elf`，内核会重建映射并从盘上恢复大写首字母和零初值计数器。[用户程序](../OS/user/hello.c) · [两条指令的执行过程](compact-integer.md)

## 从验证 ELF 到返回 shell

加载器接受小端 RISC-V ELF64 静态 `ET_EXEC`、整数 LP64 调用约定。程序头按字节读取，先验证整个装载计划，再修改用户地址空间。可装入的地址范围是 `[0x40010000, 0x40020000)`，共 16 页。每个非空 `PT_LOAD` 段须位于该范围内，段按虚拟地址依次排列，各占独立权限页。支持 R、RX、RW 三种权限。入口须半字对齐，且位于文件提供的可执行字节内。[加载器](../OS/kernel/elf.c)

前台程序使用任务槽 0。内核收回该槽上一次装入的用户映射，保留私有用户栈，建立新页表项，并在进入 U 模式前执行 `fence.i`。切换页表时的 `sfence.vma` 使新的地址翻译生效，`fence.i` 让这颗 hart 后续取指看见刚写入的代码。两个操作针对不同的状态。程序正常退出或发生无法处理的用户态故障时，内核回收其描述符和匿名页并恢复 shell 的现场。访问已预留的匿名页可触发按需分配，成功时返回用户程序重试。[安装用户页](../OS/kernel/paging.c) · [准备现场](../OS/kernel/trap.c) · [取指同步与返回](../OS/kernel/trap.S)

默认盘上的程序使用同一套装载器。`run hello.elf` 验证代码、可写数据和输出，`run memory.elf` 把文件读进跨页匿名缓冲区，`run family.elf` 验证写时复制和继承的文件描述符。`run launch.elf` 则演示进程内的第二次装载：子进程通过 `exec` 从盘上换入 `hello.elf`，父进程继续执行原来的 `launch.elf` 并等待它退出。[进程如何更换映像](exec.md) · [系统调用约定](../OS/user/abi.h)
