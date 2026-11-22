#include <os/task.h>
#include <os/string.h>
#include <os/kernel.h>
#include <type.h>
#include <os/mm.h>
#include <os/loader.h>

#if 0
uint64_t load_task_img(const char *taskname, int tasknum){
    /**
     * TODO:
     * 1. [p1-task3] load task from image via task id, and return its entrypoint
     * 2. [p1-task4] load task via task name, thus the arg should be 'char *taskname'
     */

    // uint64_t task_entry = TASK_MEM_BASE+taskid*TASK_SIZE;
    // unsigned task_sectors = NBYTES2SEC(TASK_SIZE);
    // unsigned task_block = 1+(taskid+1)*task_sectors;

    // bios_sd_read((unsigned)task_entry, task_sectors, task_block);

    // return task_entry;
    int taskid = -1;

    for(int i=0; i<tasknum; ++i){
        if(strcmp(taskname, tasks[i].task_name) == 0){
            taskid = i;
            break;
        }
    }
    if(taskid == -1){
        return 0;
    }

    task_info_t *task = &tasks[taskid];

    if(task->task_size==0 || task->task_size>TASK_SIZE){
        return 0;
    }

    uint32_t relative_block = task->task_offset/SECTOR_SIZE;
    uint32_t block_offset = task->task_offset%SECTOR_SIZE;
    uint32_t block_id = USER_IMAGE_START_SECTOR+relative_block;
    uint32_t task_sectors = NBYTES2SEC(block_offset+task->task_size);

    bios_sd_read(TASK_BUFFER_BASE, task_sectors, block_id);

    memset((void*)task->task_entry, 0, TASK_SIZE); // 加载时清空app槽位，即清空bss
    memcpy((uint8_t*)task->task_entry, (uint8_t*)(TASK_BUFFER_BASE+block_offset), task->task_size);

    asm volatile("fence.i" ::: "memory");
    
    return task->task_entry;
}
#endif

static uint8_t sector_buffer[SECTOR_SIZE] __attribute__((aligned(SECTOR_SIZE)));

int read_image_bytes(void *dest, uint64_t disk_offset, size_t len){
    uint8_t *out = (uint8_t*)dest;
    while(len > 0){ // 逐扇区处理，注意未对齐时的offset
        uint64_t block = disk_offset/SECTOR_SIZE;
        unsigned offset = disk_offset%SECTOR_SIZE;
        unsigned count = SECTOR_SIZE-offset;

        if(count > len){
            count = len;
        }

        // BIOS接收paddr，内核使用kva，需要转换
        if(block>UINT32_MAX ||
           bios_sd_read((unsigned)kva2pa((uintptr_t)sector_buffer),
                        1, (unsigned)block)<0){
            return -1;
        }

        memcpy(out, sector_buffer+offset, count);

        out += count;
        disk_offset += count;
        len -= count;
    }

    return 0;
}

/**
 * @brief 加载用户镜像
 * 
 * @param taskname app名称
 * @param tasknum app总数
 * @param pgdir 根页表
 * @return uint64_t 失败为0，否则为程序入口
 */
uint64_t load_task_img(const char *taskname, int tasknum, uintptr_t pgdir){
    int taskid = -1;

    for(int i=0; i<tasknum; ++i){
        if(strcmp(taskname, tasks[i].task_name) == 0){
            taskid = i;
            break;
        }
    }

    if(taskid < 0){
        return 0;
    }

    task_info_t *task = &tasks[taskid];
    uintptr_t stack_bottom = USER_STACK_ADDR-PAGE_SIZE;

    if(task->task_entry<PAGE_SIZE ||
       task->task_entry>=stack_bottom ||
       task->task_memsz==0 ||
       task->task_size>task->task_memsz ||
       task->task_memsz>stack_bottom-task->task_entry){
        return 0;
    }

    uintptr_t end = task->task_entry+task->task_memsz;
    // 按运行时大小建立映射
    for(uintptr_t va=ROUNDDOWN(task->task_entry, PAGE_SIZE); va<end; va+=PAGE_SIZE){
        if(alloc_page_helper(va, pgdir) == 0){
            return 0;
        }
    }

    // 文件内容逐页拷入
    for(uint64_t copied=0; copied<task->task_size; ){
        uintptr_t va = task->task_entry+copied;
        unsigned offset = va&(PAGE_SIZE-1);
        size_t count = PAGE_SIZE-offset; // 字节数

        if(count > task->task_size-copied){
            count = task->task_size-copied;
        }

        PTE *entry = walk_user_pte(pgdir, va, 0);
        if(entry==NULL || !(*entry&_PAGE_PRESENT)){
            return 0;
        }

        uintptr_t dest = pa2kva(get_pa(*entry))+offset;
        uint64_t disk_offset = (uint64_t)USER_IMAGE_START_SECTOR*SECTOR_SIZE + task->task_offset+copied;

        if(read_image_bytes((void*)dest, disk_offset, count) != 0){
            return 0;
        }

        copied += count;
    }

    local_flush_icache_all();
    return task->task_entry;
}