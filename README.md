# Project 4 虚拟内存管理

本分支在Project 3的双核调度、Shell和进程间通信基础上加入Sv39虚拟内存，按C-Core要求全程使用双核运行，覆盖Task 1至Task 5。内核启动后创建Shell，由Shell按程序名加载用户程序。

## 实现内容

| 任务 | 当前实现 |
| --- | --- |
| Task 1 启用虚拟内存与执行用户程序 | 双核进入高地址内核后撤销启动用的临时恒等映射。每个进程有自己的三级页表，用户页为4 KiB。Loader按页装载程序、清零BSS并准备参数栈，调度时切换根页表和ASID。 |
| Task 2 按需调页 | 首次读写尚未映射的用户数据页时分配页框，并处理已有映射的访问位和脏位异常。保留P3互斥锁和同步接口。 |
| Task 3 Clock换页 | 以进程地址空间为单位选择替换页，通过BIOS读写交换区。访问换出的页时恢复整页数据，进程退出时回收对应交换槽。 |
| Task 4 多线程 | 同一进程的线程共享页表，各自拥有用户栈、内核栈和调度上下文。支持 `pthread_create()`、线程入口返回后退出以及 `pthread_join()`。 |
| Task 5 共享页 | 用 `sys_shmpageget(key)` 将同一键对应的物理页映射到各进程的空闲虚拟地址。用 `sys_shmpagedt(addr)` 解除映射，最后一个引用释放后回收页框。 |

### 地址空间与换页范围

- 用户程序入口为 `0x10000`。同一个程序的不同进程可以使用相同的虚拟地址，各自的低地址用户映射独立。各地址空间沿用高半区内核映射。
- 局部Clock当前给每个进程最多32个可换出的驻留匿名数据页，同一进程的线程共用该配额。全局有1024个4 KiB交换槽，共4 MiB。换出或解除映射时会同步刷新已上线核的TLB。
- 程序映像、线程栈和共享页常驻内存。页框耗尽时，匿名缺页路径可尝试在当前地址空间换出页，`exec` 的装载和建栈不会主动触发换页。
- 共享页的虚拟地址在 `[0x10000000, 0x10100000)` 中按空闲页选取。同一键首次申请时分配并清零物理页，进程退出会清理剩余共享映射。

## 编译与双核运行

在装有课程交叉工具链、QEMU和U-Boot的Linux环境中，从仓库根目录执行：

```bash
make clean
make all
make run-smp
```

Makefile默认使用 `riscv64-unknown-linux-gnu-` 工具链，从 `$(HOME)/OSLab-RISC-V/` 查找课程QEMU和U-Boot。`PROJECT_IDX = 4` 会将Shell和 `test/test_project4/` 中的程序编入镜像。

在U-Boot提示符下输入：

```text
loadbootm
```

启动后应看到CPU0和CPU1进入内核的日志，随后出现Shell提示符。需要GDB时，使用 `make debug-smp` 启动QEMU，再在另一终端执行 `make gdb`。

`build/image` 由 `build/kernel_image` 和 `build/user_image` 组成，后者从镜像的第2048号扇区开始（从0计数）。镜像尾部追加64 MB空间，内核根据程序目录和用户映像末尾确定交换区起点。

在开发板上，确认SD卡设备以及目标分区后可执行 `make floppy DISK=/dev/sdX`，将 `/dev/sdX` 换成实际设备。本仓库的写卡规则使用设备名加 `3` 作为分区路径。可用 `make minicom` 打开串口，默认设备为 `/dev/ttyUSB1`，在板上固件提示符下同样使用 `loadbootm`。

## Shell命令

| 命令 | 用途 |
| --- | --- |
| `ps` | 查看PID、TID、状态、CPU掩码、实际运行核和程序名。 |
| `exec name [args...]` | 按名称启动程序，并等待它退出。 |
| `exec name [args...] &` | 后台启动程序。 |
| `kill pid` | 结束指定进程及其线程。 |
| `taskset mask name` | 启动程序并设置CPU掩码。 |
| `taskset -p mask pid` | 修改指定任务的CPU掩码。 |
| `clear` | 清屏。 |

CPU掩码 `0x1` 对应CPU0，`0x2` 对应CPU1，`0x3` 允许在两个核上运行。`ps` 中，同一进程的线程PID相同、TID各自独立。

## 功能测试

以下命令在系统Shell中分组执行。每组给出预期现象，已有运行反馈见下一节。

### Task 1和Task 2 用户程序、缺页和互斥锁

```text
exec fly &
ps
exec rw 0x10800000 0x80200000 0xa0000320
```

通过飞机程序和 `ps` 检查用户程序能运行。`rw` 逐个写入并读回指定虚拟地址，检查打印 `Success!` 且全程无 `Error!`。也可以选择其他尚未映射的用户态地址复测。单独测试互斥锁时执行：

```text
exec lock 1 &
exec lock 8 &
```

两个进程使用同一把锁，观察一个进程释放后另一个进程才能进入临界区。`lock` 会循环运行，可用 `ps` 查看PID后结束测试进程。

### Task 3 Clock换页

```text
exec swaptest 1
```

`swaptest` 写满64个虚拟页，超过32页的驻留配额，再按反向、正向顺序逐字读取校验。`1` 指定输出起始行。数据一致时打印 `swaptest pid=<PID> PASS`。如需观察换入和换出次数，可在 `include/os/mm.h` 中开启 `SWAP_DEBUG` 后重新构建。

### Task 4 多线程和Mailbox

```text
exec threadtest
exec mailbox a &
exec mailbox b &
exec mailbox c &
ps
```

`threadtest` 执行20轮创建、等待和共享变量校验，成功时打印 `threadtest pid=<PID> PASS`。`mailbox` 的三个实例各有一个发送线程和一个接收线程。观察三组收发计数，并用 `ps` 检查每个进程下有两个TID。Mailbox连续运行，完成观察后可用三个进程的PID分别结束它们。

### Task 5 共享内存

```text
exec consensus 1
```

测试先获取并解除共享映射，再写入原虚拟地址触发普通匿名页分配。再次申请时，共享页应映射到另一个空闲虚拟地址。随后首个进程自动创建八个子进程，九个参与者每轮选出同一个PID，直到每个参与者都被选中一次，最后解除映射并退出。

## 主要代码位置

| 位置 | 内容 |
| --- | --- |
| `arch/riscv/boot/`、`arch/riscv/kernel/`、`init/main.c` | Bootblock、双核启动、初始映射、异常入口和系统调用注册。 |
| `kernel/loader/loader.c`、`tools/createimage.c` | 镜像元数据、用户程序装载和镜像生成。 |
| `kernel/mm/mm.c`、`kernel/irq/irq.c`、`kernel/smp/smp.c` | 页表与页框、缺页处理、Clock、共享页和跨核TLB刷新。 |
| `kernel/sched/sched.c`、`tiny_libc/pthread.c` | 进程和线程调度、参数传递、退出回收和用户态线程接口。 |
| `test/test_project4/` | P4用户测试程序。 |

设计讨论见 [P4 Design Review](docs/P4_Design_Review.md)。
