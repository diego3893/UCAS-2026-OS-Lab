# Project 1：Bootloader

## 项目简介

本项目实现了 RISC-V 裸机环境下从 Bootloader 到 Kernel，再到用户程序的完整启动与加载流程。系统启动后，Bootloader 负责输出启动信息、读取 Kernel 并将控制权交给 Kernel；Kernel 根据用户输入的程序名查找 App Info，在运行时从磁盘镜像中加载并执行对应的用户程序。

## 实现内容

- Task 1：Bootloader 通过 BIOS 串口接口输出启动信息。
- Task 2：Bootloader 从磁盘加载 Kernel；Kernel 启动代码清空 `.bss`、设置栈并进入 C 语言入口函数。
- Task 3：Kernel 根据程序信息从镜像中加载并运行用户程序。
- Task 4：在镜像中保存 App Info，支持按照程序名称查找和加载用户程序。
- Task 5：将 Kernel 和用户程序分别生成 `kernel_image` 与 `user_image`，启动时只加载 Kernel，用户程序在运行时按需加载。

## 编译与运行

```bash
make clean
make all
make run
```

QEMU 启动后，输入：

```text
loadboot
```

系统启动后会提示：

```text
Please input task name:
```

可以输入以下程序名称：

```text
bss
data
auipc
2048
```

其中，`bss`、`data` 和 `auipc` 用于检查程序加载与运行是否正确，`2048` 为交互式测试程序。

退出 QEMU：先按 `Ctrl+A`，松开后再按 `X`。
