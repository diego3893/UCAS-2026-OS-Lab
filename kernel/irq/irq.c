#include <os/irq.h>
#include <os/time.h>
#include <os/sched.h>
#include <os/string.h>
#include <os/kernel.h>
#include <printk.h>
#include <assert.h>
#include <screen.h>
#include <csr.h>
#include <os/smp.h>
#include <os/mm.h>

handler_t irq_table[IRQC_COUNT];
handler_t exc_table[EXCC_COUNT];

void interrupt_helper(regs_context_t *regs, uint64_t stval, uint64_t scause)
{
    // TODO: [p2-task3] & [p2-task4] interrupt handler.
    // call corresponding handler by the value of `scause`
    if(current_running->pid!=0 && current_running->kill_pending){ // 优先检查是否需要杀进程
        do_exit();
    }
    uint64_t cause = scause&(~SCAUSE_IRQ_FLAG);

    if(scause&SCAUSE_IRQ_FLAG){
        if(cause<IRQC_COUNT && irq_table[cause]!=0){
            irq_table[cause](regs, stval, scause);
        }else{
            handle_other(regs, stval, scause);
        }
    }else{
        if(cause<EXCC_COUNT && exc_table[cause]!=0){
            exc_table[cause](regs, stval, scause);
        }else{
            handle_other(regs, stval, scause);
        }
    }
}

void handle_irq_timer(regs_context_t *regs, uint64_t stval, uint64_t scause)
{
    // TODO: [p2-task4] clock interrupt handler.
    // Note: use bios_set_timer to reset the timer and remember to reschedule
    
    #if FINE_GRAINED_LOCK
        // 定时器只尝试获取屏幕锁，无法获取则跳过本次刷新
        if(try_lock_screen()){
            unlock_kernel();

            screen_reflush();

            unlock_screen();
            lock_kernel();
        }
    #else
        screen_reflush();
    #endif

    check_sleeping();
    bios_set_timer(get_ticks()+TIMER_INTERVAL);
    if(current_running->pid!=0 && current_running->ticks_left>1){
        current_running->ticks_left--;
        return;
    }
    do_scheduler();
}

void handle_irq_ext(regs_context_t *regs, uint64_t stval, uint64_t scause)
{
    // TODO: [p5-task4] external interrupt handler.
    // Note: plic_claim and plic_complete will be helpful ...
}

void init_exception()
{
    /* TODO: [p2-task3] initialize exc_table */
    /* NOTE: handle_syscall, handle_other, etc.*/
    for(int i=0; i<EXCC_COUNT; ++i){
        exc_table[i] = handle_other;
    }
    exc_table[EXCC_SYSCALL] = handle_syscall;
    exc_table[EXCC_INST_PAGE_FAULT] = handle_page_fault;
    exc_table[EXCC_LOAD_PAGE_FAULT] = handle_page_fault;
    exc_table[EXCC_STORE_PAGE_FAULT] = handle_page_fault;

    /* TODO: [p2-task4] initialize irq_table */
    /* NOTE: handle_int, handle_other, etc.*/
    for(int i=0; i<IRQC_COUNT; ++i){
        irq_table[i] = handle_other;
    }
    irq_table[IRQC_S_TIMER] = handle_irq_timer;

    /* TODO: [p2-task3] set up the entrypoint of exceptions */
    setup_exception();
}

void handle_other(regs_context_t *regs, uint64_t stval, uint64_t scause)
{
    char* reg_name[] = {
        "zero "," ra  "," sp  "," gp  "," tp  ",
        " t0  "," t1  "," t2  ","s0/fp"," s1  ",
        " a0  "," a1  "," a2  "," a3  "," a4  ",
        " a5  "," a6  "," a7  "," s2  "," s3  ",
        " s4  "," s5  "," s6  "," s7  "," s8  ",
        " s9  "," s10 "," s11 "," t3  "," t4  ",
        " t5  "," t6  "
    };
    for (int i = 0; i < 32; i += 3) {
        for (int j = 0; j < 3 && i + j < 32; ++j) {
            printk("%s : %016lx ",reg_name[i+j], regs->regs[i+j]);
        }
        printk("\n\r");
    }
    printk("sstatus: 0x%lx sbadaddr: 0x%lx scause: %lu\n\r",
           regs->sstatus, regs->sbadaddr, regs->scause);
    printk("sepc: 0x%lx\n\r", regs->sepc);
    printk("tval: 0x%lx cause: 0x%lx\n", stval, scause);
    assert(0);
}

static void terminate_faulting_process(regs_context_t *regs, uint64_t stval, uint64_t scause, const char *reason){
    printk("> [PAGE FAULT] pid=%d cause=%lu va=0x%lx sepc=0x%lx\n", current_running->tgid, scause, stval, regs->sepc);
    printk("> [PAGE FAULT] %s\n", reason);
    // 退出发生错误缺页的进程
    do_exit();
}

void handle_page_fault(regs_context_t *regs, uint64_t stval, uint64_t scause){
    uint64_t cause = scause&~SCAUSE_IRQ_FLAG;
    // SPP=1为内核发生缺页，此时应作为错误处理
    if((regs->sstatus&SR_SPP) || current_running->pid==0){
        handle_other(regs, stval, scause);
        return;
    }

    uintptr_t pgdir = current_running->pgdir;

    // 无根页表或者高半区缺页按照错误处理
    if(pgdir==0 || stval>=USER_VA_LIMIT){
        terminate_faulting_process(regs, stval, scause, "invalid user address");
        return;
    }

    uint64_t access;
    if(cause == EXCC_INST_PAGE_FAULT){
        access = _PAGE_EXEC;
    }else if(cause == EXCC_LOAD_PAGE_FAULT){
        access = _PAGE_READ;
    }else if(cause == EXCC_STORE_PAGE_FAULT){
        access = _PAGE_WRITE;
    }else{
        handle_other(regs, stval, scause);
        return;
    }

    uintptr_t va = ROUNDDOWN(stval, PAGE_SIZE);
    PTE *entry = walk_user_pte(pgdir, va, 0); // 只查PTE，不建立次级页表

    if(entry!=NULL && (*entry&_PAGE_PRESENT)){
        PTE value = *entry;
        // 已有映射必须允许用户访问，并满足本次权限; 拒绝R=0 W=1
        if(!(value&_PAGE_USER) || (value&access)!=access ||
           ((value&_PAGE_WRITE) && !(value&_PAGE_READ))){
            terminate_faulting_process(regs, stval, scause, "page permission denied");
            return;
        }

        uint64_t needed = _PAGE_ACCESSED;
        if(cause == EXCC_STORE_PAGE_FAULT){
            needed |= _PAGE_DIRTY;
        }
        // 因A/D未设置而触发缺页，应补上标志位
        if((value&needed) != needed){
            set_attribute(entry, needed);
            flush_tlb_all();
            return;
        }
        // 映射、权限、A/D均已满足仍然缺页，视为错误
        // task4: 其他线程可能修复这个问题，因此刷新tlb重试
        flush_tlb_all();
        return;
    }

    // 程序应由loader加载完成
    if(cause == EXCC_INST_PAGE_FAULT){
        terminate_faulting_process(regs, stval, scause, "instruction page is not mapped");
        return;
    }
    // // 非0无效页，task2无换页，先报错
    // if(entry!=NULL && *entry!=0){
    //     terminate_faulting_process(regs, stval, scause, "unsupported invalid PTE");
    //     return;
    // }
    // task3: 带SWAPPED标记的无效PTE交给换入逻辑处理。
    if(entry!=NULL && *entry!=0 && !(*entry&_PAGE_SWAPPED)){
        terminate_faulting_process(regs, stval, scause, "unsupported invalid PTE");
        return;
    }
    if(swap_handle_fault(pgdir, va, access) != 0){
        terminate_faulting_process(regs, stval, scause, "cannot allocate or swap user page");
        return;
    }
}