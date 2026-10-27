#include <common.h>
#include <asm.h>
#include <asm/unistd.h>
#include <os/loader.h>
#include <os/irq.h>
#include <os/sched.h>
#include <os/lock.h>
#include <os/kernel.h>
#include <os/task.h>
#include <os/string.h>
#include <os/mm.h>
#include <os/time.h>
#include <sys/syscall.h>
#include <screen.h>
#include <printk.h>
#include <assert.h>
#include <type.h>
#include <csr.h>
#include <asm/regs.h>
#include <os/smp.h>

extern void ret_from_exception();
static long do_readch(void);

// Task info array
task_info_t tasks[TASK_MAXNUM];


static void init_jmptab(void)
{
    volatile long (*(*jmptab))() = (volatile long (*(*))())KERNEL_JMPTAB_BASE;

    jmptab[CONSOLE_PUTSTR]  = (long (*)())port_write;
    jmptab[CONSOLE_PUTCHAR] = (long (*)())port_write_ch;
    jmptab[CONSOLE_GETCHAR] = (long (*)())port_read_ch;
    jmptab[SD_READ]         = (long (*)())sd_read;
    jmptab[SD_WRITE]        = (long (*)())sd_write;
    jmptab[QEMU_LOGGING]    = (long (*)())qemu_logging;
    jmptab[SET_TIMER]       = (long (*)())set_timer;
    jmptab[READ_FDT]        = (long (*)())read_fdt;
    jmptab[MOVE_CURSOR]     = (long (*)())screen_move_cursor;
    jmptab[PRINT]           = (long (*)())screen_write;
    jmptab[YIELD]           = (long (*)())do_scheduler;
    jmptab[MUTEX_INIT]      = (long (*)())do_mutex_lock_init;
    jmptab[MUTEX_ACQ]       = (long (*)())do_mutex_lock_acquire;
    jmptab[MUTEX_RELEASE]   = (long (*)())do_mutex_lock_release;

    // TODO: [p2-task1] (S-core) initialize system call table.
    jmptab[REFLUSH] = (long (*)())screen_reflush;

}

static int init_task_info(uint32_t app_info_offset, int tasknum)
{
    // TODO: [p1-task4] Init 'tasks' array via reading app-info sector
    // NOTE: You need to get some related arguments from bootblock first
    if(tasknum<=0 || tasknum>TASK_MAXNUM){
        return 0;
    }

    uint32_t info_size = tasknum*sizeof(task_info_t);
    uint32_t block_id = app_info_offset/SECTOR_SIZE;
    uint32_t block_offset = app_info_offset%SECTOR_SIZE;
    uint32_t info_sectors = NBYTES2SEC(block_offset+info_size);

    bios_sd_read(TASK_BUFFER_BASE, info_sectors, block_id);
    memcpy((uint8_t*)tasks, (uint8_t*)(TASK_BUFFER_BASE+block_offset), info_size);

    return tasknum;
}

static void read_task_name(char *taskname){
    int len = 0;
    while(1){
        int ch = bios_getchar();
        if(ch == -1){
            continue;
        }
        if(ch=='\r' || ch=='\n'){
            taskname[len] = '\0';
            bios_putstr("\n\r");
            return;
        }
        if(ch=='\b' || ch==127){
            if(len>0){
                len--;
                bios_putstr("\b \b");
            }
            continue;
        }
        if(len < TASK_NAME_LEN-1){
            taskname[len++] = ch;
            bios_putchar(ch);
        }
    }
}

/************************************************************/
static void init_pcb(int tasknum)
{
    init_processes(tasknum);
}

static void init_syscall(void)
{
    // TODO: [p2-task3] initialize system call table.
    for(int i=0; i<NUM_SYSCALLS; ++i){
        syscall[i] = 0;
    }

    syscall[SYSCALL_SLEEP] = (long (*)())do_sleep;
    syscall[SYSCALL_YIELD] = (long (*)())do_scheduler;
    syscall[SYSCALL_WRITE] = (long (*)())screen_write;
    syscall[SYSCALL_CURSOR] = (long (*)())screen_move_cursor;
    syscall[SYSCALL_REFLUSH] = (long (*)())screen_reflush;
    syscall[SYSCALL_GET_TIMEBASE] = (long (*)())get_time_base;
    syscall[SYSCALL_GET_TICK] = (long (*)())get_ticks;
    syscall[SYSCALL_LOCK_INIT] = (long (*)())do_mutex_lock_init;
    syscall[SYSCALL_LOCK_ACQ] = (long (*)())do_mutex_lock_acquire;
    syscall[SYSCALL_LOCK_RELEASE] = (long (*)())do_mutex_lock_release;
    syscall[SYSCALL_SET_SCHE_WORKLOAD] = (long (*)())do_set_sche_workload;
    syscall[SYSCALL_EXEC] = (long (*)())do_exec;
    syscall[SYSCALL_EXIT] = (long (*)())do_exit;
    syscall[SYSCALL_KILL] = (long (*)())do_kill;
    syscall[SYSCALL_WAITPID] = (long (*)())do_waitpid;
    syscall[SYSCALL_PS] = (long (*)())do_process_show;
    syscall[SYSCALL_GETPID] = (long (*)())do_getpid;
    syscall[SYSCALL_READCH] = (long (*)())do_readch;
    syscall[SYSCALL_CLEAR] = (long (*)())screen_clear;
    syscall[SYSCALL_BARR_INIT] = (long (*)())do_barrier_init;
    syscall[SYSCALL_BARR_WAIT] = (long (*)())do_barrier_wait;
    syscall[SYSCALL_BARR_DESTROY] = (long (*)())do_barrier_destroy;
    syscall[SYSCALL_COND_INIT] = (long (*)())do_condition_init;
    syscall[SYSCALL_COND_WAIT] = (long (*)())do_condition_wait;
    syscall[SYSCALL_COND_SIGNAL] = (long (*)())do_condition_signal;
    syscall[SYSCALL_COND_BROADCAST] = (long (*)())do_condition_broadcast;
    syscall[SYSCALL_COND_DESTROY] = (long (*)())do_condition_destroy;
    syscall[SYSCALL_MBOX_OPEN] = (long (*)())do_mbox_open;
    syscall[SYSCALL_MBOX_CLOSE] = (long (*)())do_mbox_close;
    syscall[SYSCALL_MBOX_SEND] = (long (*)())do_mbox_send;
    syscall[SYSCALL_MBOX_RECV] = (long (*)())do_mbox_recv;
    syscall[SYSCALL_TASKSET] = (long (*)())do_taskset;
}

static long do_readch(void){
    return bios_getchar();
}
/************************************************************/

int main(uint64_t app_info_offset, uint64_t tasknum_from_boot)
{
    if(get_current_cpu_id() != 0){
        current_running = &secondary_idle_pcb;

        lock_kernel();

        current_running->status = TASK_RUNNING;
        current_running->cpu_id = (int)get_current_cpu_id();

        // 从核只设置本核入口，不重新初始化全局中断表。
        setup_exception();
        bios_set_timer(get_ticks()+TIMER_INTERVAL);

        current_running->cursor_x = pid0_pcb.cursor_x;
        current_running->cursor_y = pid0_pcb.cursor_y;
        printk("> [SMP] CPU1 entered main.\n");

        unlock_kernel();

        goto kernel_idle; // 前往使能设置
    }
    // Init jump table provided by kernel and bios(ΦωΦ)
    init_jmptab();
    
    smp_init(); // 主核初始化大内核锁

    // Init task information (〃'▽'〃)
    int tasknum = init_task_info((uint32_t)app_info_offset, (int)tasknum_from_boot);

    // Init Process Control Blocks |•'-'•) ✧
    init_pcb(tasknum);
    printk("> [INIT] PCB initialization succeeded.\n");

    // Read CPU frequency (｡•ᴗ-)_
    time_base = bios_read_fdt(TIMEBASE);

    // Init lock mechanism o(´^｀)o
    init_locks();
    init_barriers();
    init_conditions();
    init_mbox();
    printk("> [INIT] Lock mechanism initialization succeeded.\n");

    // Init interrupt (^_^)
    init_exception();
    printk("> [INIT] Interrupt processing initialization succeeded.\n");

    // Init system call table (0_0)
    init_syscall();
    printk("> [INIT] System call initialized successfully.\n");

    // Init screen (QAQ)
    init_screen();
    printk("> [INIT] SCREEN initialization succeeded.\n");

    // TODO: [p2-task4] Setup timer interrupt and enable all interrupt globally
    // NOTE: The function of sstatus.sie is different from sie's
    bios_set_timer(get_ticks()+TIMER_INTERVAL);

    lock_kernel();
    printk("> [SMP] CPU0 entered main.\n");
    wakeup_other_hart();

    unlock_kernel();


    // TODO: Load tasks by either task id [p1-task3] or task name [p1-task4],
    //   and then execute them.

    // Infinite while loop, where CPU stays in a low-power state (QAQQQQQQQQQQQ)
kernel_idle:
    while (1)
    {
        // If you do non-preemptive scheduling, it's used to surrender control
        // do_scheduler();

        // If you do preemptive scheduling, they're used to enable CSR_SIE and wfi
        enable_preempt();
        enable_interrupt();
        while(1){
            asm volatile("wfi");
        }
    }

    return 0;
}
