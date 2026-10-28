# Project3：进程管理、通信与多核执行

## 项目简介

本项目在Project1的镜像加载和Project2的进程调度、异常处理、系统调用基础上，实现进程的动态创建与退出、Shell终端、进程间同步与通信，以及RISC-V双核调度、CPU亲和性和按资源拆分的内核锁。项目以P3的C-Core要求为目标。

内核启动后创建PID为1的Shell，由Shell按程序名启动测试任务。当前Makefile使用`PROJECT_IDX = 3`，编译`test/shell.c`及`test/test_project3/`中的用户程序，不再自动启动P2的五架飞机。

## 实现内容

| 任务 | 当前实现 |
|---|---|
| Task1：终端与进程管理 | 输入回显、退格、命令解析、上下分屏、`ps`、`clear`、前后台`exec`、`kill`，以及`exec/exit/waitpid/getpid`系统调用和退出资源清理 |
| Task2：同步与通信 | 可重复使用的Barrier、支持等待/单个唤醒/广播的Condition，以及按名字打开、带阻塞收发和引用计数的Mailbox |
| Task3：双核运行 | IPI唤醒从核、独立启动栈和idle上下文、每核定时器与异常入口、共享就绪队列，以及大内核锁保护 |
| Task4：CPU亲和性 | 两种`taskset`命令、子进程继承CPU掩码、按掩码调度，以及`ps`显示允许核与实际运行核 |
| Task5：按资源拆锁 | 保留进程状态锁，独立保护屏幕资源，支持大锁/细粒度配置切换，并提供生产消费吞吐量测试 |

上表描述源码中的实现范围，不代表全部C-Core用例均已完成运行验证。已有测试反馈见文末。

## 关键设计

### 进程、参数与资源回收

- 普通进程使用16个静态PCB槽位，Shell也占一个槽位；槽位可以复用，PID保持递增。
- `sys_exec(name, argc, argv)`按程序名创建进程。内核把参数字符串和指针数组复制到新进程用户栈，补上`argv[argc] = NULL`，通过`a0/a1`传入`argc/argv`，用户栈保持16字节对齐。
- 创建进程时分别分配一页内核栈和一页用户栈。退出或被杀死时清理等待关系、持有的互斥锁和Mailbox引用，并唤醒等待该进程的任务。
- 栈页面通过`freePage`归还内核页池或用户页池，后续单页分配优先复用；当前仍在使用的内核栈通过`recycle_pending`和`reap_exited`延迟回收。
- 对另一核正在运行的进程，`kill`先设置`kill_pending`，由目标进程下一次陷入内核时退出，避免提前回收它正在使用的栈。

### 同程序多实例

每个程序按链接地址占用固定64KiB映像槽位。同一程序有存活实例时，新实例复用已加载的程序映像，不重复覆盖或清空BSS；各实例拥有独立PCB、用户栈和内核栈。

当前尚未实现虚拟内存隔离，同程序实例的全局可写数据仍然共享。测试中的实例私有状态应放在各自栈上；共享数据需要同步保护。

### 双核与调度

主核清空内核BSS并初始化全局数据，再通过IPI唤醒从核。从核跳过全局初始化，只设置自身运行上下文、异常入口和定时器。两个核分别使用独立启动栈和idle上下文，`current_running`绑定到各核的`tp`寄存器。

两个核共享就绪队列。调度器选择允许在本核运行的就绪进程，没有合适任务时运行本核idle。异常保存现场后获取`kernel_lock`，切换过程保持持锁，在异常返回路径释放；细粒度模式中的屏幕路径会在中途暂时释放它。

### 同步与Mailbox

Barrier在规定人数到齐后唤醒全部等待者，重置计数供下一轮使用。Condition等待将“进入等待队列、释放原互斥锁、调度”置于受保护的内核路径中；恢复后重新获取原互斥锁，调用者用`while`重新检查共享数据条件。

Mailbox使用64字节环形FIFO，内部包含互斥锁和`not_full/not_empty`条件变量。空间不足时发送者等待，数据不足时接收者等待；状态改变后广播唤醒，再由等待者重新检查条件。`send/recv`成功返回因空间或数据不足而阻塞的次数，非法请求返回`-1`，返回值不是传输字节数。最后一个引用关闭后，邮箱槽位可以复用。

### 细粒度锁

锁配置位于`include/os/smp.h`：

```c
#define FINE_GRAINED_LOCK 1 // 0：大内核锁；1：细粒度锁
```

当前采用最小改动的资源分组方案：

| 锁 | 保护范围 |
|---|---|
| `kernel_lock` | 进程状态、就绪/睡眠/阻塞队列、同步对象状态和相关资源管理 |
| `screen_lock` | 屏幕缓冲区、光标操作和终端刷新 |
| `bios_lock` | 单次BIOS调用；两种配置均保留 |

细粒度模式下，`WRITE/CURSOR/REFLUSH/CLEAR`系统调用执行屏幕操作前释放`kernel_lock`，获取`screen_lock`；完成后先释放屏幕锁，再重新获取状态锁。因此，一个核刷新飞机时，另一个核可以同时执行生产消费的同步系统调用。

定时器仅尝试获取屏幕锁：失败就跳过本次附加刷新，继续处理睡眠与调度；成功则在释放状态锁后刷新屏幕。下一次定时器在刷新和相关锁操作结束后重新预约，避免这些耗时操作使新预约的时刻在返回前就已过期。内核`printk`也使用屏幕锁，但不会主动释放调用者已有的状态锁。

该实现没有为每个条件变量或Mailbox分别设置独立的内核状态锁；这些状态操作仍由`kernel_lock`串行保护。拆锁主要减少屏幕输出对进程管理和同步路径的干扰。

## 主要目录

```text
arch/riscv/         Bootloader、双核入口、上下文切换、异常入口和用户启动代码
drivers/            屏幕驱动与共享屏幕缓冲区
include/            PCB、同步对象和内核接口声明
init/               主从核初始化和系统调用注册
kernel/sched/       进程创建、调度、退出回收和CPU亲和性
kernel/locking/     互斥锁、Barrier、Condition和Mailbox
kernel/mm/          页分配与空闲页链表
kernel/smp/         内核状态锁、屏幕锁和从核唤醒
kernel/syscall/     系统调用分发及屏幕路径的锁切换
tiny_libc/          用户态C库及系统调用封装
test/shell.c        Shell终端
test/test_project3/ P3测试程序
tools/              镜像制作工具
docs/               Design Review文档
```

## 编译与双核运行

在配置好课程交叉工具链、QEMU和U-Boot的Linux环境中，于仓库根目录执行：

```bash
make clean
make all
make run-smp
```

Makefile默认使用`riscv64-unknown-linux-gnu-`工具链，并从`$(HOME)/OSLab-RISC-V/`查找课程QEMU及U-Boot；按实际环境调整对应路径。双核QEMU使用`-smp 2`，U-Boot提示符下输入：

```text
loadbootm
```

启动后应看到两个核进入`main`的日志，随后出现Shell提示符。单核调试可以使用`make run`并输入`loadbootd`，但不能据此验证双核任务。退出QEMU时，先按`Ctrl+A`，松开后再按`X`。

主要构建产物为`build/image`、`build/kernel_image`和`build/user_image`。`image`是组合镜像，用户镜像从第2048扇区开始。

### 开发板与SD卡

Makefile默认`DISK = /dev/sdb`，`make floppy`会把`build/image`写入`${DISK}3`。先用`lsblk`核对设备及目标分区，按实际设备覆盖变量，例如：

```bash
make floppy DISK=/dev/sdb
```

该例对应`/dev/sdb3`，其他分区命名方式需要相应调整写盘规则。板上启动双核时，在课程固件提示符下输入`loadbootm`。串口工具可用`make minicom`，默认设备为`/dev/ttyUSB1`。

## Shell命令

| 命令 | 用途 |
|---|---|
| `ps` | 显示进程PID、状态、CPU掩码、实际运行核和名称；非运行进程的核号为`-1` |
| `clear` | 清屏并重新显示Shell区域标题 |
| `exec name [args...]` | 创建进程并等待它退出 |
| `exec name [args...] &` | 后台创建进程，立即返回提示符 |
| `kill pid` | 结束指定进程 |
| `taskset mask name` | 创建进程并设置允许核，立即返回提示符 |
| `taskset -p mask pid` | 修改已有进程的允许核 |

`mask`按十六进制解析：`0x1`仅CPU0、`0x2`仅CPU1、`0x3`允许两个核。普通`exec`创建的子进程继承父进程掩码。另一核正在运行的进程在下一次定时器中断时根据新掩码重新调度。

只有`exec`支持末尾`&`。Shell按空白拆分参数，不支持引号、管道或重定向；程序名作为`argv[0]`，`exec`和末尾`&`不传入子进程。`exit/waitpid/getpid`提供系统调用接口，当前没有同名Shell内建命令。

屏幕上方第0～19行为任务输出，下方第20～49行为Shell区域。

## 功能测试

以下是测试方法，不表示这些用例已全部通过。建议分组运行，结束一组后清理相关进程或重新启动，避免输出区域、固定共享地址和同步对象key相互干扰。下列`PID`均需替换为当次`ps`看到的真实数字；杀死父进程不会自动结束其全部子进程。

### Task1：进程管理

```text
ps
clear
abc
exec waitpid 3 &
ps
```

`abc`应输出`unknown command`。`waitpid`会创建`ready_to_exit`和`wait_locks`；参数`3`指定输出起始行，不是PID。观察等待者阻塞、子进程退出后等待者恢复，以及结束后进程表的变化。还可在`ready_to_exit`持有两把锁期间执行`kill PID`，检查等锁进程是否继续运行。测试程序正常退出前会显式释放锁，因此仅观察自然退出不足以验证持锁退出清理。

### Task2：同步与通信

```text
exec barrier 0 &
```

三个子进程进行10轮屏障同步，轮次为0～9；每轮必须等三者到齐后才能通过。

```text
exec condition 0 &
```

普通模式启动一个生产者和三个消费者。生产者共生产30件，库存耗尽后消费者累计消费应合计30件。消费者使用无限循环，生产结束后会阻塞等待，因此父测试任务不会自然结束。

```text
exec mbox_server 0 &
exec mbox_client &
exec mbox_client &
```

先启动一个服务器，再启动多个客户端。观察收发持续进行、`correctBytes`增长且`errorBytes`为0；`blocked`表示正常的阻塞次数。服务器和客户端都不会自然结束。清理全部参与者后可重新运行，检查邮箱引用回收和再次打开。

### Task3：双核计算

```text
exec multicore
```

测试先由一个`add`进程完成计算，再由两个`add`进程分别计算两个区间并合并结果。检查两种结果一致，并比较`single core`与`multi core`耗时。

### Task4：绑核与迁移

```text
taskset 0x1 affinity
ps
taskset -p 0x2 PID
ps
```

先观察`affinity`及其五个子任务继承`0x1`，再选择一个尚未退出的`test_affinity`子进程迁移到CPU1。检查其掩码变为`0x2`，被调度运行时实际核号为1，以及各实例的`auipc`输出相同。子任务完成150轮计算后退出，迁移操作需要在其存活期间执行。

## Task5性能对比

### 测试模式

```text
exec condition 0 perf &
```

`0`是输出起始行，`perf`选择吞吐量测试。此模式启动一个producer和一个consumer，每次只生产/消费一件，不执行普通模式的随机睡眠和逐次打印。库存上限为1024；满或空时通过条件变量等待。consumer按实际经过的ticks换算items/s，约每秒输出一次速率。

先记录不运行飞机时的多组稳定速率，再启动：

```text
exec fly &
```

确认飞机持续移动后，记录有飞机时的多组稳定速率。保持测试程序、双核配置、统计方式及其他负载一致，分别对大锁和细粒度配置测试。指导书要求细粒度模式加入飞机后，速率不低于原来的50%。

### 保存两种镜像

先将`FINE_GRAINED_LOCK`设为`0`，构建并保存大锁镜像；再设为`1`，重新构建并保存细粒度镜像。每次切换后执行`make clean`，避免头文件变更未触发重编译：

```bash
# FINE_GRAINED_LOCK设为0后
make clean
make all
mkdir -p images/coarse
cp build/image build/kernel_image build/user_image images/coarse/

# FINE_GRAINED_LOCK设为1后
make clean
make all
mkdir -p images/fine
cp build/image build/kernel_image build/user_image images/fine/
```

`make run-smp`和`make floppy`默认使用当前`build/image`，保存副本并不会自动切换运行镜像。应在对应配置构建后分别运行和记录结果；两组测试之间重新启动，避免残留进程。

### 已有运行反馈

以下为开发者手工运行后反馈的数据，未由自动化测试复现；具体数值取决于平台、终端输出和调度情况。

| 测试 | 无fly | 有fly |
|---|---:|---:|
| 大内核锁 | 约100items/s | 约18items/s |
| 细粒度锁 | 约600～800items/s | 约600～800items/s |

这组反馈中，细粒度模式加入飞机前后的速率范围重叠，未观察到明显下降，满足该次测试的50%保留率要求；它不能代替全部双核功能回归。

另一次`multicore`反馈为单任务65272389ticks、双任务33989765ticks，两种结果均为630，加速比约1.92。该记录不是本次README更新时重新运行得到的结果。

## 调试与验证范围

使用`make debug-smp`启动等待GDB连接的双核QEMU，再在另一终端执行`make gdb`。可用`info threads`查看两个核，重点检查各核`tp`、PCB的`status/cpu_mask/cpu_id/kill_pending`、就绪队列及锁状态。

当前页分配器支持空闲单页复用，多页分配仍采用地址递增方式，尚未提供完整的物理内存耗尽检测和虚拟地址空间隔离。双核Barrier、Condition、Mailbox、跨核kill、持锁退出和反复创建/回收仍需结合实际运行进行完整回归；本文不将源码实现或单次性能反馈等同于全部测试通过。

本说明按当前Project3分支源码更新。课程要求见仓库外的`../p3/guide_book_p3.pdf`，设计讨论见`docs/P3_Design_Review.md`；设计文档中的方案应以当前源码为准。
