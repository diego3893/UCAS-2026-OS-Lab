# 操作系统研讨课Project 2 Design Review

## 1 实验目标

### 1.1 总体目标

Project 1中，内核可以从镜像中加载用户程序，并通过函数指针跳转到程序入口。但是，这种方式本质上仍然是内核调用一个普通函数，系统中没有独立的进程状态，也不能在多个程序之间保存现场并切换。

Project 2计划在Project 1的启动和加载功能之上，实现一个具有基本进程管理能力的内核，主要包括以下内容：

1. 设计进程控制块PCB，为每个进程保存运行状态、栈指针、进程号和队列节点等信息。
2. 为不同进程分配独立的用户栈和内核栈，并初始化进程第一次运行所需的栈帧。
3. 实现`switch_to`，保存当前进程的内核态上下文并恢复下一个进程的内核态上下文。
4. 实现非抢占式轮转调度，使多个进程可以通过`sys_yield`主动交出CPU。
5. 实现进程的阻塞与唤醒，以及基于阻塞队列的互斥锁。
6. 实现RISC-V异常入口、上下文保存与恢复和基于`ecall`的系统调用。
7. 实现定时器中断，在用户程序不主动调用`sys_yield`时也能进行抢占式调度。
8. 在C-core要求下实现动态调度算法，使运行速度不同的`fly`程序保持接近的显示速度。

### 1.2 P1和P2运行方式的区别

| Project 1 | Project 2 |
|---|---|
| App由Kernel通过函数指针直接调用 | App被组织为独立进程 |
| 不保存独立的进程现场 | 每个进程拥有PCB和寄存器上下文 |
| 一个App运行完成后返回Kernel | 进程可以暂停，之后从暂停位置继续运行 |
| 没有进程状态和调度队列 | 具有RUNNING、READY、BLOCKED等状态 |
| 通过跳转表直接使用内核功能 | Task 3后通过`ecall`进入内核 |
| 没有定时器调度 | 通过定时器中断进行抢占式调度 |

## 2 P2代码框架

| 文件或目录 | 主要作用 | 对应任务 |
|---|---|---|
| `init/main.c` | 初始化App Info、PCB、锁、异常、系统调用和屏幕 | Task 1至Task 4 |
| `include/os/sched.h` | 定义PCB、进程状态和两类上下文 | Task 1至Task 4 |
| `kernel/sched/sched.c` | 保存PCB数组，实现调度、阻塞和睡眠 | Task 1至Task 4 |
| `include/os/list.h` | 定义双向链表节点和队列头 | Task 1、Task 2 |
| `arch/riscv/kernel/entry.S` | 实现`switch_to`和异常现场保存、恢复 | Task 1、Task 3 |
| `arch/riscv/kernel/trap.S` | 设置`stvec`并打开中断 | Task 3、Task 4 |
| `kernel/locking/lock.c` | 实现自旋锁和互斥锁 | Task 2 |
| `kernel/irq/irq.c` | 分发异常和中断，处理系统调用与定时器 | Task 3、Task 4 |
| `kernel/syscall/syscall.c` | 根据系统调用号调用内核服务 | Task 3 |
| `tiny_libc/syscall.c` | 在用户态封装系统调用API | Task 1至Task 3 |
| `kernel/sched/time.c` | 读取时间，管理睡眠进程 | Task 3、Task 4 |
| `include/os/mm.h`、`kernel/mm/mm.c` | 为用户栈和内核栈分配页面 | Task 1以后 |
| `drivers/screen.c` | 在内存中维护模拟屏幕并刷新到串口 | Task 1以后 |
| `arch/riscv/include/asm/regs.h` | 定义寄存器在栈帧中的偏移 | Task 1、Task 3 |

## 3 PCB和进程资源设计

### 3.1 PCB需要保存的内容

进程是一个抽象概念，内核需要通过PCB记录进程的实际状态。框架中的`pcb_t`包含：

```c
typedef struct pcb
{
    reg_t kernel_sp;
    reg_t user_sp;
    list_node_t list;
    pid_t pid;
    task_status_t status;
    int cursor_x;
    int cursor_y;
    uint64_t wakeup_time;
} pcb_t;
```

各字段的用途如下：

| 字段 | 用途 |
|---|---|
| `kernel_sp` | 保存该进程内核栈当前的栈指针，是`switch_to`切换的核心 |
| `user_sp` | 保存该进程在用户态运行时的栈指针 |
| `list` | 将PCB挂入就绪队列、睡眠队列或互斥锁阻塞队列 |
| `pid` | 唯一标识一个进程 |
| `status` | 表示进程当前是READY、RUNNING、BLOCKED还是EXITED |
| `cursor_x`、`cursor_y` | 保存每个进程独立的屏幕光标位置 |
| `wakeup_time` | 保存睡眠进程应该被唤醒的时刻 |

当前框架最多保存16个普通进程：

```c
#define NUM_MAX_TASK 16
pcb_t pcb[NUM_MAX_TASK];
```

此外，`pid0_pcb`表示最开始运行内核`main`函数的0号进程。普通用户进程尚未运行时，`current_running`应先指向`pid0_pcb`。

### 3.2 每个进程需要的栈

本实验采用多内核栈设计。每个用户进程拥有：

```text
一个用户栈：用户程序在User Mode下执行时使用
一个内核栈：系统调用、中断和内核调度期间使用
```

框架定义一页为4096字节：

```c
#define PAGE_SIZE 4096
```

计划通过`allocKernelPage(1)`和`allocUserPage(1)`分别为每个进程分配一页内核栈和用户栈。栈从高地址向低地址增长，所以初始化时应将分配区域的高地址作为栈顶。

多内核栈会比所有进程共用一个内核栈多占用内存，但在进程进入内核后被切换出去时，其内核调用过程和上下文仍可以保留在自己的内核栈上，设计更清晰，也便于后续扩展。

### 3.3 PCB初始化流程

`init_pcb`计划完成以下步骤：

1. 令`current_running`指向`pid0_pcb`，并将0号进程状态设为`TASK_RUNNING`。
2. 初始化`ready_queue`和每个PCB中的链表节点。
3. 通过Project 1保留的`tasks[]`和`load_task_img`加载`print1`、`print2`、`fly`等测试程序，取得各自入口地址。
4. 为每个用户进程分配用户栈和内核栈。
5. 为进程分配PID，初始状态设为`TASK_READY`。
6. 调用`init_pcb_stack`制作进程第一次运行所需的初始上下文。
7. 将初始化完成的PCB加入`ready_queue`。

Project 1中的`tasks[]`保存App在镜像中的名称、偏移、大小和入口地址；Project 2中的`pcb[]`保存App被组织成进程后的运行状态。二者作用不同：前者用于把程序装入内存，后者用于管理已经创建的进程。

## 4 上下文和`switch_to`

### 4.1 两类上下文

框架定义了两种上下文：

```text
switchto_context_t：内核主动调用switch_to时保存
regs_context_t：发生异常或中断、进入内核时保存
```

`switchto_context_t`只需要保存RISC-V函数调用约定中的callee-saved寄存器：

```text
ra、sp、s0、s1、s2～s11
```

一共14个64位寄存器，占用：

```text
14×8=112字节
```

`regs_context_t`用于异常现场，需要保存全部32个通用寄存器以及：

```text
sstatus
sepc
sbadaddr/stval
scause
```

它保存的信息更多，是因为异常可能在任意一条用户指令处发生，不能假设调用者已经按照普通函数调用约定保存了临时寄存器。

### 4.2 `switch_to`的工作机制

假设当前进程为A，调度器准备切换到进程B，调用：

```c
switch_to(A, B);
```

按照RISC-V调用约定：

```text
a0=A的PCB地址
a1=B的PCB地址
```

`switch_to`计划执行：

```text
1. 在A的内核栈上预留switchto_context_t空间
2. 保存A的ra、sp和s0～s11
3. 将当前sp写入A->kernel_sp
4. 从B->kernel_sp读取B的内核栈指针，并写入sp
5. 从B的内核栈恢复ra、sp和s0～s11
6. 释放B栈上的switchto_context_t空间
7. 执行ret
```

最关键的操作是切换`sp`。`sp`切换后，后续恢复动作读取的已经是B的栈，因此恢复的是B上一次保存的寄存器。

调用`switch_to`的是进程A，但函数执行`ret`时，使用的是从B栈中恢复的`ra`，所以可能返回到B上一次调用`switch_to`之后的位置。这就是一个函数调用实现两个进程上下文切换的原因。

### 4.3 新进程第一次如何运行

一个已经运行过的进程，其内核栈上有真正由`switch_to`保存的上下文；新进程从未运行过，没有可以恢复的现场。

因此，`init_pcb_stack`需要提前制作一个“假的现场”，使`switch_to`认为新进程以前被切换出去过。

Task 1仍运行在同一特权级时，计划在`switchto_context_t`中设置：

```text
ra=用户程序入口地址
sp=用户栈顶
```

其他callee-saved寄存器清零，并让`pcb->kernel_sp`指向这份伪造上下文。第一次切换到该进程时，`switch_to`恢复这些值，最后的`ret`就会跳到用户程序入口。

Task 3引入User Mode后，进程不能再通过`switch_to`直接返回到用户入口，而应当：

```text
switch_to恢复内核上下文
→ra指向ret_from_exception
→RESTORE_CONTEXT恢复用户上下文
→sret根据sepc进入用户程序
```

所以Task 3还需要在内核栈上准备`regs_context_t`，主要初始化：

- 用户态`sp`；
- `sepc`为用户程序入口；
- `sstatus.SPP=0`，表示`sret`返回User Mode；
- `sstatus.SPIE=1`，使`sret`返回后恢复中断使能；
- `switchto_context_t.ra=ret_from_exception`。

## 5 非抢占式调度设计

### 5.1 就绪队列

本实验计划采用双向循环链表维护就绪队列。`ready_queue`本身是哨兵节点，不对应真实进程；每个PCB通过`list`字段加入队列。

Task 1采用Round Robin轮转调度：

```text
ready_queue：A→B→C

A运行并调用sys_yield
→A重新放到队尾
→选择B运行

B调用sys_yield
→B重新放到队尾
→选择C运行
```

### 5.2 `do_scheduler`工作流程

`do_scheduler`计划执行以下步骤：

1. 保存原来的`current_running`为`prev`。
2. 如果`prev`是普通进程并且状态仍为`TASK_RUNNING`，将其改为`TASK_READY`并放回就绪队列队尾。
3. 从`ready_queue`队头取出一个PCB作为`next`。
4. 将`next`从就绪队列移除，并把状态设为`TASK_RUNNING`。
5. 更新`current_running=next`。由于`current_running`映射到`tp`寄存器，这一步同时更新`tp`。
6. 调用`switch_to(prev,next)`完成内核栈和callee-saved寄存器切换。

`sys_yield`在Task 1、Task 2中仍通过跳转表调用`do_scheduler`，因此它属于进程主动让出CPU的非抢占式调度。如果一个程序始终不调用`sys_yield`，此时的内核不能强制停止它；这个问题将在Task 4通过定时器中断解决。

## 6 进程阻塞和互斥锁

### 6.1 阻塞和唤醒

当进程等待锁或睡眠时，它不应继续占用CPU，也不应保留在就绪队列中。

`do_block`计划完成：

```text
将current_running状态设为TASK_BLOCKED
→把它的list节点加入指定阻塞队列
→调用do_scheduler切换到其他READY进程
```

`do_unblock`计划完成：

```text
从原阻塞队列移除PCB
→将状态设为TASK_READY
→加入ready_queue队尾
```

进程调用`do_block`后，虽然C函数调用尚未返回，但CPU已经切换到其他进程。当该进程以后被唤醒并重新调度时，才会从原来`do_block`内部的`do_scheduler`调用处继续运行。

### 6.2 互斥锁结构

框架中的互斥锁包含：

```c
typedef struct mutex_lock
{
    spin_lock_t lock;
    list_head block_queue;
    int key;
} mutex_lock_t;
```

其中：

- `lock.status`记录锁是`LOCKED`还是`UNLOCKED`；
- `block_queue`保存申请失败的进程；
- `key`用于将用户给出的键映射到内核中的锁。

`do_mutex_lock_init(key)`应保证在同一生命周期内，相同`key`得到相同的handle。新的锁需要初始化为未占用状态，并建立空阻塞队列。

### 6.3 获取和释放锁

申请互斥锁时：

```text
锁空闲
→置为LOCKED
→当前进程继续执行

锁已占用
→当前进程加入该锁的block_queue
→状态变为TASK_BLOCKED
→调度其他进程
```

释放互斥锁时：

```text
阻塞队列为空
→将锁设为UNLOCKED

阻塞队列非空
→唤醒队头等待进程
→将其放入ready_queue
→使该进程之后能够继续申请或获得锁
```

与自旋锁不断循环检查相比，互斥锁会让等待进程进入BLOCKED状态，可以避免在单核环境中浪费CPU时间。

## 7 系统调用和异常处理

### 7.1 系统调用执行流程

Task 3以后，用户程序运行在User Mode，不能再通过跳转表直接调用内核函数。计划采用以下流程：

```text
用户程序调用sys_write等API
→tiny_libc中的invoke_syscall
→把系统调用号和参数放入寄存器
→执行ecall
→硬件切换到Supervisor Mode
→跳转到stvec保存的exception_handler_entry
→SAVE_CONTEXT保存用户态现场
→interrupt_helper根据scause分发异常
→handle_syscall调用对应内核函数
→RESTORE_CONTEXT恢复用户态现场
→sret返回用户程序
```

### 7.2 参数和返回值传递

计划沿用RISC-V调用习惯：

| 寄存器 | 含义 |
|---|---|
| `a0`至`a4` | 最多5个系统调用参数 |
| `a7` | 系统调用号 |
| `a0` | 系统调用返回值 |

`invoke_syscall`使用内联汇编把参数放入对应寄存器，再执行`ecall`。异常入口把全部通用寄存器保存到`regs_context_t`后，`handle_syscall`可以从：

```text
regs[10]～regs[14]读取a0～a4
regs[17]读取a7
```

之后调用：

```c
syscall[sysno](arg0, arg1, arg2, arg3, arg4);
```

返回值写回`regs[10]`，这样恢复现场后，用户程序可以从`a0`取得返回结果。

系统调用处理完还需要执行：

```c
regs->sepc += 4;
```

因为`sepc`指向触发异常的`ecall`指令。如果直接返回原`sepc`，用户程序会再次执行同一条`ecall`并不断陷入内核。

### 7.3 异常入口初始化

`setup_exception`首先需要把异常入口写入：

```text
stvec=exception_handler_entry
```

本实验采用Direct模式，因此`stvec`最低两位应为0。Task 4还需要：

- 设置`sie.STIE=1`，允许Supervisor Timer Interrupt；
- 设置`sstatus.SIE=1`，打开Supervisor Mode全局中断；
- 设置下一次定时器触发时间。

这里需要区分`sie`和`sstatus.SIE`：前者决定某一种中断是否允许，后者是Supervisor Mode下的全局开关，两者都满足时定时器中断才能正常进入内核。

### 7.4 异常发生时保存的内容

异常可能在任意指令处发生，因此`SAVE_CONTEXT`需要保存：

1. 全部32个通用寄存器；
2. `sstatus`，用于恢复异常前的特权级和中断状态；
3. `sepc`，用于恢复异常前的执行位置；
4. `stval`，用于记录出错地址等附加信息；
5. `scause`，用于判断异常或中断原因。

保存时还要特别处理`sp`和`tp`：

- `sp`需要从用户栈切换到当前进程的内核栈，同时原用户栈指针不能丢失；
- `tp`表示`current_running`，异常处理期间仍需要通过它找到当前PCB。

`RESTORE_CONTEXT`按照相反顺序恢复寄存器，最后由`sret`根据`sstatus.SPP`选择返回特权级，并跳转到`sepc`。

## 8 睡眠和定时器中断

### 8.1 睡眠队列

`do_sleep(time)`计划完成：

```text
current_running->wakeup_time=get_timer()+time
→将当前进程阻塞到sleep_queue
→调用do_scheduler
```

`check_sleeping`遍历`sleep_queue`，将满足：

```text
current_time>=pcb->wakeup_time
```

的进程解除阻塞并放回`ready_queue`。

Task 3还没有定时器中断，只能在进程主动调用`do_scheduler`时检查睡眠队列；Task 4已经有周期性定时器中断，应在定时器处理流程中检查睡眠队列。这样即使没有其他进程主动`yield`，睡眠进程也能按时间被唤醒。

### 8.2 定时器中断处理

`handle_irq_timer`计划执行：

```text
重新设置下一次timer
→刷新模拟屏幕
→检查并唤醒到期的睡眠进程
→调用do_scheduler
```

定时器属于一次性预约，处理当前中断时必须设置下一次触发时间，例如：

```text
next_tick=get_ticks()+TIMER_INTERVAL
```

否则系统可能只收到第一次定时器中断。

Task 4测试时将用户程序中的`sys_yield`全部注释掉。如果多个程序仍能轮流运行，说明CPU控制权已经从“用户程序主动交出”改为“内核通过定时器强制收回”，抢占式调度实现成功。

## 9 C-core复杂调度设计

Task 5中有5个`fly`程序，它们的`CYCLE_PER_MOVE`不同。相同时间片只能保证每个进程得到近似相同的CPU时间，不能保证飞机在屏幕上的移动速度相同。

计划增加`set_sche_workload`系统调用，使每个`fly`进程定期向内核报告当前位置。PCB中需要补充：

```text
初始位置
最近一次报告位置
最近一次报告时间
累计运行时间
调度权重或时间片
```

调度时不直接使用当前坐标比较，因为各飞机起点可能不同，而是使用：

```text
实际进度=当前位置-初始位置
```

再根据一段时间内的进度变化估计进程速度。若某个进程的实际进度落后于整体平均进度，则适当增加它的调度权重或时间片；若进度明显领先，则适当减小其权重。

所有进程的时间片需要设置正的下限，不能减为0。这样可以避免快飞机完全停止、等待慢飞机追赶的现象。权重还应设置上限并采用逐步调整，避免因为一次位置报告产生过大变化，导致调度不断振荡。

这个设计只依赖进程运行时报告的进度，不直接读取或写死`CYCLE_PER_MOVE`，因此助教改变飞机初始位置或循环速度后，调度器仍可以动态重新估计并调整。

## 10 计划测试方法

### 10.1 Task 1

- 同时运行`print1`、`print2`和`fly`；
- 检查`print1`、`print2`计数进度是否接近；
- 检查进程多次切换后能否从原位置继续运行；
- 使用GDB观察切换前后的`sp`、`ra`、`s0～s11`和`tp`。

### 10.2 Task 2

- 运行`lock1`和`lock2`；
- 检查同一时刻是否只有一个进程进入临界区；
- 检查申请失败的进程是否进入BLOCKED状态；
- 检查释放锁后等待进程是否回到READY状态。

### 10.3 Task 3

- 使用syscall版本测试程序；
- 检查`ecall`后能否进入异常入口；
- 检查系统调用参数和返回值是否正确；
- 检查`sleep`进程能否阻塞并被唤醒；
- 使用`loadbootd`验证用户程序不能直接访问内核地址。

### 10.4 Task 4

- 注释用户程序中的全部`sys_yield`；
- 检查多个进程是否仍能轮流运行；
- 检查定时器是否被持续重新设置；
- 检查睡眠进程是否在定时器中断中被唤醒。

### 10.5 Task 5

- 同时运行5个速度不同的`fly`程序；
- 检查所有飞机是否持续移动；
- 检查各飞机长期进度是否接近；
- 修改初始位置和`CYCLE_PER_MOVE`，验证调度算法没有依赖写死参数。

## 11 TODO

- [ ] Task 1：任务启动与非抢占式调度
  - [ ] 完成双向链表基本操作
  - [ ] 初始化PCB、用户栈和内核栈
  - [ ] 实现`switch_to`
  - [ ] 实现Round Robin调度器
  - [ ] 通过跳转表实现`sys_yield`、输出和光标操作
- [ ] Task 2：互斥锁
  - [ ] 实现`do_block`和`do_unblock`
  - [ ] 实现自旋锁基本操作
  - [ ] 实现互斥锁初始化、申请和释放
- [ ] Task 3：系统调用
  - [ ] 初始化异常入口和系统调用表
  - [ ] 实现`SAVE_CONTEXT`和`RESTORE_CONTEXT`
  - [ ] 实现`invoke_syscall`和`handle_syscall`
  - [ ] 实现`sleep`和睡眠队列
  - [ ] 使用`loadbootd`测试用户态和内核态隔离
- [ ] Task 4：定时器中断和抢占式调度
  - [ ] 使能Supervisor Timer Interrupt
  - [ ] 实现定时器中断处理
  - [ ] 在没有`sys_yield`时完成进程调度
- [ ] Task 5：复杂调度算法
  - [ ] 实现`set_sche_workload`系统调用
  - [ ] 记录并估计不同进程的运行进度
  - [ ] 动态调整调度权重或时间片
  - [ ] 保证不同速度的飞机持续、近似同步地移动
