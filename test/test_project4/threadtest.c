#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static int shared_value;
static pid_t worker_pid;

static void worker(void *arg){
    int value = *(int*)arg;

    __atomic_store_n(&shared_value, value+1, __ATOMIC_RELEASE);
    __atomic_store_n(&worker_pid, sys_getpid(), __ATOMIC_RELEASE);

    // 返回后应由thread_entry自动退出当前线程。
}

int main(int argc, char **argv){
    (void)argc;
    (void)argv;

    pid_t pid = sys_getpid();

    for(int round=0; round<20; ++round){
        int arg = round;
        pthread_t thread = 0;

        pthread_create(&thread, worker, &arg);

        if(thread==0 || pthread_join(thread)!=0){
            printf("threadtest: create/join FAIL\n");
            return -1;
        }

        if(__atomic_load_n(&shared_value, __ATOMIC_ACQUIRE)!=round+1 ||
           __atomic_load_n(&worker_pid, __ATOMIC_ACQUIRE)!=pid){
            printf("threadtest: shared address space FAIL\n");
            return -1;
        }
    }

    printf("threadtest pid=%d PASS\n", pid);
    return 0;
}