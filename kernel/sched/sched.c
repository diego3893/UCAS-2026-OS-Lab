#include <os/list.h>
#include <os/lock.h>
#include <os/sched.h>
#include <os/time.h>
#include <os/mm.h>
#include <screen.h>
#include <printk.h>
#include <assert.h>
#include <os/kernel.h>
#include <os/loader.h>
#include <os/task.h>
#include <os/string.h>
#include <asm/regs.h>
#include <csr.h>
#include <os/smp.h>

#define MIN_TIME_SLICE 1
#define MAX_TIME_SLICE 8

extern void ret_from_exception(void);

static int program_count;

pcb_t pcb[NUM_MAX_TASK];
const ptr_t pid0_stack = INIT_KERNEL_STACK + PAGE_SIZE;
pcb_t pid0_pcb = {
    .pid = 0,
    .kernel_sp = (ptr_t)pid0_stack,
    .user_sp = (ptr_t)pid0_stack
};
pcb_t secondary_idle_pcb;

LIST_HEAD(ready_queue);
LIST_HEAD(sleep_queue);

/* global process id */
pid_t process_id = 1;

/**
 * @brief 求字符串长度
 * 
 * @param str 字符串
 * @param limit 长度限制
 * @return int 串长，失败返回-1
 */
static int bounded_strlen(const char *str, int limit){
    if(str == NULL){
        return -1;
    }
    for(int i=0; i<limit; ++i){
        if(str[i] == '\0'){
            return i;
        }
    }
    return -1;
}

/**
 * @brief 重置pcb，但保留两个栈，因为task1的栈分配是内存地址单增的无法回收
 * 
 * @param task 任务对应的pcb
 */
static void reset_pcb(pcb_t *task){
    ptr_t kernel_base = task->kernel_stack_base;
    ptr_t user_base = task->user_stack_base;

    memset(task, 0, sizeof(pcb_t));

    task->kernel_stack_base = kernel_base;
    task->user_stack_base = user_base;
    task->status = TASK_EXITED;
    task->task_id = -1;
    task->time_slice = 1;
    task->ticks_left = 1;

    list_init(&task->list);
    list_init(&task->wait_list);
}

/**
 * @brief 找pid对应的pcb
 * 
 * @param pid 进程的pid
 * @return pcb_t* pid对应的pcb
 */
static pcb_t *find_process(pid_t pid){
    if(pid <= 0){
        return NULL;
    }
    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(pcb[i].status!=TASK_EXITED && pcb[i].pid==pid){
            return &pcb[i];
        }
    }
    return NULL;
}

/**
 * @brief 标记可以被回收栈的进程pcb
 * 
 */
static void reap_exited(void){
    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(pcb[i].status==TASK_EXITED && pcb[i].recycle_pending && &pcb[i]!=current_running){
            pcb[i].recycle_pending = 0;
        }
    }
}

/**
 * @brief 初始化进程栈，构造假现场。复制参数到栈中，避免一直占用shell缓冲区
 * 
 * @param task 进程pcb
 * @param entry 程序入口
 * @param argc 参数个数
 * @param argv 参数
 * @param lengths 每个参数的长度
 */
static void init_process_stack(pcb_t *task, ptr_t entry, int argc, char **argv, int *lengths){
    ptr_t kernel_top = task->kernel_stack_base+PAGE_SIZE;
    ptr_t user_top = task->user_stack_base+PAGE_SIZE;
    ptr_t user_sp = user_top;
    ptr_t arg_ptrs[EXEC_MAX_ARGC]; // 存每个参数的地址

    memset((void *)task->kernel_stack_base, 0, PAGE_SIZE);
    memset((void *)task->user_stack_base, 0, PAGE_SIZE);

    // 复制参数到用户栈
    for(int i=argc-1; i>=0; --i){
        user_sp -= lengths[i]+1; // 给'\0'留空间
        memcpy((uint8_t*)user_sp, (const uint8_t*)argv[i], lengths[i]+1);
        arg_ptrs[i] = user_sp;
    }

    user_sp &= ~((ptr_t)7); // 清零后三位，8字节对齐
    user_sp -= (argc+1)*sizeof(char*); // 开空间存每个参数的地址，末尾存NULL

    char **user_argv = (char**)user_sp;

    for(int i=0; i<argc; ++i){
        user_argv[i] = (char*)arg_ptrs[i];
    }

    user_argv[argc] = NULL;

    user_sp &= ~((ptr_t)15); // 16字节对齐，后续参考p2的假现场构造

    regs_context_t *regs =
        (regs_context_t *)(kernel_top-sizeof(regs_context_t));

    switchto_context_t *switch_regs =
        (switchto_context_t *)((ptr_t)regs-sizeof(switchto_context_t));

    regs->regs[OFFSET_REG_SP>>3] = user_sp;
    regs->regs[OFFSET_REG_TP>>3] = (reg_t)task;
    regs->regs[OFFSET_REG_A0>>3] = argc;
    regs->regs[OFFSET_REG_A1>>3] = (reg_t)user_argv;

    regs->sepc = entry;
    regs->sstatus = SR_SPIE;

    switch_regs->regs[SWITCH_TO_RA>>3] = (reg_t)ret_from_exception;
    switch_regs->regs[SWITCH_TO_SP>>3] = (reg_t)switch_regs;

    task->kernel_sp = (ptr_t)switch_regs;
    task->user_sp = user_sp;
}

/**
 * @brief 初始化进程，参考p2 init_pcb
 * 
 * @param tasknum 任务个数
 */
void init_processes(int tasknum){
    current_running = &pid0_pcb;

    memset(&pid0_pcb, 0, sizeof(pcb_t));

    pid0_pcb.pid = 0;
    pid0_pcb.kernel_sp = pid0_stack;
    pid0_pcb.user_sp = pid0_stack;
    pid0_pcb.task_id = -1;
    pid0_pcb.status = TASK_RUNNING;

    list_init(&pid0_pcb.list);
    list_init(&pid0_pcb.wait_list);

    memset(&secondary_idle_pcb, 0, sizeof(pcb_t));
    secondary_idle_pcb.pid = 0;
    secondary_idle_pcb.task_id = -1;
    secondary_idle_pcb.status = TASK_READY;
    // 从核idle使用独立的栈
    secondary_idle_pcb.kernel_stack_base = allocKernelPage(1);
    secondary_idle_pcb.kernel_sp = secondary_idle_pcb.kernel_stack_base+PAGE_SIZE;
    secondary_idle_pcb.user_sp = INIT_KERNEL_STACK-PAGE_SIZE; // 对应head.S中CPU1启动栈顶
    list_init(&secondary_idle_pcb.list);
    list_init(&secondary_idle_pcb.wait_list);

    list_init(&ready_queue);
    list_init(&sleep_queue);

    assert(tasknum>0 && tasknum<=TASK_MAXNUM);

    program_count = tasknum;
    process_id = 1;

    memset(pcb, 0, sizeof(pcb));

    // p3-task1，只开pcb，清空，保留栈空间
    for(int i=0; i<NUM_MAX_TASK; ++i){
        pcb[i].kernel_stack_base = allocKernelPage(1);
        pcb[i].user_stack_base = allocUserPage(1);
        reset_pcb(&pcb[i]);
    }

    // 启动shell
    char *shell_argv[] = {"shell"};

    pid_t shell_pid = do_exec("shell", 1, shell_argv);
    assert(shell_pid==1);
}

/**
 * @brief 启动进程
 * 
 * @param name app名字
 * @param argc 参数个数
 * @param argv 参数
 * @return pid_t 进程pid，0表示失败
 */
pid_t do_exec(char *name, int argc, char **argv){
    reap_exited();

    if(name==NULL || argv==NULL || argc<1 || argc>EXEC_MAX_ARGC){
        return 0;
    }

    int name_len = bounded_strlen(name, TASK_NAME_LEN);
    if(name_len <= 0){
        return 0;
    }

    int task_id = -1;
    for(int i=0; i<program_count; ++i){
        if(strcmp(name, tasks[i].task_name) == 0){
            task_id = i;
            break;
        }
    }
    if(task_id < 0){
        return 0;
    }

    // // p3-task1 loader将程序加载到固定地址，无法并发
    // for(int i=0; i<NUM_MAX_TASK; ++i){
    //     if(pcb[i].status!=TASK_EXITED && pcb[i].task_id==task_id){
    //         return 0;
    //     }
    // }
    // 同程序已有存活实例时，共享现有映像
    int already_exist = 0;

    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(pcb[i].status!=TASK_EXITED && pcb[i].task_id==task_id){
            already_exist = 1;
            break;
        }
    }

    int lengths[EXEC_MAX_ARGC];
    int total_bytes = 0;
    for(int i=0; i<argc; ++i){
        lengths[i] = bounded_strlen(argv[i], EXEC_MAX_ARG_BYTES);
        if(lengths[i] < 0){
            return 0;
        }
        total_bytes += lengths[i]+1;
        if(total_bytes > EXEC_MAX_ARG_BYTES){
            return 0;
        }
    }

    if(process_id<=0 || process_id>=INT32_MAX){
        return 0;
    }

    // 找一个可以被复用的pcb作为当前进程的pcb
    pcb_t *task = NULL;
    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(pcb[i].status==TASK_EXITED && !pcb[i].recycle_pending){
            task = &pcb[i];
            break;
        }
    }
    if(task == NULL){
        return 0;
    }
    reset_pcb(task);

    // ptr_t entry = load_task_img(tasks[task_id].task_name, program_count);

    ptr_t entry = tasks[task_id].task_entry;
    if(!already_exist){
        entry = load_task_img(tasks[task_id].task_name, program_count);
    }

    if(entry == 0){
        return 0;
    }

    init_process_stack(task, entry, argc, argv, lengths);

    task->task_id = task_id;
    task->pid = process_id++;
    task->status = TASK_READY;

    list_add_tail(&task->list, &ready_queue);

    return task->pid;
}

void do_scheduler(void)
{
    // TODO: [p2-task3] Check sleep queue to wake up PCBs
    check_sleeping();

    /************************************************************/
    /* Do not touch this comment. Reserved for future projects. */
    /************************************************************/

    // TODO: [p2-task1] Modify the current_running pointer.
    pcb_t *prev = current_running;
    if(prev->status == TASK_RUNNING){
        prev->status = TASK_READY;
        if(prev->pid != 0){
            list_add_tail(&prev->list, &ready_queue);
        }
    }

    pcb_t *next;
    if(list_empty(&ready_queue)){ // 空就绪队列不再轮询sleep queue，将每个核自己的idle进行调度
        // next = &pid0_pcb;
        next = get_current_cpu_id()==0 ? &pid0_pcb : &secondary_idle_pcb;
    }else{
        list_node_t *node = list_pop_front(&ready_queue);
        next = list_entry(node, pcb_t, list);
    }

    if(next->time_slice == 0){
        next->time_slice = MIN_TIME_SLICE;
    }

    next->ticks_left = next->time_slice;
    next->status = TASK_RUNNING;
    current_running = next;

    if(prev == next){
        return;
    }

    // TODO: [p2-task1] switch_to current_running
    switch_to(prev, next);

}

void do_sleep(uint32_t sleep_time)
{
    // TODO: [p2-task3] sleep(seconds)
    // NOTE: you can assume: 1 second = 1 `timebase` ticks
    // 1. block the current_running
    // 2. set the wake up time for the blocked task
    // 3. reschedule because the current_running is blocked.
    current_running->wakeup_time = get_timer()+sleep_time;
    do_block(&current_running->list, &sleep_queue);
}

void do_block(list_node_t *pcb_node, list_head *queue)
{
    // TODO: [p2-task2] block the pcb task into the block queue
    pcb_t *task = list_entry(pcb_node, pcb_t, list);

    task->status = TASK_BLOCKED;
    list_add_tail(pcb_node, queue);

    do_scheduler(); // 从队列删除block的PCB，并唤醒下一个READY的PCB
}

void do_unblock(list_node_t *pcb_node)
{
    // TODO: [p2-task2] unblock the `pcb` from the block queue
    pcb_t *task = list_entry(pcb_node, pcb_t, list);

    if(task->status != TASK_BLOCKED){
        return;
    }
    list_del(pcb_node);
    task->status = TASK_READY;
    list_add_tail(pcb_node, &ready_queue);
}

static void update_time_slices(void){
    uint64_t max_progress = 0;
    uint64_t min_progress = UINT64_MAX;

    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(!pcb[i].workload_valid){
            continue;
        }

        if(pcb[i].progress>max_progress){
            max_progress = pcb[i].progress;
        }

        if(pcb[i].progress<min_progress){
            min_progress = pcb[i].progress;
        }
    }

    uint64_t max_lag = max_progress-min_progress;

    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(!pcb[i].workload_valid){
            continue;
        }

        uint64_t lag = max_progress-pcb[i].progress;
        uint64_t slice = MIN_TIME_SLICE;

        if(max_lag>0){
            slice += lag*(MAX_TIME_SLICE-MIN_TIME_SLICE)/max_lag;
        }

        pcb[i].time_slice = (uint32_t)slice;
    }
}

long do_set_sche_workload(int workload){
    pcb_t *task = current_running;

    if(workload <= 0){
        return -1;
    }

    if(!task->workload_valid){
        task->workload_valid = 1;
        task->initial_workload = workload;
        task->last_workload = workload;
        task->completed_rounds = 0;
        task->progress = 0;
    }else{
        if(workload > task->last_workload){ // 新一轮开始
            task->completed_rounds++;
        }

        task->last_workload = workload;
        task->progress =
                task->completed_rounds*(uint64_t)task->initial_workload+(uint64_t)(task->initial_workload-workload);
    }

    update_time_slices();

    return 0;
}

/**
 * @brief 终止进程
 * 
 * @param task 进程pcb
 */
static void terminate_process(pcb_t *task){
    do_barrier_cancel_wait(&task->list);
    list_del(&task->list);

    task->status = TASK_EXITED;
    task->workload_valid = 0;

    do_mutex_release_all(task->pid);
    do_mbox_release_all(task->pid);

    while(!list_empty(&task->wait_list)){
        do_unblock(task->wait_list.next);
    }

    task->recycle_pending = task==current_running;
}

/**
 * @brief 退出正在运行的进程
 * 
 */
void do_exit(void){
    if(current_running->pid == 0){
        return;
    }
    terminate_process(current_running);
    do_scheduler();
    assert(0);
    while(1){
    }
}

/**
 * @brief 杀进程
 * 
 * @param pid 进程pid
 * @return int 1成功，0失败
 */
int do_kill(pid_t pid){
    pcb_t *task = find_process(pid);

    if(task == NULL){
        return 0;
    }
    if(task == current_running){
        do_exit();
        return 1;
    }
    if(task->status == TASK_RUNNING){ // 正在运行且不在本核，标记退出请求
        task->kill_pending = 1;
        return 1;
    }
    terminate_process(task);
    return 1;
}

/**
 * @brief 等待pid的进程结束
 * 
 * @param pid 进程pid
 * @return int 原本的pid
 */
int do_waitpid(pid_t pid){
    pcb_t *target = find_process(pid);
    if(target==NULL || target==current_running){
        return 0;
    }
    do_block(&current_running->list, &target->wait_list);
    return pid;
}

pid_t do_getpid(void){
    return current_running->pid;
}

/**
 * @brief 打印所有进程信息
 * 
 */
void do_process_show(void){
    static const char *status_names[] = {
        "BLOCKED",
        "RUNNING",
        "READY",
        "EXITED"
    };

    printk("PID  STATUS  NAME\n");
    printk("0  %s  idle\n", status_names[pid0_pcb.status]);

    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(pcb[i].status == TASK_EXITED){
            continue;
        }

        printk("%d  %s  %s\n",
               pcb[i].pid,
               status_names[pcb[i].status],
               tasks[pcb[i].task_id].task_name);
    }
}