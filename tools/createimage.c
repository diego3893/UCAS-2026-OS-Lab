#include <assert.h>
#include <elf.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define KERNEL_IMAGE_FILE "./kernel_image"
#define USER_IMAGE_FILE "./user_image"
#define ARGS "[--extended] [--vm] <bootblock> <executable-file> ..."

#define SECTOR_SIZE 512
#define BOOT_LOADER_SIG_OFFSET 0x1fe
#define OS_SIZE_LOC (BOOT_LOADER_SIG_OFFSET - 2)
#define TASK_NUM_LOC (OS_SIZE_LOC-2)
#define APP_INFO_OFFSET_LOC (TASK_NUM_LOC-6)
#define BOOT_LOADER_SIG_1 0x55
#define BOOT_LOADER_SIG_2 0xaa
#define TASK_NAME_LEN 16

#define NBYTES2SEC(nbytes) (((nbytes) / SECTOR_SIZE) + ((nbytes) % SECTOR_SIZE != 0))

#define TASK_SIZE 0x10000

/* TODO: [p1-task4] design your own task_info_t */
typedef struct {
    char task_name[TASK_NAME_LEN];
    uint32_t task_offset;
    uint32_t task_size;
    uint64_t task_entry;
} task_info_t;

#define TASK_MAXNUM 16
static task_info_t taskinfo[TASK_MAXNUM];

/* structure to store command line options */
static struct {
    int vm;
    int extended;
} options;

/* prototypes of local functions */
static void create_image(int nfiles, char *files[]);
static void error(char *fmt, ...);
static void read_ehdr(Elf64_Ehdr *ehdr, FILE *fp);
static void read_phdr(Elf64_Phdr *phdr, FILE *fp, int ph, Elf64_Ehdr ehdr);
static uint64_t get_entrypoint(Elf64_Ehdr ehdr);
static uint32_t get_filesz(Elf64_Phdr phdr);
static uint32_t get_memsz(Elf64_Phdr phdr);
static void write_segment(Elf64_Phdr phdr, FILE *fp, FILE *img, int *phyaddr);
static void write_padding(FILE *img, int *phyaddr, int new_phyaddr);
static void write_img_info(int nbytes_kernel, task_info_t *taskinfo,
                           short tasknum, uint32_t app_info_offset, FILE *img);

int main(int argc, char **argv)
{
    char *progname = argv[0];

    /* process command line options */
    options.vm = 0;
    options.extended = 0;
    while ((argc > 1) && (argv[1][0] == '-') && (argv[1][1] == '-')) {
        char *option = &argv[1][2];

        if (strcmp(option, "vm") == 0) {
            options.vm = 1;
        } else if (strcmp(option, "extended") == 0) {
            options.extended = 1;
        } else {
            error("%s: invalid option\nusage: %s %s\n", progname,
                  progname, ARGS);
        }
        argc--;
        argv++;
    }
    if (options.vm == 1) {
        error("%s: option --vm not implemented\n", progname);
    }
    if (argc < 3) {
        /* at least 3 args (createimage bootblock main) */
        error("usage: %s %s\n", progname, ARGS);
    }
    create_image(argc - 1, argv + 1);
    return 0;
}

/* TODO: [p1-task4] assign your task_info_t somewhere in 'create_image' */
static void create_image(int nfiles, char *files[])
{
    int tasknum = nfiles - 2;
    int nbytes_kernel = 0;
    int kernel_phyaddr = 0, user_phyaddr = 0;
    FILE *fp = NULL, *kernel_img = NULL, *user_img = NULL;
    Elf64_Ehdr ehdr;
    Elf64_Phdr phdr;

    if(tasknum>TASK_MAXNUM){
        error("too many tasks: %d, maximum is %d\n", tasknum, TASK_MAXNUM);
    }
    memset(taskinfo, 0, sizeof(taskinfo));

    /* open the image file */
    kernel_img = fopen(KERNEL_IMAGE_FILE, "w");
    user_img = fopen(USER_IMAGE_FILE, "w");
    assert(kernel_img != NULL);
    assert(user_img != NULL);

    /* for each input file */
    for (int fidx = 0; fidx < nfiles; ++fidx) {

        int taskidx = fidx - 2;

        /* open input file */
        fp = fopen(*files, "r");
        assert(fp != NULL);

        /* read ELF header */
        read_ehdr(&ehdr, fp);
        printf("0x%04lx: %s\n", ehdr.e_entry, *files);
        FILE *target_img;
        int *target_phyaddr;
        if(fidx < 2){
            target_img = kernel_img;
            target_phyaddr = &kernel_phyaddr;
        }else{
            target_img = user_img;
            target_phyaddr = &user_phyaddr;
        }

        uint32_t file_start = (uint32_t)(*target_phyaddr);

        /* for each program header */
        for (int ph = 0; ph < ehdr.e_phnum; ph++) {

            /* read program header */
            read_phdr(&phdr, fp, ph, ehdr);

            if (phdr.p_type != PT_LOAD) continue;

            /* write segment to the image */
            write_segment(phdr, fp, target_img, target_phyaddr);

            /* update nbytes_kernel */
            // if (strcmp(*files, "main") == 0) {
            //     nbytes_kernel += get_filesz(phdr);
            // }
        }

        /* write padding bytes */
        /**
         * TODO:
         * 1. [p1-task3] do padding so that the kernel and every app program
         *  occupies the same number of sectors
         * 2. [p1-task4] only padding bootblock is allowed!
         */
        // if (strcmp(*files, "bootblock") == 0) {
        //     write_padding(img, &phyaddr, SECTOR_SIZE);
        // }

        // task3: 补0
        // int new_phyaddr;
        // if(fidx == 0){
        //     new_phyaddr = SECTOR_SIZE;
        // }else{
        //     new_phyaddr = SECTOR_SIZE+fidx*TASK_SIZE;
        // }
        // if(fidx==0 && phyaddr>TASK_NUM_LOC){
        //     error("bootblock is too large for image metadata\n");
        // }
        // if(phyaddr > new_phyaddr){
        //     error("%s is larger than its reserved area\n", *files);
        // }
        // write_padding(img, &phyaddr, new_phyaddr);
        // task4&5
        uint32_t file_size = (uint32_t)(*target_phyaddr)-file_start;
        if(fidx == 0){
            if(fidx==0 && kernel_phyaddr>APP_INFO_OFFSET_LOC){
                error("bootblock is too large for image metadata\n");
            }
            write_padding(kernel_img, &kernel_phyaddr, SECTOR_SIZE);
        }else if(fidx == 1){
            nbytes_kernel = (int)file_size;
        }else{
            if(file_size > TASK_SIZE){
                error("%s is larger than its memory area\n", *files);
            }

            strncpy(taskinfo[taskidx].task_name, *files, TASK_NAME_LEN-1);
            taskinfo[taskidx].task_name[TASK_NAME_LEN-1] = '\0';
            taskinfo[taskidx].task_offset = file_start;
            taskinfo[taskidx].task_size = file_size;
            taskinfo[taskidx].task_entry = get_entrypoint(ehdr);
        }

        fclose(fp);
        files++;
    }
    uint32_t app_info_offset = (uint32_t)kernel_phyaddr;

    fwrite(taskinfo, sizeof(task_info_t), tasknum, kernel_img);
    kernel_phyaddr += sizeof(task_info_t)*tasknum;

    int kernel_image_end = NBYTES2SEC(kernel_phyaddr)*SECTOR_SIZE;
    write_padding(kernel_img, &kernel_phyaddr, kernel_image_end);

    int user_image_end = NBYTES2SEC(user_phyaddr)*SECTOR_SIZE;
    write_padding(user_img, &user_phyaddr, user_image_end);

    write_img_info(nbytes_kernel, taskinfo, tasknum, app_info_offset, kernel_img);

    fclose(kernel_img);
    fclose(user_img);
}

static void read_ehdr(Elf64_Ehdr * ehdr, FILE * fp)
{
    int ret;

    ret = fread(ehdr, sizeof(*ehdr), 1, fp);
    assert(ret == 1);
    assert(ehdr->e_ident[EI_MAG1] == 'E');
    assert(ehdr->e_ident[EI_MAG2] == 'L');
    assert(ehdr->e_ident[EI_MAG3] == 'F');
}

static void read_phdr(Elf64_Phdr * phdr, FILE * fp, int ph,
                      Elf64_Ehdr ehdr)
{
    int ret;

    fseek(fp, ehdr.e_phoff + ph * ehdr.e_phentsize, SEEK_SET);
    ret = fread(phdr, sizeof(*phdr), 1, fp);
    assert(ret == 1);
    if (options.extended == 1) {
        printf("\tsegment %d\n", ph);
        printf("\t\toffset 0x%04lx", phdr->p_offset);
        printf("\t\tvaddr 0x%04lx\n", phdr->p_vaddr);
        printf("\t\tfilesz 0x%04lx", phdr->p_filesz);
        printf("\t\tmemsz 0x%04lx\n", phdr->p_memsz);
    }
}

static uint64_t get_entrypoint(Elf64_Ehdr ehdr)
{
    return ehdr.e_entry;
}

static uint32_t get_filesz(Elf64_Phdr phdr)
{
    return phdr.p_filesz;
}

static uint32_t get_memsz(Elf64_Phdr phdr)
{
    return phdr.p_memsz;
}

static void write_segment(Elf64_Phdr phdr, FILE *fp, FILE *img, int *phyaddr)
{
    if (phdr.p_memsz != 0 && phdr.p_type == PT_LOAD) {
        /* write the segment itself */
        /* NOTE: expansion of .bss should be done by kernel or runtime env! */
        if (options.extended == 1) {
            printf("\t\twriting 0x%04lx bytes\n", phdr.p_filesz);
        }
        fseek(fp, phdr.p_offset, SEEK_SET);
        while (phdr.p_filesz-- > 0) {
            fputc(fgetc(fp), img);
            (*phyaddr)++;
        }
    }
}

static void write_padding(FILE *img, int *phyaddr, int new_phyaddr)
{
    if (options.extended == 1 && *phyaddr < new_phyaddr) {
        printf("\t\twrite 0x%04x bytes for padding\n", new_phyaddr - *phyaddr);
    }

    while (*phyaddr < new_phyaddr) {
        fputc(0, img);
        (*phyaddr)++;
    }
}

static void write_img_info(int nbytes_kernel, task_info_t *taskinfo,
                           short tasknum, uint32_t app_info_offset, FILE * img)
{
    // TODO: [p1-task3] & [p1-task4] write image info to some certain places
    // NOTE: os size, infomation about app-info sector(s) ...
    unsigned short kernel_sectors = NBYTES2SEC(nbytes_kernel);

    // 写入App Info
    fseek(img, APP_INFO_OFFSET_LOC, SEEK_SET);
    fwrite(&app_info_offset, sizeof(app_info_offset), 1, img);

    // 写APP数量
    fseek(img, TASK_NUM_LOC, SEEK_SET);
    fwrite(&tasknum, sizeof(tasknum), 1, img);

    // 写kernel大小
    fseek(img, OS_SIZE_LOC, SEEK_SET);
    fwrite(&kernel_sectors, sizeof(kernel_sectors), 1, img);

    // 写启动签名
    fseek(img, BOOT_LOADER_SIG_OFFSET, SEEK_SET);
    fputc(BOOT_LOADER_SIG_1, img);
    fputc(BOOT_LOADER_SIG_2, img);
}

/* print an error message and exit */
static void error(char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (errno != 0) {
        perror(NULL);
    }
    exit(EXIT_FAILURE);
}
