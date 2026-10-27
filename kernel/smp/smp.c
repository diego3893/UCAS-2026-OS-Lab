#include <atomic.h>
#include <os/sched.h>
#include <os/smp.h>
#include <os/lock.h>
#include <os/kernel.h>

static spin_lock_t kernel_lock;
static spin_lock_t screen_lock;

void smp_init(void){
    /* TODO: P3-TASK3 multicore*/
    spin_lock_init(&kernel_lock);
    spin_lock_init(&screen_lock);
}

void wakeup_other_hart(void){
    /* TODO: P3-TASK3 multicore*/
    unsigned long hart_mask = 1UL<<1;

    // 全局初始化结果先于唤醒请求对从核可见
    asm volatile("fence" ::: "memory");
    send_ipi(&hart_mask);
}

void lock_kernel(void){
    /* TODO: P3-TASK3 multicore*/
    spin_lock_acquire(&kernel_lock);
}

void unlock_kernel(void){
    /* TODO: P3-TASK3 multicore*/
    spin_lock_release(&kernel_lock);
}

void lock_screen(void){
#if FINE_GRAINED_LOCK
    spin_lock_acquire(&screen_lock);
#endif
}

void unlock_screen(void){
#if FINE_GRAINED_LOCK
    spin_lock_release(&screen_lock);
#endif
}

int try_lock_screen(void){
#if FINE_GRAINED_LOCK
    return spin_lock_try_acquire(&screen_lock);
#else
    return 1;
#endif
}