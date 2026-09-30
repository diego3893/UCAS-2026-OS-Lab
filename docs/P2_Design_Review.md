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

### 7.1 用户态、内核态和例外

Task3开始后，应用运行在User Mode，内核运行在Supervisor Mode。用户程序不能再通过跳转表直接调用`do_scheduler`、`screen_write`等内核函数，而要执行`ecall`主动触发系统调用例外。CPU随后切换到Supervisor Mode，并跳转到内核设置的例外入口。

RISC-V把异常和中断统称为Trap。系统调用属于由当前指令主动触发的同步异常；定时器中断则是由硬件异步触发的中断。Task3主要实现系统调用异常，Task4再打开定时器中断。

### 7.2 系统调用的完整流程

系统调用计划按照以下数据通路执行：

```text
用户程序调用sys_write等API
→invoke_syscall把参数放入a0～a4，把系统调用号放入a7
→执行ecall
→CPU将当前PC保存到sepc，并在scause中记录例外原因
→CPU切换到Supervisor Mode，并跳转到stvec指向的exception_handler_entry
→SAVE_CONTEXT在当前进程的内核栈中保存用户态现场
→interrupt_helper根据scause选择异常处理函数
→handle_syscall根据a7查找并调用具体内核函数
→将返回值写入保存现场中的a0，并令sepc=sepc+4
→RESTORE_CONTEXT恢复用户态现场
→sret返回User Mode，从ecall的下一条指令继续执行
```

`sepc`必须加4，因为它最初指向触发例外的`ecall`。如果直接返回原地址，程序会再次执行同一条`ecall`并反复陷入内核。

### 7.3 相关寄存器

系统调用和例外入口涉及的通用寄存器如下：

| 寄存器 | 主要用途 | 本实验中的作用 |
|---|---|---|
| `zero` | 恒为0 | 保存现场时对应`regs[0]`，实际值始终为0 |
| `ra` | 普通函数返回地址 | 用户态`ra`需要保存和恢复；进入内核后另行把`ra`设为`ret_from_exception`，供内核C函数返回 |
| `sp` | 当前栈指针 | Trap发生时由用户栈切换到当前进程的内核栈，返回前再恢复用户栈 |
| `gp` | 全局数据指针 | 属于用户现场，需要原样保存和恢复 |
| `tp` | 线程指针 | 本实验固定保存`current_running`，即当前PCB地址 |
| `t0`～`t6` | 临时寄存器 | 可能被内核代码覆盖，因此Trap入口需要全部保存 |
| `s0`～`s11` | 被调用者保存寄存器 | Trap现场和`switch_to`现场都需要保存 |
| `a0`～`a4` | 函数参数 | 保存系统调用的5个参数，其中`a0`还保存返回值 |
| `a5`～`a6` | 其他参数寄存器 | 本实验未用于传递系统调用参数，但仍属于用户现场 |
| `a7` | 第8个参数寄存器 | 本实验用于保存系统调用号 |

例外处理涉及的主要CSR如下：

| CSR或状态位 | 作用 |
|---|---|
| `stvec` | 保存Supervisor Mode的Trap总入口地址，本实验指向`exception_handler_entry` |
| `scause` | 记录Trap类型和原因；最高位区分中断与异常，U态`ecall`的异常编号为8 |
| `sepc` | 保存被Trap打断的指令地址，也是`sret`返回时使用的PC |
| `stval` | 保存出错地址等附加信息；框架中的旧名称为`sbadaddr` |
| `sscratch` | 为Trap入口提供临时存储，可辅助交换用户栈和内核栈；本设计主要通过`tp`和PCB完成栈切换 |
| `sstatus.SPP` | 记录Trap前的特权级；设为0时`sret`返回User Mode |
| `sstatus.SIE` | Supervisor Mode的全局中断开关 |
| `sstatus.SPIE` | 保存Trap前的中断使能状态，供`sret`恢复 |
| `sstatus.SUM` | 控制Supervisor Mode能否访问用户页面 |
| `sstatus.FS` | 记录浮点单元状态；入口代码关闭它以避免内核误用未保存的浮点现场 |
| `sie.STIE` | Supervisor Timer Interrupt的分类开关，Task4使用 |

其中`sie.STIE`类似定时器中断的分开关，`sstatus.SIE`是Supervisor Mode的中断总开关。`ecall`是同步异常，不依赖这两个中断开关。

### 7.4 两类上下文和两套栈

每个进程同时拥有用户栈和内核栈。应用在User Mode下使用用户栈，发生Trap后切换到该进程自己的内核栈，以免不同进程的内核调用过程互相覆盖。

本实验存在两类上下文：

| 上下文 | 保存内容 | 保存时机 |
|---|---|---|
| `regs_context_t` | 全部通用寄存器以及`sstatus`、`sepc`、`stval`、`scause` | 进程由User Mode进入Supervisor Mode时 |
| `switchto_context_t` | `ra`、`sp`和`s0`～`s11` | 内核调用`switch_to`切换进程时 |

例如进程A执行`sys_yield`后，内核先保存A的用户态`regs_context_t`，再由`switch_to`保存A的内核态`switchto_context_t`；随后恢复进程B的内核态上下文，最后由`ret_from_exception`恢复B的用户态上下文并执行`sret`。因此，一次完整的进程切换需要经过两层上下文保存和恢复。

新进程从未真正运行过，所以`init_pcb_stack`需要预先构造两层假现场：`switchto_context_t.ra`指向`ret_from_exception`，`regs_context_t.sepc`指向应用入口，用户`sp`指向用户栈顶，`tp`指向该进程PCB，同时保持`SPP=0`、设置`SPIE=1`。第一次被调度时，进程便会按照“恢复内核上下文→恢复用户上下文→`sret`”的统一路径进入User Mode。

### 7.5 例外和系统调用的分发

本实验采用两级分发：

```text
scause
→interrupt_helper
→异常查exc_table，中断查irq_table
→exc_table[8]=handle_syscall
→handle_syscall读取a7
→syscall[a7]
→具体内核服务
```

这种表驱动设计将“Trap种类”和“具体系统调用号”分开，便于以后继续增加异常处理函数或系统调用。各文件的主要职责如下：

| 文件 | 主要职责 |
|---|---|
| `init/main.c` | 构造进程初始现场并初始化`syscall[]` |
| `arch/riscv/kernel/trap.S` | 将`exception_handler_entry`写入`stvec` |
| `arch/riscv/kernel/entry.S` | 保存和恢复现场，完成Trap入口与`sret`返回 |
| `kernel/irq/irq.c` | 根据`scause`分发异常或中断 |
| `kernel/syscall/syscall.c` | 根据`a7`调用具体系统调用并处理返回值 |
| `tiny_libc/syscall.c` | 用户侧封装参数并执行`ecall` |
| `kernel/sched/sched.c`和`time.c` | 实现睡眠阻塞及到时唤醒 |

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

## 12 Design Review问题详细回答

### 12.1 Syscall的执行流程与参数传递

Task1和Task2中，用户程序通过跳转表直接调用内核函数。当时用户程序和内核都运行在Supervisor Mode，还没有真正的特权级隔离。Task3开始后，用户程序运行在User Mode，内核运行在Supervisor Mode，用户程序需要通过`ecall`进入内核。

以`sys_write(buff)`为例，完整执行流程如下：

```text
用户程序调用sys_write
→tiny_libc中的sys_write调用invoke_syscall
→a0保存buff地址，a7保存SYSCALL_WRITE
→执行ecall
→CPU产生来自User Mode的系统调用异常
→scause=8，sepc保存ecall指令地址
→CPU切换到Supervisor Mode
→PC跳转到stvec中的exception_handler_entry
→SAVE_CONTEXT保存用户态现场
→interrupt_helper根据scause判断异常类型
→exc_table[8]找到handle_syscall
→handle_syscall从a7取得系统调用号
→通过syscall[a7]调用screen_write
→将返回值写入保存现场中的a0
→sepc加4
→RESTORE_CONTEXT恢复用户态现场
→执行sret，返回User Mode
```

系统调用采用以下参数约定：

| 寄存器 | 用途 |
|---|---|
| `a0` | 第1个参数，同时用于保存返回值 |
| `a1` | 第2个参数 |
| `a2` | 第3个参数 |
| `a3` | 第4个参数 |
| `a4` | 第5个参数 |
| `a7` | 系统调用号 |

例如`sys_move_cursor(10,20)`执行`ecall`前，`a0=10`、`a1=20`、`a7=SYSCALL_CURSOR`。用户侧`invoke_syscall`通过固定寄存器变量装载参数，内核侧再从`regs_context_t`中读取`a0～a4`和`a7`。

系统调用完成后，返回值写入保存现场中的`a0`。同时必须令`sepc=sepc+4`，因为`sepc`原本指向触发异常的`ecall`；如果不加4，返回后会再次执行同一条`ecall`。

### 12.2 初始化异常和中断处理时设置的寄存器

Task3首先设置：

```text
stvec=exception_handler_entry
```

`stvec`规定发生交给Supervisor Mode处理的Trap后，CPU从哪里开始执行。本实验使用Direct模式，所有异常和中断首先进入同一个`exception_handler_entry`，再由软件读取`scause`进行分发。

系统调用`ecall`属于同步异常，不受中断开关控制，所以Task3不需要为系统调用打开定时器中断。新进程的初始`sstatus`设置为`SR_SPIE`，使`SPP=0`、`SPIE=1`：`SPP=0`表示`sret`返回User Mode，`SPIE=1`用于恢复中断使能状态。

Task4实现定时器中断时还需要以下设置：

| 寄存器或状态位 | 初始化值 | 作用 |
|---|---:|---|
| `stvec` | `exception_handler_entry` | 设置Trap总入口 |
| `sstatus.SIE` | 1 | 打开Supervisor Mode全局中断 |
| `sie.STIE` | 1 | 允许Supervisor Timer Interrupt |
| Timer比较值 | `get_ticks()+TIMER_INTERVAL` | 设置第一次定时器中断时间 |

其中`sstatus.SIE`相当于中断总开关，`sie.STIE`相当于定时器中断的分开关。

### 12.3 进程因异常进入内核时保存的上下文

异常可能发生在用户程序的任意指令处，因此`SAVE_CONTEXT`需要保存完整用户态现场，包括：

- `ra`、`sp`、`gp`和`tp`；
- `t0～t6`临时寄存器；
- `s0～s11`被调用者保存寄存器；
- `a0～a7`参数和返回值寄存器；
- `sstatus`、`sepc`、`stval`和`scause`。

刚发生Trap时，`sp`仍指向用户栈，`tp`指向当前PCB。入口汇编先将用户`sp`保存到PCB，再读取当前进程的`kernel_sp`，切换到内核栈并在内核栈上建立`regs_context_t`。

因此，保存的内容属于用户态，但保存位置是当前进程的内核栈。用户栈原有内容仍然保留在内存中，只需要保存它的栈指针，不需要整体复制用户栈。

如果异常处理中又调用`do_scheduler`发生进程切换，还要通过`switch_to`保存一次内核态上下文：

| 上下文 | 保存时机 | 保存内容 |
|---|---|---|
| `regs_context_t` | User Mode进入Supervisor Mode | 全部用户寄存器和关键CSR |
| `switchto_context_t` | 内核执行`switch_to` | `ra`、`sp`和`s0～s11` |

完整过程为：

```text
保存进程A的用户态上下文
→保存进程A的内核态上下文
→恢复进程B的内核态上下文
→恢复进程B的用户态上下文
→sret返回进程B
```

### 12.4 Task3中`init_pcb_stack`的初始化操作

新进程从未运行过，没有真实的寄存器现场。为了复用统一的恢复流程，`init_pcb_stack`需要在内核栈上构造两层假现场：

```text
内核栈高地址
┌────────────────────────┐
│ regs_context_t         │ ← 用户态假现场
├────────────────────────┤
│ switchto_context_t     │ ← 内核态假现场
└────────────────────────┘
内核栈低地址
```

首先清零两层现场，避免未初始化寄存器中存在随机值。随后设置：

| 初始化内容 | 设置值 | 作用 |
|---|---|---|
| 用户`sp` | `user_stack` | 进入用户程序后使用用户栈 |
| 用户`tp` | 当前PCB地址 | 发生Trap时能够找到当前PCB |
| `sepc` | 用户程序入口 | 第一次`sret`后从应用入口执行 |
| `sstatus.SPP` | 0 | `sret`后进入User Mode |
| `sstatus.SPIE` | 1 | 返回后恢复中断使能状态 |
| 内核假现场`ra` | `ret_from_exception` | `switch_to`恢复后进入用户现场恢复流程 |
| 内核假现场`sp` | `pt_switchto` | `switch_to`加上现场大小后恰好指向`pt_regs` |
| `pcb->kernel_sp` | `pt_switchto` | 第一次调度时找到内核假现场 |
| `pcb->user_sp` | `user_stack` | 记录用户栈位置 |

第一次调度时的执行过程为：

```text
switch_to恢复内核假现场
→ra指向ret_from_exception
→sp指向用户态假现场
→RESTORE_CONTEXT恢复用户现场
→sret
→从sepc记录的用户程序入口开始执行
```

### 12.5 Task3和Task4唤醒睡眠进程的时机

进程调用`sys_sleep(sleep_time)`后，内核设置：

```c
current_running->wakeup_time = get_timer()+sleep_time;
```

然后将当前PCB阻塞到`sleep_queue`。`check_sleeping`遍历睡眠队列，把满足`wakeup_time<=current_time`的PCB解除阻塞并放回`ready_queue`。

Task3没有定时器中断，采用非抢占式调度。只有其他进程主动调用`sys_yield`、`sys_sleep`或因锁而阻塞，进入`do_scheduler`时，才会调用`check_sleeping`检查睡眠队列。如果就绪队列为空，调度器会持续检查时间，直到有睡眠进程到期。

Task4加入周期性定时器中断。用户进程即使不主动调用`sys_yield`，定时器也会强制进入`handle_irq_timer`并调用`do_scheduler`，从而周期性检查睡眠队列。

| 任务 | 调度触发方式 | 检查睡眠队列的时机 |
|---|---|---|
| Task3 | 用户进程主动`yield`、`sleep`或阻塞 | 每次主动进入调度器时 |
| Task4 | 硬件定时器强制中断 | 每个时间片结束时 |
