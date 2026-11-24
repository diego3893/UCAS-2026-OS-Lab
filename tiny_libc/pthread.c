#include <pthread.h>
#include <stddef.h>

// 用户态包装函数
static void thread_entry(void (*start_routine)(void*), void *arg){
    start_routine(arg);
    sys_thread_exit();

    for(;;){
    }
}

/* TODO:[P4-task4] pthread_create/wait */
void pthread_create(pthread_t *thread, void (*start_routine)(void*), void *arg)
{
    /* TODO: [p4-task4] implement pthread_create */
    if(thread == NULL){
        return;
    }
    *thread = 0;
    if(start_routine == NULL){
        return;
    }
    *thread = sys_thread_create((uintptr_t)thread_entry, (uintptr_t)start_routine, (uintptr_t)arg);
}

int pthread_join(pthread_t thread)
{
    /* TODO: [p4-task4] implement pthread_join */
    if(thread <= 0){
        return -1;
    }
    return sys_thread_join(thread);
}