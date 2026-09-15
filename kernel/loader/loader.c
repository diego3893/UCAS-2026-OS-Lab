#include <os/task.h>
#include <os/string.h>
#include <os/kernel.h>
#include <type.h>

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

    memcpy((uint8_t*)task->task_entry, (uint8_t*)(TASK_BUFFER_BASE+block_offset), task->task_size);

    return task->task_entry;
}