#include <os/lock.h>
#include <os/sched.h>
#include <os/list.h>
#include <atomic.h>

mutex_lock_t mlocks[LOCK_NUM];
static int mutex_count = 0;

/**
 * @brief 初始化锁，置UNLOCK，初始化阻塞队列，key复位
 * 
 */
void init_locks(void)
{
    /* TODO: [p2-task2] initialize mlocks */
    mutex_count = 0;

    for(int i=0; i<LOCK_NUM; ++i){
        spin_lock_init(&mlocks[i].lock);
        list_init(&mlocks[i].block_queue);
        mlocks[i].key = 0;
    }
}

/**
 * @brief 初始化锁为UNLOCK
 * 
 * @param lock 被操作的锁
 */
void spin_lock_init(spin_lock_t *lock)
{
    /* TODO: [p2-task2] initialize spin lock */
    lock->status = UNLOCKED;
}

/**
 * @brief 自旋锁-尝试占用
 * 
 * @param lock 锁
 * @return int 是否成功
 */
int spin_lock_try_acquire(spin_lock_t *lock)
{
    /* TODO: [p2-task2] try to acquire spin lock */
    uint32_t old_status = atomic_swap(LOCKED, (ptr_t)&lock->status); // 先返回旧状态，再置LOCK

    return old_status==UNLOCKED; // 旧状态可用，则占用锁
}

/**
 * @brief 自旋锁轮询
 * 
 * @param lock 锁
 */
void spin_lock_acquire(spin_lock_t *lock)
{
    /* TODO: [p2-task2] acquire spin lock */
    while(!spin_lock_try_acquire(lock)){

    }
}

/**
 * @brief 释放自旋锁
 * 
 * @param lock 锁
 */
void spin_lock_release(spin_lock_t *lock)
{
    /* TODO: [p2-task2] release spin lock */
    atomic_swap(UNLOCKED, (ptr_t)&lock->status);
}

/**
 * @brief 互斥锁初始化
 * 
 * @param key 用户设定的键值
 * @return int 互斥锁的下标
 */
int do_mutex_lock_init(int key)
{
    /* TODO: [p2-task2] initialize mutex lock */
    for(int i=0; i<mutex_count; ++i){
        if(mlocks[i].key == key){
            return i;
        }
    }

    if(mutex_count >= LOCK_NUM){
        return -1;
    }

    int mutex_idx = mutex_count++;

    spin_lock_init(&mlocks[mutex_idx].lock);
    list_init(&mlocks[mutex_idx].block_queue);
    mlocks[mutex_idx].key = key;

    return mutex_idx;
}

/**
 * @brief 尝试占用互斥锁
 * 
 * @param mlock_idx 互斥锁下标
 */
void do_mutex_lock_acquire(int mlock_idx)
{
    /* TODO: [p2-task2] acquire mutex lock */
    if(mlock_idx<0 || mlock_idx>=mutex_count){
        return;
    }

    mutex_lock_t *mutex = &mlocks[mlock_idx];

    if(!spin_lock_try_acquire(&mutex->lock)){
        do_block(
            &current_running->list,
            &mutex->block_queue
        );
    }
}

/**
 * @brief 释放互斥锁
 * 
 * @param mlock_idx 互斥锁下标
 */
void do_mutex_lock_release(int mlock_idx)
{
    /* TODO: [p2-task2] release mutex lock */
    if(mlock_idx<0 || mlock_idx>=mutex_count){
        return;
    }

    mutex_lock_t *mutex = &mlocks[mlock_idx];

    if(list_empty(&mutex->block_queue)){
        spin_lock_release(&mutex->lock);
        return;
    }

    do_unblock(mutex->block_queue.next);
}
