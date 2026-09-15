# 操作系统研讨课 Project 1 Design Review

## 1 实验目标

### 1.1 实验目标

本实验计划完成以下功能：

1. 编写 Bootloader，并通过 BIOS 接口在终端输出启动信息。
2. 由 Bootloader 从 SD 卡或 QEMU 虚拟磁盘读取内核，将其装载到 `0x50201000`。
3. 在内核入口代码中清零 BSS、设置栈指针，然后进入 C 语言编写的内核 `main` 函数。
4. 制作包含 Bootloader、Kernel 和多个用户程序的 image 镜像。
5. 在内核中根据 task id 或应用名称选择程序，装载后跳转执行。
6. 在应用入口 `crt0.S` 中准备 C 语言运行环境，并在应用结束后返回内核。

### 1.2 地址用途

| 地址 | 用途 |
|---|---|
| `0x50000000` | BIOS/BBL 所在区域起始 |
| `0x50150000` | BIOS 功能统一入口 `bios_func_entry` |
| `0x50200000` | Bootloader 装载地址 |
| `0x502001fc` | Bootloader 中保存内核大小的字段地址 |
| `0x50201000` | Kernel 装载地址和入口地址 |
| `0x50500000` | Kernel 栈顶地址 |
| `0x51ffff00` | Kernel Jump Table 地址 |
| `0x52000000` | 第一个App的链接和装载地址，后续每个相隔 `0x10000` |

### 1.3 BIOS 函数调用号

| 名称         | 调用号 |
| ------------ | ------ |
| BIOS_PUTCHAR | 1      |
| BIOS_GETCHAR | 2      |
| BIOS_PUTSTR  | 9      |
| BIOS_SDWRITE | 10     |
| BIOS_SDREAD  | 11     |

## 2 P1 代码框架

| 文件或目录 | 主要作用 | 对应任务 |
|---|---|---|
| `arch/riscv/boot/bootblock.S` | Bootloader 入口，调用 BIOS 输出、读取内核并跳转 | Task 1、2、4 |
| `arch/riscv/kernel/head.S` | Kernel 汇编入口，清零 BSS、设置栈、进入 `main` | Task 2 |
| `init/main.c` | 内核主函数，初始化跳转表、处理输入、选择应用 | Task 2、3、4、5 |
| `kernel/loader/loader.c` | 根据应用信息从镜像装载用户程序 | Task 3、4、5 |
| `arch/riscv/crt0/crt0.S` | 用户程序入口，建立 C 语言运行环境并返回内核 | Task 3 |
| `tools/createimage.c` | 在宿主 Linux 上解析 ELF 并生成 image | Task 3、4、5 |
| `include/os/task.h` | 定义应用数量、内存区域及 `task_info_t` | Task 3、4 |
| `include/os/kernel.h` | 提供跳转表及 BIOS 包装接口 | Task 2 以后 |
| `arch/riscv/bios/common.c` | 将 C 函数调用转换成 BIOS 调用 |                 |
| `arch/riscv/include/asm/biosdef.h` | 定义 BIOS 功能号 |                 |
| `riscv.lds` | 规定程序入口、Section 顺序和 BSS 边界 |                 |
| `Makefile` | 完成交叉编译、链接、镜像制作和 QEMU 启动 |                 |

## 3 BIOS 调用方式

### 3.1 BIOS 接口约定

在 Bootloader 运行时，内核还没有建立栈和跳转表，因此 Bootloader 直接按照实验规定的寄存器约定调用 BIOS：

- `a7` 保存 BIOS 功能号；
- `a0` 至 `a4` 依次保存参数；
- `jal bios_func_entry` 跳转到 `0x50150000`；
- 返回值位于 `a0`。

### 3.2 bootBlock 调用 BIOS 函数

直接通过上述约定进行调用，即通过`li`等将参数写入相应寄存器，在`a7`写入调用号后，`jal bios_func_entry`进行调用

比如在Task 1中：

```assembly
li a7, BIOS_PUTSTR 
la a0, msg
jal bios_func_entry
```

我认为可以将这种方式类比为OS理论课第一次作业中内联汇编的调用方式。

### 3.3 Kernel 调用 BIOS 函数

Kernel 不直接操作寄存器进行调用，而是使用**跳转表**来调用 BIOS 函数。

一种可能的跳转表如下：

```
CONSOLE_PUTSTR  -> port_write
CONSOLE_PUTCHAR -> port_write_ch
CONSOLE_GETCHAR -> port_read_ch
SD_READ         -> sd_read
```

Kernel 通过调用`call_jmptab(CONSOLE_PUTSTR)`找到`port_write()`，从而调用BIOS的字符串打印功能。

我认为可以将这种调用方式类比为OS理论课第一次作业中`glibc`的调用方式。

## 4 Kernel 装载并启动 App

### 4.1 根据 task id 选择应用

Task 3 中，内核读取用户输入的数字字符，将 ASCII 码转换为 task id，`load_task_img(taskid)` 计算应用起始扇区和目标内存地址，再调用 `bios_sd_read()`。装载完成后，函数返回该应用的入口地址，然后内核把入口地址转换为函数指针并调用。

### 4.2 根据应用名称选择应用

Task 4 中，内核逐字符接收应用名，遇到回车后结束输入，然后遍历 `tasks[]` 比较名字。找到对应 `task_info_t` 后，Loader 使用其中的镜像偏移、文件大小和装载地址读取应用。

### 4.3 应用入口 `crt0.S`

每个应用的 C `main` 之前都链接了 `crt0.S`。内核跳转到应用入口时，首先执行的是 `_start`。在其中完成：

1. 保存内核调用应用时的返回地址和栈指针。
2. 为应用设置不会覆盖代码、数据和 BSS 的栈，并保持 16 字节对齐。
3. 清零该应用的 `__bss_start` 至 `__BSS_END__`。
4. 调用应用的 C `main` 函数。
5. 应用 `main` 返回后，恢复内核栈和返回地址。
6. 使用 `ret` 返回内核。

## 5 Image 文件的组成和制作

Bootloader、Kernel 和各 App 会先被分别编译为 ELF 文件。`createimage` 遍历 ELF 的 Program Header，将 `PT_LOAD` 段中实际存在的 `p_filesz` 字节依次写入 image，形成可供 BIOS 读取的镜像。

Task 3 为 Kernel 和每个 App 预留相同大小的存储空间。程序写入后，剩余部分用 0 填充，因此可以根据 task id 直接计算对应 App 在 image 中的起始扇区。

```text
Bootblock（0 号扇区）| Kernel 填充区 | App 0 填充区 | App 1 填充区 | ...
```

这样每个App都是从扇区起始位置开始的，因此可以根据`task_id`直接定位。第一个扇区的偏移 `0x1fc` 保存 Kernel 扇区数，偏移 `0x1fe` 至 `0x1ff` 保存启动签名 `0x55 0xaa`。

Task 4 去掉填充区，使各部分按实际大小紧密排列：

```text
Bootblock | Kernel | App 0 | App 1 | ... | App Info
```

此时应用位置不能再由`task_id`直接推算，需要在 App Info 中记录应用名、镜像偏移、大小和入口地址。`createimage.c` 和内核中的 `task_info_t` 必须使用一致的字段定义。

## 6 TODO

- [x] Task 1：bootloader输出指定内容
- [x] Task 2：
  - [x] bootloader加载内核
  - [x] 清零bss，设置栈指针，跳转到内核起始
  - [x] `main.c`回显键盘内容
- [x] Task 3：
  - [x] 编写`createimage.c`，为内核添加信息
  - [x] `main.c`通过task id来开启App
  - [x] `loader.c`来辅助加载App
  - [x] `crt0.S` 初始化 App 的 C 语言运行环境
- [x] Task 4：
  - [x] 使用真实大小来生成image
  - [x] 使用App Name来启动App
- [ ] Task 5：
  - [ ] 将内核和App分两个image；内核镜像需要保存App相关信息
  - [ ] 加载内核，只加载内核镜像，可以提供显示App的指令
  - [ ] 启动App时从App镜像加载App
