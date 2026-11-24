#include <atomic.h>
#include <os/sched.h>
#include <os/smp.h>
#include <os/lock.h>
#include <os/kernel.h>
#include <pgtable.h>

static spin_lock_t kernel_lock;
static spin_lock_t screen_lock;

static unsigned tlb_request[NR_CPUS]; // 任务编号
static unsigned tlb_ack[NR_CPUS]; // ack确认编号
static unsigned tlb_online[NR_CPUS]; // 是否可以刷新tlb

// 双核tlb刷新不依赖于内核锁
static void service_tlb_request(void){
    unsigned cpu = (unsigned)get_current_cpu_id();
    unsigned request = __atomic_load_n(&tlb_request[cpu], __ATOMIC_ACQUIRE);

    if(request != __atomic_load_n(&tlb_ack[cpu], __ATOMIC_RELAXED)){
        local_flush_tlb_all();
        __atomic_store_n(&tlb_ack[cpu], request, __ATOMIC_RELEASE);
    }
}

// 由当前持有内核锁的核心调用，另一个核心在时间中断时刷新tlb
void flush_tlb_all(void){
    unsigned self = (unsigned)get_current_cpu_id();
    unsigned targets[NR_CPUS] = {0};
    unsigned mask = 0;

    for(int cpu=0; cpu<NR_CPUS; ++cpu){ // 对非调用核做request处理
        if((unsigned)cpu==self || !__atomic_load_n(&tlb_online[cpu], __ATOMIC_ACQUIRE)){
            continue;
        }
        targets[cpu] = __atomic_add_fetch(&tlb_request[cpu], 1U, __ATOMIC_RELEASE);
        mask |= 1U<<cpu; //掩码保存核号
    }

    local_flush_tlb_all();

    for(int cpu=0; cpu<NR_CPUS; ++cpu){
        if(!(mask&(1U<<cpu))){
            continue;
        }
        while(__atomic_load_n(&tlb_ack[cpu], __ATOMIC_ACQUIRE) != targets[cpu]){ // 等待另一个核刷新tlb
            asm volatile("nop");
        }
    }
}

void smp_init(void){
    /* TODO: P3-TASK3 multicore*/
    spin_lock_init(&kernel_lock);
    spin_lock_init(&screen_lock);
    for(int cpu=0; cpu<NR_CPUS; ++cpu){
        tlb_request[cpu] = 0;
        tlb_ack[cpu] = 0;
        tlb_online[cpu] = 0;
    }
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
    unsigned cpu = (unsigned)get_current_cpu_id();
    __atomic_store_n(&tlb_online[cpu], 1U, __ATOMIC_RELEASE);
    while(!spin_lock_try_acquire(&kernel_lock)){ // 无法获取锁时也检查tlb刷新
        service_tlb_request();
    }
    service_tlb_request();
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