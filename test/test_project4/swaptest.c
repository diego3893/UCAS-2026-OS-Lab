#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define TEST_PAGES 64
#define TEST_PAGE_SIZE 4096
#define WORDS_PER_PAGE (TEST_PAGE_SIZE/sizeof(uint64_t))
#define TEST_BASE 0x10000000UL

static uint64_t expected_value(pid_t pid, int page, int word){
    return 0x1234567800000000UL^
           ((uint64_t)(uint32_t)pid<<32)^
           ((uint64_t)page<<16)^
           (uint64_t)word;
}

int main(int argc, char **argv){
    int row = argc>1 ? atoi(argv[1]) : 1;
    pid_t pid = sys_getpid();

    sys_move_cursor(0, row);
    printf("swaptest pid=%d start\n", pid);

    // 写满64个虚拟页，超过每进程32页的驻留配额。
    for(int page=0; page<TEST_PAGES; ++page){
        volatile uint64_t *data =
            (volatile uint64_t*)(TEST_BASE+(uintptr_t)page*TEST_PAGE_SIZE);

        for(int word=0; word<WORDS_PER_PAGE; ++word){
            data[word] = expected_value(pid, page, word);
        }

        sys_yield();
    }

    // 先反向、再正向读取，检查整页内容。
    for(int pass=0; pass<2; ++pass){
        for(int i=0; i<TEST_PAGES; ++i){
            int page = pass==0 ? TEST_PAGES-1-i : i;
            volatile uint64_t *data =
                (volatile uint64_t*)(TEST_BASE+(uintptr_t)page*TEST_PAGE_SIZE);

            for(int word=0; word<WORDS_PER_PAGE; ++word){
                uint64_t expected = expected_value(pid, page, word);
                uint64_t actual = data[word];

                if(actual != expected){
                    printf("FAIL pid=%d page=%d word=%d\n", pid, page, word);
                    printf("expected=%lx actual=%lx\n", expected, actual);
                    return 1;
                }
            }

            sys_yield();
        }
    }

    printf("swaptest pid=%d PASS\n", pid);
    return 0;
}