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

extern void ret_from_exception();

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
static void init_pcb_stack(
    ptr_t kernel_stack, ptr_t user_stack, ptr_t entry_point,
    pcb_t *pcb)
{
     /* TODO: [p2-task3] initialization of registers on kernel stack
      * HINT: sp, ra, sepc, sstatus
      * NOTE: To run the task in user mode, you should set corresponding bits
      *     of sstatus(SPP, SPIE, etc.).
      */
    regs_context_t *pt_regs =
        (regs_context_t *)(kernel_stack - sizeof(regs_context_t));


    /* TODO: [p2-task1] set sp to simulate just returning from switch_to
     * NOTE: you should prepare a stack, and push some values to
     * simulate a callee-saved context.
     */
    switchto_context_t *pt_switchto =
        (switchto_context_t *)((ptr_t)pt_regs - sizeof(switchto_context_t));

    memset(pt_regs, 0, sizeof(regs_context_t));
    memset(pt_switchto, 0, sizeof(switchto_context_t));

    pt_switchto->regs[0] = entry_point; // 假现场的ra指向入口

    pt_switchto->regs[1] = user_stack-sizeof(switchto_context_t); // 预留假现场的栈空间
    pcb->kernel_sp = (ptr_t)pt_switchto;
    pcb->user_sp = user_stack;
}

static void init_pcb(int tasknum)
{
    /* TODO: [p2-task1] load needed tasks and init their corresponding PCB */
    static const char *task_names[] = {
        "lock1",
        "lock2"
    };

    int task_count = sizeof(task_names)/sizeof(task_names[0]);

    list_init(&ready_queue);
    list_init(&sleep_queue);

    memset(&pid0_pcb.list, 0, sizeof(list_node_t));
    list_init(&pid0_pcb.list);

    pid0_pcb.status = TASK_RUNNING;
    pid0_pcb.cursor_x = 0;
    pid0_pcb.cursor_y = 0;
    pid0_pcb.wakeup_time = 0;

    for(int i=0; i<task_count; ++i){
        uint64_t entry_point = load_task_img(task_names[i], tasknum);

        assert(entry_point!=0);

        // 向下生长！分配页的高位地址
        ptr_t kernel_stack = allocKernelPage(1)+PAGE_SIZE;

        ptr_t user_stack = allocUserPage(1)+PAGE_SIZE;

        memset(&pcb[i], 0, sizeof(pcb_t));

        pcb[i].pid = process_id++;
        pcb[i].status = TASK_READY;
        pcb[i].cursor_x = 0;
        pcb[i].cursor_y = 0;
        pcb[i].wakeup_time = 0;

        list_init(&pcb[i].list);

        init_pcb_stack(kernel_stack, user_stack, entry_point, &pcb[i]);

        list_add_tail(&pcb[i].list, &ready_queue);
    }

    /* TODO: [p2-task1] remember to initialize 'current_running' */
    current_running = &pid0_pcb; // 指向内核进程
}

static void init_syscall(void)
{
    // TODO: [p2-task3] initialize system call table.
}
/************************************************************/

int main(uint64_t app_info_offset, uint64_t tasknum_from_boot)
{
    // Init jump table provided by kernel and bios(ΦωΦ)
    init_jmptab();

    // Init task information (〃'▽'〃)
    int tasknum = init_task_info((uint32_t)app_info_offset, (int)tasknum_from_boot);

    // Init Process Control Blocks |•'-'•) ✧
    init_pcb(tasknum);
    printk("> [INIT] PCB initialization succeeded.\n");

    // Read CPU frequency (｡•ᴗ-)_
    time_base = bios_read_fdt(TIMEBASE);

    // Init lock mechanism o(´^｀)o
    init_locks();
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
    


    // TODO: Load tasks by either task id [p1-task3] or task name [p1-task4],
    //   and then execute them.

    // Infinite while loop, where CPU stays in a low-power state (QAQQQQQQQQQQQ)
    while (1)
    {
        // If you do non-preemptive scheduling, it's used to surrender control
        do_scheduler();

        // If you do preemptive scheduling, they're used to enable CSR_SIE and wfi
        // enable_preempt();
        // asm volatile("wfi");
    }

    return 0;
}
