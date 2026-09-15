#include <common.h>
#include <asm.h>
#include <os/kernel.h>
#include <os/task.h>
#include <os/string.h>
#include <os/loader.h>
#include <type.h>

#define VERSION_BUF 50
#define TASK_NUM_ADDR 0x502001fa

int version = 2; // version must between 0 and 9
char buf[VERSION_BUF];

// Task info array
task_info_t tasks[TASK_MAXNUM];

static int bss_check(void)
{
    for (int i = 0; i < VERSION_BUF; ++i)
    {
        if (buf[i] != 0)
        {
            return 0;
        }
    }
    return 1;
}

static void init_jmptab(void)
{
    volatile long (*(*jmptab))() = (volatile long (*(*))())KERNEL_JMPTAB_BASE;

    jmptab[CONSOLE_PUTSTR]  = (long (*)())port_write;
    jmptab[CONSOLE_PUTCHAR] = (long (*)())port_write_ch;
    jmptab[CONSOLE_GETCHAR] = (long (*)())port_read_ch;
    jmptab[SD_READ]         = (long (*)())sd_read;
}

static int init_task_info(uint32_t app_info_offset, int tasknum)
{
    // TODO: [p1-task4] Init 'tasks' array via reading app-info sector
    // NOTE: You need to get some related arguments from bootblock first
    if(tasknum<=0 || tasknum>TASK_MAXNUM){
        return 0;
    }

    uint32_t info_size = tasknum*sizeof(task_info_t);
    uint32_t block_id = app_info_offset/SECTOR_SIZE;
    uint32_t block_offset = app_info_offset%SECTOR_SIZE;
    uint32_t info_sectors = NBYTES2SEC(block_offset+info_size);

    bios_sd_read(TASK_BUFFER_BASE, info_sectors, block_id);
    memcpy((uint8_t*)tasks, (uint8_t*)(TASK_BUFFER_BASE+block_offset), info_size);

    return tasknum;
}

static void read_task_name(char *taskname){
    int len = 0;
    while(1){
        int ch = bios_getchar();
        if(ch == -1){
            continue;
        }
        if(ch=='\r' || ch=='\n'){
            taskname[len] = '\0';
            bios_putstr("\n\r");
            return;
        }
        if(ch=='\b' || ch==127){
            if(len>0){
                len--;
                bios_putstr("\b \b");
            }
            continue;
        }
        if(len < TASK_NAME_LEN-1){
            taskname[len++] = ch;
            bios_putchar(ch);
        }
    }
}

/************************************************************/
/* Do not touch this comment. Reserved for future projects. */
/************************************************************/

int main(uint64_t app_info_offset, uint64_t tasknum_from_boot)
{
    // Check whether .bss section is set to zero
    int check = bss_check();

    // Init jump table provided by kernel and bios(ΦωΦ)
    init_jmptab();

    // Init task information (〃'▽'〃)
    int tasknum = init_task_info((uint32_t)app_info_offset, (int)tasknum_from_boot);

    // Output 'Hello OS!', bss check result and OS version
    char output_str[] = "bss check: _ version: _\n\r";
    char output_val[2] = {0};
    int i, output_val_pos = 0;

    output_val[0] = check ? 't' : 'f';
    output_val[1] = version + '0';
    for (i = 0; i < sizeof(output_str); ++i)
    {
        buf[i] = output_str[i];
        if (buf[i] == '_')
        {
            buf[i] = output_val[output_val_pos++];
        }
    }

    bios_putstr("Hello OS!\n\r");
    bios_putstr(buf);

    // TODO: Load tasks by either task id [p1-task3] or task name [p1-task4],
    //   and then execute them.

    // Infinite while loop, where CPU stays in a low-power state (QAQQQQQQQQQQQ)
    // while(1){
    //     int ch = bios_getchar();
    //     if(ch != -1){
    //         bios_putchar(ch);
    //     }
    // }
    while(1){
        // int ch = bios_getchar();
        // if(ch == -1){
        //     continue;
        // }
        // bios_putchar(ch);
        // if(ch<'0' || ch>'9'){
        //     continue;
        // }
        // int taskid = ch-'0';
        // if (taskid >= tasknum)
        // {
        //     bios_putstr("\n\rInvalid task id\n\r");
        //     continue;
        // }

        // uint64_t entry_addr = load_task_img(taskid);
        char taskname[TASK_NAME_LEN];

        bios_putstr("Please input task name: ");
        read_task_name(taskname);

        uint64_t entry_addr = load_task_img(taskname, tasknum);

        if(entry_addr == 0){
            bios_putstr("Task not found\n\r");
            continue;
        }
        void (*task_entry)(void) = (void (*)(void))entry_addr;
        task_entry();
        bios_putstr("\n\rTask finished\n\r");
    }

    return 0;
}
