/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * *
 *            Copyright (C) 2018 Institute of Computing Technology, CAS
 *               Author : Han Shukai (email : hanshukai@ict.ac.cn)
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * *
 *        Process scheduling related content, such as: scheduler, process blocking,
 *                 process wakeup, process creation, process kill, etc.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * *
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * */

#ifndef INCLUDE_SCHEDULER_H_
#define INCLUDE_SCHEDULER_H_

#include <type.h>
#include <os/list.h>

#define NUM_MAX_TASK 16
#define EXEC_MAX_ARGC 16
#define EXEC_MAX_ARG_BYTES 1024

/* used to save register infomation */
typedef struct regs_context
{
    /* Saved main processor registers.*/
    reg_t regs[32];

    /* Saved special registers. */
    reg_t sstatus;
    reg_t sepc;
    reg_t sbadaddr;
    reg_t scause;
} regs_context_t;

/* used to save register infomation in switch_to */
typedef struct switchto_context
{
    /* Callee saved registers.*/
    reg_t regs[14]; // reg[0]=ra, reg[1]=sp,后续为s0~s11
} switchto_context_t;

typedef enum {
    TASK_BLOCKED,
    TASK_RUNNING,
    TASK_READY,
    TASK_EXITED,
} task_status_t;

// 线程共享的地址空间
typedef struct address_space{
    uintptr_t pgdir; // 根页表
    pid_t tgid; // 进程编号
    int refs; // 引用此地址空间的PCB数，e.g.尚未join的已结束线程
    int live; // 尚未退出的线程数
    int dying; // 整个进程正在退出
    list_head waiters; // waitpid等待队列
}address_space_t;

/* Process Control Block */
typedef struct pcb
{
    /* register context */
    // NOTE: this order must be preserved, which is defined in regs.h!!
    reg_t kernel_sp;
    reg_t user_sp;
    ptr_t kernel_stack_base;
    ptr_t user_stack_base;

    /* previous, next pointer */
    list_node_t list;
    list_head wait_list;
    
    // /* pgdir */
    // uintptr_t pgdir;

    /* process id */
    pid_t pid;

    /* BLOCK | READY | RUNNING */
    task_status_t status;

    /* cursor position */
    int cursor_x;
    int cursor_y;

    /* time(seconds) to wake up sleeping PCB */
    uint64_t wakeup_time;

    int workload_valid; // 是否收到首次路程报告
    int initial_workload; // 将首次路程作为起始点
    int last_workload; // 上次报告的剩余路程

    uint64_t completed_rounds; // 已完成轮数
    uint64_t progress; // 累计前进步数

    uint32_t time_slice; // 每次获得几个时间片
    uint32_t ticks_left; // 还剩几个时间片

    int task_id; // 对应tasks数组中的程序编号
    int recycle_pending; // 回收标记，1为需要回收
    int kill_pending; // 核之间的退出标记

    int cpu_mask; // 允许在哪些CPU核上运行
    int cpu_id; // 当前运行的核号，没有运行时为-1

    uintptr_t pgdir; // 根页表的内核虚拟地址
    pid_t tgid; // 所属进程编号；原pid作为唯一线程编号
    address_space_t *space; // 地址空间
    uintptr_t user_stack_top; // 线程用户栈顶

    int is_thread; // 由pthread_create创建则为1
    int joined; // 已被join
    pid_t joiner; // 正在等待本线程的线程编号，0表示没有
    pid_t join_target; // 本线程正在join的目标，0表示没有
} pcb_t;

long do_thread_create(uintptr_t trampoline, uintptr_t start_routine, uintptr_t arg);
int do_thread_join(pid_t tid);
void do_thread_exit(void);

/* ready queue to run */
extern list_head ready_queue;

/* sleep queue to be blocked in */
extern list_head sleep_queue;

/* current running task PCB */
register pcb_t * current_running asm("tp");
extern pid_t process_id;

extern pcb_t pcb[NUM_MAX_TASK];
extern pcb_t pid0_pcb;
extern pcb_t secondary_idle_pcb;
extern const ptr_t pid0_stack;

extern void switch_to(pcb_t *prev, pcb_t *next);
void do_scheduler(void);
void do_sleep(uint32_t);

void do_block(list_node_t *, list_head *queue);
void do_unblock(list_node_t *);

long do_set_sche_workload(int workload);

void init_processes(int tasknum);
extern pid_t do_taskset(int mask, pid_t pid, char *name);

/************************************************************/
/* TODO [P3-TASK1] exec exit kill waitpid ps*/
#ifdef S_CORE
extern pid_t do_exec(int id, int argc, uint64_t arg0, uint64_t arg1, uint64_t arg2);
#else
extern pid_t do_exec(char *name, int argc, char *argv[]);
#endif
extern void do_exit(void);
extern int do_kill(pid_t pid);
extern int do_waitpid(pid_t pid);
extern void do_process_show();
extern pid_t do_getpid();
void reap_exited(void);
/************************************************************/

#endif
