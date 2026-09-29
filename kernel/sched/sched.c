#include <os/list.h>
#include <os/lock.h>
#include <os/sched.h>
#include <os/time.h>
#include <os/mm.h>
#include <screen.h>
#include <printk.h>
#include <assert.h>

pcb_t pcb[NUM_MAX_TASK];
const ptr_t pid0_stack = INIT_KERNEL_STACK + PAGE_SIZE;
pcb_t pid0_pcb = {
    .pid = 0,
    .kernel_sp = (ptr_t)pid0_stack,
    .user_sp = (ptr_t)pid0_stack
};

LIST_HEAD(ready_queue);
LIST_HEAD(sleep_queue);

/* global process id */
pid_t process_id = 1;

void do_scheduler(void)
{
    // TODO: [p2-task3] Check sleep queue to wake up PCBs
    check_sleeping();

    /************************************************************/
    /* Do not touch this comment. Reserved for future projects. */
    /************************************************************/

    // TODO: [p2-task1] Modify the current_running pointer.
    pcb_t *prev = current_running;
    if(prev->pid!=0 && prev->status==TASK_RUNNING){ //pid0是内核进程，不进入调度队列
        prev->status = TASK_READY;
        list_add_tail(&prev->list, &ready_queue);
    }

    while(list_empty(&ready_queue)){
        check_sleeping();
    }

    list_node_t *node = list_pop_front(&ready_queue);
    pcb_t *next = list_entry(node, pcb_t, list);

    next->status = TASK_RUNNING;
    current_running = next;

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

    list_del(pcb_node);

    task->status = TASK_READY;
    list_add_tail(pcb_node, &ready_queue);
}
