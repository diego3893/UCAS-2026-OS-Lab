#include <os/lock.h>
#include <os/sched.h>
#include <os/list.h>
#include <os/string.h>
#include <atomic.h>
#include <assert.h>

mutex_lock_t mlocks[LOCK_NUM];
static int mutex_count = 0;
static barrier_t barriers[BARRIER_NUM];
static condition_t conditions[CONDITION_NUM];
static mailbox_t mailboxes[MBOX_NUM];
static int mailbox_refs[MBOX_NUM][NUM_MAX_TASK]; // 每个PCB槽位对各邮箱的打开次数，退出时清零

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
        mlocks[i].owner = 0;
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
    mlocks[mutex_idx].owner = 0;

    return mutex_idx;
}

static void acquire_mutex(mutex_lock_t *mutex){
    if(spin_lock_try_acquire(&mutex->lock)){
        mutex->owner = current_running->pid;
        return;
    }

    do_block(&current_running->list, &mutex->block_queue);

    assert(mutex->owner==current_running->pid); // 检查owner是否被正确移交
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

    acquire_mutex(&mlocks[mlock_idx]);
}

static void release_mutex(mutex_lock_t *mutex){
    if(list_empty(&mutex->block_queue)){
        mutex->owner = 0;
        spin_lock_release(&mutex->lock);
        return;
    }

    list_node_t *node = mutex->block_queue.next;
    pcb_t *next_owner = list_entry(node, pcb_t, list);
    mutex->owner = next_owner->pid; // 交接所有权
    do_unblock(node);
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
    if(mutex->owner != current_running->pid){
        return;
    }
    release_mutex(mutex);
}

void do_mutex_release_all(pid_t pid){
    for(int i=0; i<mutex_count; ++i){
        if(mlocks[i].owner == pid){
            release_mutex(&mlocks[i]);
        }
    }
}

///////////////////////////////// barrier

void init_barriers(void){
    for(int i=0; i<BARRIER_NUM; ++i){
        barriers[i].used = 0;
        barriers[i].key = 0;
        barriers[i].goal = 0;
        barriers[i].arrived = 0;
        list_init(&barriers[i].block_queue);
    }
}

int do_barrier_init(int key, int goal){
    if(goal<=0 || goal>NUM_MAX_TASK){
        return -1;
    }

    // 相同key返回同一个Barrier，不重置正在使用的计数
    for(int i=0; i<BARRIER_NUM; ++i){
        if(barriers[i].used && barriers[i].key==key){
            if(barriers[i].goal != goal){
                return -1; // 数据冲突
            }

            return i;
        }
    }

    for(int i=0; i<BARRIER_NUM; ++i){
        if(!barriers[i].used){
            barriers[i].used = 1;
            barriers[i].key = key;
            barriers[i].goal = goal;
            barriers[i].arrived = 0;
            list_init(&barriers[i].block_queue);
            return i;
        }
    }
    return -1;
}

void do_barrier_wait(int bar_idx){
    if(bar_idx<0 || bar_idx>=BARRIER_NUM || !barriers[bar_idx].used){
        return;
    }

    barrier_t *barrier = &barriers[bar_idx];

    barrier->arrived++;

    if(barrier->arrived == barrier->goal){
        // 先重置本轮计数，再唤醒等待者。
        barrier->arrived = 0;

        while(!list_empty(&barrier->block_queue)){
            do_unblock(barrier->block_queue.next);
        }

        return;
    }

    do_block(&current_running->list, &barrier->block_queue);
}

void do_barrier_destroy(int bar_idx){
    if(bar_idx<0 || bar_idx>=BARRIER_NUM || !barriers[bar_idx].used){
        return;
    }

    barrier_t *barrier = &barriers[bar_idx];

    // 有进程正在等待时，暂不销毁
    if(barrier->arrived!=0 || !list_empty(&barrier->block_queue)){
        return;
    }

    barrier->used = 0;
    barrier->key = 0;
    barrier->goal = 0;
    barrier->arrived = 0;

    list_init(&barrier->block_queue);
}

/**
 * @brief 从barrier中删除一个进程
 * 
 * @param pcb_node 进程pcb
 */
void do_barrier_cancel_wait(list_node_t *pcb_node){
    for(int i=0; i<BARRIER_NUM; ++i){
        if(!barriers[i].used){
            continue;
        }

        barrier_t *barrier = &barriers[i];

        for(list_node_t *node=barrier->block_queue.next; node!=&barrier->block_queue; node=node->next){
            if(node == pcb_node){
                assert(barrier->arrived>0);

                list_del(pcb_node);
                barrier->arrived--;

                return;
            }
        }
    }
}

////////////////// condition

/**
 * @brief 让正在运行的进程等待condition
 * 
 * @param condition 条件变量
 * @param mutex 互斥锁
 */
static void wait_condition(condition_t *condition, mutex_lock_t *mutex){
    assert(mutex->owner==current_running->pid);

    current_running->status = TASK_BLOCKED;
    list_add_tail(&current_running->list, &condition->block_queue);
    release_mutex(mutex);
    do_scheduler();
    // 被唤醒后，重新获取调用前持有的互斥锁
    acquire_mutex(mutex);
}

/**
 * @brief 唤醒一个等待进程
 * 
 * @param condition 条件变量
 */
static void signal_condition(condition_t *condition){
    if(!list_empty(&condition->block_queue)){
        do_unblock(condition->block_queue.next);
    }
}

/**
 * @brief 唤醒所有等待进程
 * 
 * @param condition 条件变量
 */
static void broadcast_condition(condition_t *condition){
    while(!list_empty(&condition->block_queue)){
        do_unblock(condition->block_queue.next);
    }
}

void init_conditions(void){
    for(int i=0; i<CONDITION_NUM; ++i){
        conditions[i].used = 0;
        conditions[i].key = 0;
        list_init(&conditions[i].block_queue);
    }
}

int do_condition_init(int key){
    for(int i=0; i<CONDITION_NUM; ++i){
        if(conditions[i].used && conditions[i].key==key){
            return i;
        }
    }

    for(int i=0; i<CONDITION_NUM; ++i){
        if(!conditions[i].used){
            conditions[i].used = 1;
            conditions[i].key = key;
            list_init(&conditions[i].block_queue);
            return i;
        }
    }

    return -1;
}

/**
 * @brief 执行条件等待
 * 
 * @param cond_idx 条件变量下标
 * @param mutex_idx 互斥锁下标
 */
void do_condition_wait(int cond_idx, int mutex_idx){
    if(cond_idx<0 || cond_idx>=CONDITION_NUM || !conditions[cond_idx].used){
        return;
    }
    if(mutex_idx<0 || mutex_idx>=mutex_count){
        return;
    }

    mutex_lock_t *mutex = &mlocks[mutex_idx];
    if(mutex->owner != current_running->pid){
        return;
    }
    wait_condition(&conditions[cond_idx], mutex);
}

void do_condition_signal(int cond_idx){
    if(cond_idx<0 || cond_idx>=CONDITION_NUM || !conditions[cond_idx].used){
        return;
    }
    signal_condition(&conditions[cond_idx]);
}

void do_condition_broadcast(int cond_idx){
    if(cond_idx<0 || cond_idx>=CONDITION_NUM || !conditions[cond_idx].used){
        return;
    }
    broadcast_condition(&conditions[cond_idx]);
}

void do_condition_destroy(int cond_idx){
    if(cond_idx<0 || cond_idx>=CONDITION_NUM || !conditions[cond_idx].used){
        return;
    }

    condition_t *condition = &conditions[cond_idx];

    // 有进程仍在条件队列中等待时，暂不销毁
    if(!list_empty(&condition->block_queue)){
        return;
    }

    condition->used = 0;
    condition->key = 0;
    list_init(&condition->block_queue);
}

//////////// mailbox

/**
 * @brief 根据pid找pcb的下标，用于访问mbox_refs
 * 
 * @param pid 进程pid
 * @return int 下标，-1为失败
 */
static int find_process_slot(pid_t pid){
    if(pid <= 0){
        return -1;
    }
    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(pcb[i].pid == pid){
            return i;
        }
    }
    return -1;
}

static mailbox_t *get_opened_mailbox(int mbox_idx, int slot){
    if(mbox_idx<0 || mbox_idx>=MBOX_NUM || slot<0 || slot>=NUM_MAX_TASK){
        return NULL;
    }
    if(!mailboxes[mbox_idx].used || mailbox_refs[mbox_idx][slot]<=0){
        return NULL;
    }
    return &mailboxes[mbox_idx];
}

static void reset_mailbox(int mbox_idx){
    mailbox_t *mailbox = &mailboxes[mbox_idx];

    if(mailbox->used){
        assert(mailbox->ref_count==0);
        assert(mailbox->mutex.owner==0);
        assert(list_empty(&mailbox->mutex.block_queue));
        assert(list_empty(&mailbox->not_full.block_queue));
        assert(list_empty(&mailbox->not_empty.block_queue));
    }

    memset(mailbox, 0, sizeof(mailbox_t));
    spin_lock_init(&mailbox->mutex.lock);
    list_init(&mailbox->mutex.block_queue);
    mailbox->not_full.used = 1;
    mailbox->not_empty.used = 1;
    list_init(&mailbox->not_full.block_queue);
    list_init(&mailbox->not_empty.block_queue);

    for(int i=0; i<NUM_MAX_TASK; ++i){
        mailbox_refs[mbox_idx][i] = 0;
    }
}

void init_mbox(void){
    for(int i=0; i<MBOX_NUM; ++i){
        reset_mailbox(i);
    }
}

int do_mbox_open(char *name){
    int slot = find_process_slot(current_running->pid);
    if(slot<0 || name==NULL){
        return -1;
    }

    int length = 0;
    while(length<MBOX_NAME_LEN && name[length]!='\0'){
        length++;
    }
    if(length==0 || length>=MBOX_NAME_LEN){
        return -1;
    }

    int mbox_idx = -1;
    for(int i=0; i<MBOX_NUM; ++i){
        if(mailboxes[i].used && strcmp(mailboxes[i].name, name)==0){
            mbox_idx = i;
            break;
        }
    }
    if(mbox_idx < 0){
        for(int i=0; i<MBOX_NUM; ++i){
            if(!mailboxes[i].used){
                mbox_idx = i;
                break;
            }
        }
        if(mbox_idx < 0){
            return -1;
        }
        // 复用空闲的mbox，清空后写入name
        reset_mailbox(mbox_idx);
        mailbox_t *mailbox = &mailboxes[mbox_idx];
        memcpy((uint8_t*)mailbox->name, (const uint8_t*)name, length+1);
        mailbox->used = 1;
    }

    mailbox_t *mailbox = &mailboxes[mbox_idx];
    if(mailbox->ref_count == INT32_MAX){
        return -1;
    }
    // 当前进程正在使用该mbox
    mailbox_refs[mbox_idx][slot]++;
    mailbox->ref_count++;
    return mbox_idx;
}

/**
 * @brief 关闭当前进程的引用（原子操作），并在mbox无引用时重置
 * 
 * @param mbox_idx mbox下标
 */
void do_mbox_close(int mbox_idx){
    int slot = find_process_slot(current_running->pid);
    mailbox_t *mailbox = get_opened_mailbox(mbox_idx, slot);
    if(mailbox == NULL){
        return;
    }

    acquire_mutex(&mailbox->mutex);
    mailbox_refs[mbox_idx][slot]--;
    mailbox->ref_count--;

    int should_reset = mailbox->ref_count==0;
    release_mutex(&mailbox->mutex);
    if(should_reset){
        reset_mailbox(mbox_idx);
    }
}

/**
 * @brief 在允许时，写mbox
 * 
 * @param mbox_idx mbox下标
 * @param msg 写入的数据
 * @param msg_length 写入数据的长度
 * @return int -1为非法操作，否则返回阻塞数
 */
int do_mbox_send(int mbox_idx, void *msg, int msg_length){
    int slot = find_process_slot(current_running->pid);
    mailbox_t *mailbox = get_opened_mailbox(mbox_idx, slot);

    if(mailbox==NULL || msg_length<0 || msg_length>MAX_MBOX_LENGTH || (msg_length>0 && msg==NULL)){
        return -1;
    }
    if(msg_length == 0){
        return 0;
    }

    int blocked = 0;
    const uint8_t *src = (const uint8_t*)msg;

    acquire_mutex(&mailbox->mutex);

    while(MAX_MBOX_LENGTH-mailbox->used_bytes < msg_length){ // 剩余空间不足，阻塞直到空间足够。用WHILE！
        blocked++;
        wait_condition(&mailbox->not_full, &mailbox->mutex);
    }

    for(int i=0; i<msg_length; ++i){
        mailbox->buffer[mailbox->write_pos] = src[i];
        mailbox->write_pos = (mailbox->write_pos+1)%MAX_MBOX_LENGTH; // 循环利用
    }
    mailbox->used_bytes += msg_length;

    broadcast_condition(&mailbox->not_empty); // 广播当前mbox非空
    release_mutex(&mailbox->mutex);

    return blocked;
}

/**
 * @brief 允许时，读mbox
 * 
 * @param mbox_idx mbox下标
 * @param msg 读取的数据
 * @param msg_length 读取长度
 * @return int -1非法，否则返回阻塞数
 */
int do_mbox_recv(int mbox_idx, void *msg, int msg_length){
    int slot = find_process_slot(current_running->pid);
    mailbox_t *mailbox = get_opened_mailbox(mbox_idx, slot);

    if(mailbox==NULL || msg_length<0 || msg_length>MAX_MBOX_LENGTH || (msg_length>0 && msg==NULL)){
        return -1;
    }

    if(msg_length == 0){
        return 0;
    }

    int blocked = 0;
    uint8_t *dest = (uint8_t*)msg;

    acquire_mutex(&mailbox->mutex);

    while(mailbox->used_bytes < msg_length){
        blocked++;
        wait_condition(&mailbox->not_empty, &mailbox->mutex);
    }

    for(int i=0; i<msg_length; ++i){
        dest[i] = mailbox->buffer[mailbox->read_pos];
        mailbox->read_pos = (mailbox->read_pos+1)%MAX_MBOX_LENGTH;
    }

    mailbox->used_bytes -= msg_length;

    broadcast_condition(&mailbox->not_full);
    release_mutex(&mailbox->mutex);

    return blocked;
}

/**
 * @brief 释放所有和pid相关的mbox引用
 * 
 * @param pid 进程pid
 */
void do_mbox_release_all(pid_t pid){
    int slot = find_process_slot(pid);

    if(slot < 0){
        return;
    }

    for(int i=0; i<MBOX_NUM; ++i){
        mailbox_t *mailbox = &mailboxes[i];

        if(!mailbox->used){
            continue;
        }

        if(mailbox->mutex.owner == pid){
            release_mutex(&mailbox->mutex);
        }

        int references = mailbox_refs[i][slot];

        if(references == 0){
            continue;
        }

        assert(mailbox->ref_count>=references);

        mailbox_refs[i][slot] = 0;
        mailbox->ref_count -= references;

        if(mailbox->ref_count == 0){
            reset_mailbox(i);
        }
    }
}