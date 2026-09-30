# Project 2：简易内核

## 项目简介

本项目在Project 1完成的Bootloader、Kernel和用户程序加载功能之上，实现了一个运行于RISC-V平台的简易操作系统内核。内核能够创建多个用户进程，保存和恢复进程上下文，并通过就绪队列完成进程调度；用户程序通过系统调用进入内核，定时器中断则用于实现抢占式调度。

项目还实现了阻塞、唤醒、互斥锁和睡眠等基本功能。Task 5在普通轮转调度的基础上加入动态时间片分配，使计算量不同的5个`fly`进程能够保持近似一致的长期飞行进度。

## 实现内容

- Task 1：实现PCB、就绪队列、用户栈和内核栈，并构造进程第一次运行所需的初始现场；实现`switch_to`和非抢占式轮转调度。
- Task 2：实现进程的阻塞与唤醒、自旋锁以及带阻塞队列的互斥锁。
- Task 3：实现RISC-V例外入口、用户现场的保存与恢复以及基于`ecall`的系统调用；支持用户态与内核态切换、进程睡眠和系统时间读取。
- Task 4：实现Supervisor Timer Interrupt，并在用户程序不主动调用`sys_yield`时完成抢占式调度和睡眠进程唤醒。
- Task 5：实现`set_sche_workload`系统调用，根据各进程报告的实时进度动态调整时间片，使速度不同的飞机保持近似同步。

## Task 5调度策略

每个`fly`进程通过`sys_set_sche_workload`报告当前一轮的剩余路程。内核根据剩余路程的回跳判断新一轮开始，并计算进程从启动以来的累计进度：

```text
累计进度=已完成轮数×每轮长度+当前轮已经完成的路程
```

内核找出所有飞机中的最大进度和最小进度，再把每个进程的相对落后程度映射到`MIN_TIME_SLICE`至`MAX_TIME_SLICE`之间：

```text
时间片=最小时间片+
       当前落后量×可增加的时间片范围÷最大落后量
```

领先进程仍至少获得一个时间片，避免飞机完全停止；落后越多的进程获得越多时间片，从而逐渐追赶领先进程。该算法不直接读取或写死测试程序中的`CYCLE_PER_MOVE`。

## 主要目录

```text
arch/riscv/        RISC-V启动、上下文切换和例外入口代码
drivers/           屏幕和设备驱动
include/           内核数据结构及接口声明
init/              内核初始化、PCB初始化和系统调用表注册
kernel/            调度、锁、加载器、时间和系统调用实现
tiny_libc/         用户态C库及系统调用封装
test/test_project2 用户测试程序
tools/             镜像制作工具
docs/              Design Review文档
```

## 编译与运行

在Linux实验环境中执行：

```bash
make clean
make all
make run
```

QEMU启动后输入：

```text
loadbootd
```

当前内核会自动加载并运行`fly1`至`fly5`。正常情况下，5架飞机会持续移动，各进程的累计移动进度长期保持接近。

退出QEMU时，先按`Ctrl+A`，松开后再按`X`。

## 写入SD卡

Makefile中的默认设备为`/dev/sdb`。写入前必须使用`fdisk -l`或`lsblk`确认SD卡的实际设备名，避免覆盖系统磁盘。确认无误后执行：

```bash
make floppy
```

该命令会把生成的`build/image`写入目标设备的第3个分区。

## 调试

启动等待GDB连接的QEMU：

```bash
make debug
```

在另一个终端中连接：

```bash
make gdb
```

调试调度器时，可以重点检查`current_running`以及各PCB中的`status`、`progress`、`time_slice`和`ticks_left`字段。
