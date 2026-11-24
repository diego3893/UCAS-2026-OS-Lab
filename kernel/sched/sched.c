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
    // ptr_t kernel_base = task->kernel_stack_base;
    // ptr_t user_base = task->user_stack_base;

    memset(task, 0, sizeof(pcb_t));

    // task->kernel_stack_base = kernel_base;
    // task->user_stack_base = user_base;
    task->status = TASK_EXITED;
    task->task_id = -1;
    task->time_slice = 1;
    task->ticks_left = 1;
    task->cpu_mask = (1<<NR_CPUS)-1; // 默认0x03
    task->cpu_id = -1;

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
        if(pcb[i].pid == pid){
            return &pcb[i];
        }
    }
    return NULL;
}

// 根据进程号找仍有活动线程的线程组
static address_space_t *find_group(pid_t tgid){
    if(tgid <= 0){
        return NULL;
    }
    for(int i=0; i<NUM_MAX_TASK; ++i){
        address_space_t *space = pcb[i].space;
        if(space!=NULL && space->tgid==tgid && space->live>0){
            return space;
        }
    }

    return NULL;
}

/**
 * @brief 回收pcb
 * 
 */
void reap_exited(void){
    for(int i=0; i<NUM_MAX_TASK; ++i){
        pcb_t *task = &pcb[i];
        if(task->pid==0 || task->status!=TASK_EXITED || task==current_running){
            continue;
        }

        address_space_t *space = task->space;
        assert(space!=NULL);

        if(task->recycle_pending){
            freePage(task->kernel_stack_base);
            task->kernel_stack_base = 0;

            if(task->is_thread && task->user_stack_base!=0){
                uintptr_t va = task->user_stack_top-PAGE_SIZE;
                PTE *entry = walk_user_pte(task->pgdir, va, 0);
                assert(entry!=NULL && (*entry&_PAGE_PRESENT));
                // 先撤销映射并同步两核，再将页框放回空闲链表
                *entry = 0;
                flush_tlb_all();
                freePage(task->user_stack_base);
                task->user_stack_base = 0;
            }
            // 主线程栈随整个地址空间一起回收
            task->recycle_pending = 0;
        }

        // 保留结束记录，允许目标先退出、调用者后join
        if(task->is_thread && !task->joined && !space->dying){
            continue;
        }

        assert(space->refs>0);
        space->refs--; // 回收一个pcb，引用减1

        if(space->refs == 0){
            assert(space->live==0);
            flush_tlb_all();
            free_user_pgdir(space->pgdir); // 回收用户所有页，包括栈页
            freePage((ptr_t)space);
        }
        reset_pcb(task);
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
    // user_stack_base保存kva，这是给内核看的；用户寄存器及argv保存va
    ptr_t user_top = task->user_stack_top;
    ptr_t user_bottom = user_top-PAGE_SIZE;
    ptr_t user_sp = user_top;
    ptr_t arg_ptrs[EXEC_MAX_ARGC]; // 存每个参数的地址

    memset((void*)task->kernel_stack_base, 0, PAGE_SIZE);
    memset((void*)task->user_stack_base, 0, PAGE_SIZE);

    // 复制参数到用户栈
    for(int i=argc-1; i>=0; --i){
        user_sp -= lengths[i]+1; // '\0'

        ptr_t dest = task->user_stack_base+(user_sp-user_bottom);
        memcpy((uint8_t*)dest, (const uint8_t*)argv[i], lengths[i]+1);

        arg_ptrs[i] = user_sp;
    }

    user_sp &= ~((ptr_t)7); // 清零后三位，8字节对齐
    user_sp -= (argc+1)*sizeof(char*); // 开空间存每个参数的地址，末尾存NULL

    ptr_t user_argv_va = user_sp;
    char **user_argv = (char**)(task->user_stack_base+(user_sp-user_bottom));

    for(int i=0; i<argc; ++i){
        user_argv[i] = (char*)arg_ptrs[i];
    }

    user_argv[argc] = NULL;

    user_sp &= ~((ptr_t)15); // 16字节对齐，后续参考p2的假现场构造

    regs_context_t *regs =
        (regs_context_t*)(kernel_top-sizeof(regs_context_t));

    switchto_context_t *switch_regs =
        (switchto_context_t*)((ptr_t)regs-sizeof(switchto_context_t));

    regs->regs[OFFSET_REG_SP>>3] = user_sp;
    regs->regs[OFFSET_REG_TP>>3] = (reg_t)task;
    regs->regs[OFFSET_REG_A0>>3] = argc;
    regs->regs[OFFSET_REG_A1>>3] = user_argv_va;

    regs->sepc = entry;
    regs->sstatus = SR_SPIE|SR_SUM;

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

    // 保留之前打印调试信息的光标，避免相互覆盖
    int boot_cursor_x = pid0_pcb.cursor_x;
    int boot_cursor_y = pid0_pcb.cursor_y;
    memset(&pid0_pcb, 0, sizeof(pcb_t));
    pid0_pcb.cursor_x = boot_cursor_x;
    pid0_pcb.cursor_y = boot_cursor_y;

    pid0_pcb.pid = 0;
    pid0_pcb.kernel_sp = pid0_stack;
    pid0_pcb.user_sp = pid0_stack;
    pid0_pcb.task_id = -1;
    pid0_pcb.status = TASK_RUNNING;
    pid0_pcb.cpu_mask = 1;
    pid0_pcb.cpu_id = 0;
    pid0_pcb.pgdir = pa2kva(PGDIR_PA);

    list_init(&pid0_pcb.list);
    list_init(&pid0_pcb.wait_list);

    memset(&secondary_idle_pcb, 0, sizeof(pcb_t));
    secondary_idle_pcb.pid = 0;
    secondary_idle_pcb.task_id = -1;
    secondary_idle_pcb.status = TASK_READY;
    // 从核idle使用独立的栈
    // 从核idle使用独立的栈
    secondary_idle_pcb.kernel_stack_base = INIT_KERNEL_STACK-PAGE_SIZE;
    secondary_idle_pcb.kernel_sp = INIT_KERNEL_STACK;
    secondary_idle_pcb.user_sp = INIT_KERNEL_STACK; // 对应head.S中CPU1启动栈顶
    secondary_idle_pcb.pgdir = pa2kva(PGDIR_PA);
    secondary_idle_pcb.cpu_mask = 2;
    secondary_idle_pcb.cpu_id = -1; // 从核此时未运行idle执行流
    list_init(&secondary_idle_pcb.list);
    list_init(&secondary_idle_pcb.wait_list);

    list_init(&ready_queue);
    list_init(&sleep_queue);

    assert(tasknum>0 && tasknum<=TASK_MAXNUM);

    program_count = tasknum;
    process_id = 1;

    memset(pcb, 0, sizeof(pcb));

    for(int i=0; i<NUM_MAX_TASK; ++i){
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
#if 0
    int already_exist = 0;

    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(pcb[i].status!=TASK_EXITED && pcb[i].task_id==task_id){
            already_exist = 1;
            break;
        }
    }
#endif

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
        if(pcb[i].pid==0 && pcb[i].status==TASK_EXITED && !pcb[i].recycle_pending){
            task = &pcb[i];
            break;
        }
    }
    if(task == NULL){
        return 0;
    }
    reset_pcb(task);

    if(current_running->pid != 0){
        task->cpu_mask = current_running->cpu_mask; // 普通子进程继承父进程的CPU掩码
    }

    // ptr_t entry = load_task_img(tasks[task_id].task_name, program_count);

    task->pgdir = allocPage(1);
    if(task->pgdir == 0){
        return 0;
    }

    clear_pgdir(task->pgdir);
    share_pgtable(task->pgdir, pa2kva(PGDIR_PA)); // 高半区设置为公共

    ptr_t entry = load_task_img(tasks[task_id].task_name, program_count, task->pgdir);

    // 加载镜像之后先分配栈，再构造假现场
    if(entry != 0){
        task->kernel_stack_base = allocKernelPage(1);
        task->user_stack_base = alloc_page_helper(USER_STACK_ADDR-PAGE_SIZE, task->pgdir);
    }

    if(entry==0 || task->kernel_stack_base==0 || task->user_stack_base==0){
        freePage(task->kernel_stack_base);
        free_user_pgdir(task->pgdir);
        reset_pcb(task);
        return 0;
    }
    // 创建地址空间
    address_space_t *space = (address_space_t*)allocPage(1);
    if(space == NULL){
        freePage(task->kernel_stack_base);
        free_user_pgdir(task->pgdir);
        reset_pcb(task);
        return 0;
    }
    memset(space, 0, PAGE_SIZE);
    list_init(&space->waiters);
    task->space = space;
    task->user_stack_top = USER_STACK_ADDR;

    init_process_stack(task, entry, argc, argv, lengths);

    task->task_id = task_id;
    task->pid = process_id++;
    task->tgid = task->pid;
    task->status = TASK_READY;
    space->pgdir = task->pgdir;
    space->tgid = task->tgid;
    space->refs = 1;
    space->live = 1;

    list_add_tail(&task->list, &ready_queue);

    return task->tgid;
}

// 检查addr是否可以作为线程入口
static int user_code_mapped(uintptr_t pgdir, uintptr_t addr){
    if((addr&1UL) || addr>=USER_VA_LIMIT-1){ // 指令地址按照riscv需要2字节对齐
        return 0;
    }
    uint64_t required = _PAGE_PRESENT|_PAGE_USER|_PAGE_EXEC;
    PTE *entry = walk_user_pte(pgdir, addr, 0);
    if(entry==NULL || (*entry&required)!=required){
        return 0;
    }
    return 1;
}

/**
 * @brief 创建线程
 * 
 * @param trampoline 用户态包装函数
 * @param start_routine 指定的线程函数
 * @param arg 线程函数参数
 * @return long 线程id
 */
long do_thread_create(uintptr_t trampoline, uintptr_t start_routine, uintptr_t arg){
    reap_exited();
    pcb_t *owner = current_running;
    address_space_t *space = owner->space; // 上级线程地址空间

    if(space==NULL || space->dying || process_id<=0 || process_id>=INT32_MAX){
        return 0;
    }
    if(!user_code_mapped(owner->pgdir, trampoline) || !user_code_mapped(owner->pgdir, start_routine)){
        return 0;
    }

    pcb_t *task = NULL;
    uintptr_t stack_top = 0;

    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(pcb[i].pid!=0 || pcb[i].status!=TASK_EXITED || pcb[i].recycle_pending){
            continue;
        }

        // 每个PCB槽位对应不同的线程栈地址，与主线程栈错开；偏移两页，留一页空白
        uintptr_t candidate = USER_STACK_ADDR-(uintptr_t)(i+1)*2*PAGE_SIZE;
        PTE *entry = walk_user_pte(owner->pgdir, candidate-PAGE_SIZE, 0);
        if(entry!=NULL && *entry!=0){
            continue;
        }
        task = &pcb[i];
        stack_top = candidate;
        break;
    }

    if(task == NULL){
        return 0;
    }

    reset_pcb(task); // 开始继承
    task->pgdir = owner->pgdir;
    task->space = space;
    task->tgid = owner->tgid;
    task->is_thread = 1;
    task->task_id = owner->task_id;
    task->cpu_mask = owner->cpu_mask;
    task->cursor_x = owner->cursor_x;
    task->cursor_y = owner->cursor_y;
    task->user_stack_top = stack_top;
    // 分配自己的用户栈和内核栈
    task->kernel_stack_base = allocKernelPage(1);
    if(task->kernel_stack_base == 0){
        reset_pcb(task);
        return 0;
    }
    task->user_stack_base = alloc_page_helper(stack_top-PAGE_SIZE, task->pgdir);
    if(task->user_stack_base == 0){
        freePage(task->kernel_stack_base);
        reset_pcb(task);
        return 0;
    }

    PTE *stack_entry = walk_user_pte(task->pgdir, stack_top-PAGE_SIZE, 0);
    assert(stack_entry!=NULL);
    *stack_entry &= ~(PTE)_PAGE_EXEC;

    // argc=0复用现有假现场初始化，再手动设置入口和参数
    init_process_stack(task, trampoline, 0, NULL, NULL);
    regs_context_t *regs = (regs_context_t*)(task->kernel_stack_base+PAGE_SIZE-sizeof(regs_context_t));
    regs->regs[OFFSET_REG_A0>>3] = start_routine;
    regs->regs[OFFSET_REG_A1>>3] = arg;

    task->pid = process_id++;
    space->refs++;
    space->live++;
    flush_tlb_all();
    task->status = TASK_READY;
    list_add_tail(&task->list, &ready_queue);

    return task->pid;
}

int do_thread_join(pid_t tid){
    pcb_t *self = current_running;
    pcb_t *target = find_process(tid);
    if(target==NULL || target==self || !target->is_thread || target->space!=self->space || target->joined){
        return -1;
    }
    // 线程的joiner应该唯一
    if(target->joiner!=0 && target->joiner!=self->pid){
        return -1;
    }

    // 沿join关系检查，避免互相join
    pcb_t *cursor = target;
    for(int i=0; cursor!=NULL && i<NUM_MAX_TASK; ++i){
        if(cursor == self){
            return -1;
        }
        if(cursor->join_target == 0){
            break;
        }
        cursor = find_process(cursor->join_target);
    }

    target->joiner = self->pid;
    self->join_target = tid;

    while(target->status != TASK_EXITED){
        do_block(&self->list, &target->wait_list);
    }

    self->join_target = 0;
    target->joined = 1;

    return 0;
}

/**
 * @brief 创建并绑定进程，或修改已有进程的CPU掩码
 *
 * @param mask CPU掩码
 * @param pid 0为创建新进程，否则修改已有pcb
 * @param name 创建时的程序名，如果是修改则可以为NULL
 * @return pid_t 返回pid，失败为0
 */
pid_t do_taskset(int mask, pid_t pid, char *name){
    if(mask<1 || mask>((1<<NR_CPUS)-1) || pid<0){
        return 0;
    }

    if(pid == 0){ // 创建进程
        if(name == NULL){
            return 0;
        }
        char *argv[] = {name};
        pid = do_exec(name, 1, argv);
        if(pid == 0){
            return 0;
        }
    }

    address_space_t *space = find_group(pid);
    if(space == NULL){
        return 0;
    }

    for(int i=0; i<NUM_MAX_TASK; ++i){
        pcb_t *task = &pcb[i];
        if(task->space!=space || task->status==TASK_EXITED){
            continue;
        }
        task->cpu_mask = mask;

        // 正在运行，但新掩码和当前核冲突
        if(task->status==TASK_RUNNING && !(mask&(1U<<task->cpu_id))){
            task->ticks_left = 1;
        }
    }

    if(current_running->space==space && !(mask&(1U<<current_running->cpu_id))){ // 本核进程，立即调度
        do_scheduler();
    }

    return pid;
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

    int cpu_id = (int)get_current_cpu_id();
    prev->cpu_id = -1; // 重置CPU状态

    if(prev->status == TASK_RUNNING){
        prev->status = TASK_READY;
        if(prev->pid != 0){
            list_add_tail(&prev->list, &ready_queue);
        }
    }

    // pcb_t *next;
    // if(list_empty(&ready_queue)){ // 空就绪队列不再轮询sleep queue，将每个核自己的idle进行调度
    //     // next = &pid0_pcb;
    //     next = get_current_cpu_id()==0 ? &pid0_pcb : &secondary_idle_pcb;
    // }else{
    //     list_node_t *node = list_pop_front(&ready_queue);
    //     next = list_entry(node, pcb_t, list);
    // }
    pcb_t *next = cpu_id==0 ? &pid0_pcb : &secondary_idle_pcb;
    // 选取第一个可以使用本核的进程进行调度
    for(list_node_t *node=ready_queue.next; node!=&ready_queue; node=node->next){
        pcb_t *task = list_entry(node, pcb_t, list);
        if(task->cpu_mask&(1U<<cpu_id)){
            next = task;
            list_del(node);
            break;
        }
    }

    if(next->time_slice == 0){
        next->time_slice = MIN_TIME_SLICE;
    }

    next->ticks_left = next->time_slice;
    next->status = TASK_RUNNING;
    next->cpu_id = cpu_id;
    current_running = next;

    if(prev == next){
        return;
    }

    // TODO: [p2-task1] switch_to current_running
    unsigned asid = next->tgid==0 ? 0 : ((unsigned)next->tgid&0xffffU);

    set_satp(SATP_MODE_SV39, asid, kva2pa(next->pgdir)>>NORMAL_PAGE_SHIFT); // 切换之前设置新的satp并刷TLB
    local_flush_tlb_all();

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
    if(task->status == TASK_EXITED){
        return;
    }

    do_barrier_cancel_wait(&task->list);
    list_del(&task->list);

    task->status = TASK_EXITED;
    task->workload_valid = 0;

    // 等待者被结束时，取消它对join占用
    if(task->join_target != 0){
        pcb_t *target = find_process(task->join_target);

        if(target!=NULL && target->joiner==task->pid){
            target->joiner = 0;
        }

        task->join_target = 0;
    }

    do_mutex_release_all(task->pid);
    do_mbox_release_all(task->pid);

    while(!list_empty(&task->wait_list)){
        do_unblock(task->wait_list.next);
    }

    address_space_t *space = task->space;
    assert(space!=NULL && space->live>0);
    space->live--;

    if(space->live == 0){
        space->dying = 1;

        while(!list_empty(&space->waiters)){
            do_unblock(space->waiters.next);
        }
    }

    task->recycle_pending = 1;
}

/**
 * @brief 结束对应的整个线程组
 * 
 * @param space 进程的地址空间
 */
static void terminate_group(address_space_t *space){
    space->dying = 1;
    for(int i=0; i<NUM_MAX_TASK; ++i){
        pcb_t *task = &pcb[i];
        if(task->space!=space || task->status==TASK_EXITED){
            continue;
        }
        if(task->status == TASK_RUNNING){
            task->kill_pending = 1; // 正在运行且不在本核，标记退出请求
        }else{
            terminate_process(task);
        }
    }
}

/**
 * @brief 结束向前线程
 * 
 */
void do_thread_exit(void){
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
 * @brief 退出正在运行的进程
 * 
 */
void do_exit(void){
    if(current_running->pid == 0){
        return;
    }
    terminate_group(current_running->space);
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
    address_space_t *space = find_group(pid);

    if(space == NULL){
        return 0;
    }
    if(space == current_running->space){
        do_exit();
        return 1;
    }
    terminate_group(space);
    return 1;
}

/**
 * @brief 等待pid的进程结束
 * 
 * @param pid 进程pid
 * @return int 原本的pid
 */
int do_waitpid(pid_t pid){
    address_space_t *space = find_group(pid);
    if(space==NULL || space==current_running->space){
        return 0;
    }
    do_block(&current_running->list, &space->waiters);
    return pid;
}

pid_t do_getpid(void){
    return current_running->tgid;
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

    printk("PID  TID  STATUS  MASK  CPU  NAME\n");
    printk("0  0  %s  0x%x  %d  idle0\n",
        status_names[pid0_pcb.status], pid0_pcb.cpu_mask, pid0_pcb.cpu_id);
    printk("0  0  %s  0x%x  %d  idle1\n",
        status_names[secondary_idle_pcb.status],
        secondary_idle_pcb.cpu_mask, secondary_idle_pcb.cpu_id);

    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(pcb[i].status == TASK_EXITED){
            continue;
        }

        printk("%d  %d  %s  0x%x  %d  %s\n",
            pcb[i].tgid, pcb[i].pid, status_names[pcb[i].status],
            pcb[i].cpu_mask, pcb[i].cpu_id,
            tasks[pcb[i].task_id].task_name);
    }
}