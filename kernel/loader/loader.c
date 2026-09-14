#include <os/task.h>
#include <os/string.h>
#include <os/kernel.h>
#include <type.h>

uint64_t load_task_img(int taskid)
{
    /**
     * TODO:
     * 1. [p1-task3] load task from image via task id, and return its entrypoint
     * 2. [p1-task4] load task via task name, thus the arg should be 'char *taskname'
     */

    uint64_t task_entry = TASK_MEM_BASE+taskid*TASK_SIZE;
    unsigned task_sectors = NBYTES2SEC(TASK_SIZE);
    unsigned task_block = 1+(taskid+1)*task_sectors;

    bios_sd_read((unsigned)task_entry, task_sectors, task_block);

    return task_entry;
}