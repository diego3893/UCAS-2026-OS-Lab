#include <os/mm.h>
#include <assert.h>
#include <os/string.h>
#include <os/kernel.h>
#include <os/sched.h>
#include <printk.h>

typedef struct free_page{
    struct free_page *next;
}free_page_t;

static ptr_t kernMemCurr = FREEMEM_KERNEL;
static free_page_t *free_pages = NULL;

// 启动时由主核使用，运行期间沿用已有的内核状态锁保护
ptr_t allocPage(int numPage){
    if(numPage <= 0){
        return 0;
    }

    // 空闲链表只用于单页复用，多页仍连续分配
    if(numPage==1 && free_pages!=NULL){
        free_page_t *page = free_pages;
        free_pages = page->next;
        return (ptr_t)page;
    }

    ptr_t ret = ROUND(kernMemCurr, PAGE_SIZE); // 4KB对齐

    if((uint64_t)numPage > (FREEMEM_END-ret)/PAGE_SIZE){
        return 0;
    }

    kernMemCurr = ret+(uint64_t)numPage*PAGE_SIZE;
    return ret;
}

// 兼容P3现有调用
ptr_t allocKernelPage(int numPage){
    return allocPage(numPage);
}

// 返回内核虚拟地址；P4用户地址映射由页表管理代码建立。
ptr_t allocUserPage(int numPage){
    return allocPage(numPage);
}

void freePage(ptr_t base){
    if(base == 0){
        return;
    }

    assert((base&(PAGE_SIZE-1))==0);
    assert(base>=FREEMEM_KERNEL && base<kernMemCurr);

    free_page_t *page = (free_page_t*)base;
    page->next = free_pages;
    free_pages = page;
}

// NOTE: Only need for S-core to alloc 2MB large page
#ifdef S_CORE
static ptr_t largePageMemCurr = LARGE_PAGE_FREEMEM;
ptr_t allocLargePage(int numPage)
{
    // align LARGE_PAGE_SIZE
    ptr_t ret = ROUND(largePageMemCurr, LARGE_PAGE_SIZE);
    largePageMemCurr = ret + numPage * LARGE_PAGE_SIZE;
    return ret;    
}
#endif

// void freePage(ptr_t baseAddr)
// {
//     // TODO [P4-task1] (design you 'freePage' here if you need):
// }

void *kmalloc(size_t size){
    // TODO [P4-task1] (design you 'kmalloc' here if you need):
    if(size==0 || size>FREEMEM_END-FREEMEM_KERNEL){
        return NULL;
    }
    return (void*)allocPage((size+PAGE_SIZE-1)/PAGE_SIZE);
}

/**
 * @brief 根据用户VA找到对应的最后一级PTE；缺少中间页表时，可以分配中间页表
 * 
 * @param pgdir 根页表
 * @param va 用户va
 * @param create 若表项空，是否建表
 * @return PTE* 对应的PTE
 */
PTE *walk_user_pte(uintptr_t pgdir, uintptr_t va, int create){
    if(pgdir==0 || va>=USER_VA_LIMIT){
        return NULL;
    }

    PTE *table = (PTE*)pgdir;

    for(int level=2; level>0; --level){
        unsigned index = (va>>(NORMAL_PAGE_SHIFT+level*PPN_BITS))&0x1ffUL; // 当前页表的VPN
        PTE *entry = &table[index];

        if(!(*entry&_PAGE_PRESENT)){ // 检查Valid
            if(!create){
                return NULL;
            }

            uintptr_t child = allocPage(1); // 建下级页表
            if(child == 0){
                return NULL;
            }
            clear_pgdir(child);

            *entry = 0;
            set_pfn(entry, kva2pa(child)>>NORMAL_PAGE_SHIFT);
            set_attribute(entry, _PAGE_PRESENT);
        }else if(*entry&(_PAGE_READ|_PAGE_WRITE|_PAGE_EXEC)){ // RWX全0才是非叶子
            // 用户区采用三级4KB页，不在这里处理大页
            return NULL;
        }

        table = (PTE*)pa2kva(get_pa(*entry));
    }

    return &table[(va>>NORMAL_PAGE_SHIFT)&0x1ffUL];
}

static void free_user_table(uintptr_t table_kva, int level){
    PTE *table = (PTE*)table_kva;
    // 根页表只遍历低半区，高半区是公共内核映射
    int count = level==2 ? NUM_PTE_ENTRY/2 : NUM_PTE_ENTRY;

    for(int i=0; i<count; ++i){
        PTE entry = table[i];

        if(!(entry&_PAGE_PRESENT)){
            continue;
        }

        if(entry&(_PAGE_READ|_PAGE_WRITE|_PAGE_EXEC)){
            assert(level==0 && (entry&_PAGE_USER)); // 释放末级用户页
            freePage(pa2kva(get_pa(entry)));
        }else{
            assert(level>0);
            free_user_table(pa2kva(get_pa(entry)), level-1); // 递归释放次级页表
        }

        table[i] = 0;
    }

    freePage(table_kva);
}

void free_user_pgdir(uintptr_t pgdir){
    if(pgdir==0 || pgdir==pa2kva(PGDIR_PA)){
        return;
    }

    swap_forget_pgdir(pgdir);
    free_user_table(pgdir, 2);
}


/* this is used for mapping kernel virtual address into user page table */
void share_pgtable(uintptr_t dest_pgdir, uintptr_t src_pgdir){
    // TODO [P4-task1] share_pgtable:
    PTE *dest = (PTE*)dest_pgdir;
    PTE *src = (PTE*)src_pgdir;

    // 只复制内核高半区，不复制临时低地址恒等映射
    for(int i=NUM_PTE_ENTRY/2; i<NUM_PTE_ENTRY; ++i){
        dest[i] = src[i];
    }
}

/* allocate physical page for `va`, mapping it into `pgdir`,
   return the kernel virtual address for the page
   */
uintptr_t alloc_page_helper(uintptr_t va, uintptr_t pgdir){
    // TODO [P4-task1] alloc_page_helper:
    va = ROUNDDOWN(va, PAGE_SIZE);

    if(va>=USER_VA_LIMIT){
        return 0;
    }

    PTE *entry = walk_user_pte(pgdir, va, 1);
    if(entry == NULL){
        return 0;
    }

    if(*entry&_PAGE_PRESENT){
        if(!(*entry&_PAGE_USER) ||
           !(*entry&(_PAGE_READ|_PAGE_WRITE|_PAGE_EXEC))){
            return 0;
        }

        return pa2kva(get_pa(*entry));
    }

    // 分配新页
    uintptr_t page = allocPage(1);
    if(page == 0){
        return 0;
    }
    // 新页清零
    memset((void*)page, 0, PAGE_SIZE);

    *entry = 0;
    set_pfn(entry, kva2pa(page)>>NORMAL_PAGE_SHIFT);
    set_attribute(entry, _PAGE_PRESENT|_PAGE_READ|_PAGE_WRITE|
                        _PAGE_EXEC|_PAGE_USER|_PAGE_ACCESSED|_PAGE_DIRTY); // task1 AD先置1

    return page;
}

// P4-task3: Clock page replacement

// 每个记录对应一个用户虚拟页，同时独占一个swap槽位
typedef struct swap_page{
    uintptr_t pgdir;
    uintptr_t va;
    uintptr_t frame; // frame!=0表示驻留，frame==0表示数据保存在对应swap槽中
} swap_page_t;

// 每个地址空间维护独立的驻留数量和Clock扫描位置
typedef struct swap_space{
    uintptr_t pgdir;
    pid_t pid;
    int resident; // 内存驻留数
    int hand; // clock扫描指针
    uint64_t swap_outs; 
    uint64_t swap_ins;
} swap_space_t;

static swap_page_t swap_pages[SWAP_SLOT_COUNT];
static swap_space_t swap_spaces[NUM_MAX_TASK];

static uint32_t swap_first_sector;
static int swap_ready;

static PTE make_swap_pte(int slot){
    return ((PTE)(slot+1)<<_PAGE_PFN_SHIFT)|_PAGE_SWAPPED; // 将PPN直接填充为swap slot+1
}

static unsigned swap_sector(int slot){
    return swap_first_sector+(unsigned)slot*SWAP_SECTORS_PER_PAGE;
}

int init_swap(uint32_t first_sector){
    uint64_t end = (uint64_t)first_sector+(uint64_t)SWAP_SLOT_COUNT*SWAP_SECTORS_PER_PAGE;
    if(end > UINT32_MAX){
        return -1;
    }
    memset(swap_pages, 0, sizeof(swap_pages));
    memset(swap_spaces, 0, sizeof(swap_spaces));
    swap_first_sector = first_sector;
    swap_ready = 1;
    return 0;
}

/**
 * @brief 寻找进程的换页地址空间
 * 
 * @param pgdir 根页表
 * @param create 是否新建
 * @return swap_space_t* 换页地址空间
 */
static swap_space_t *find_swap_space(uintptr_t pgdir, int create){
    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(swap_spaces[i].pgdir == pgdir){
            return &swap_spaces[i];
        }
    }
    if(!create){
        return NULL;
    }
    for(int i=0; i<NUM_MAX_TASK; ++i){
        if(swap_spaces[i].pgdir == 0){
            memset(&swap_spaces[i], 0, sizeof(swap_spaces[i]));
            swap_spaces[i].pgdir = pgdir;
            swap_spaces[i].pid = current_running->pid;
            return &swap_spaces[i];
        }
    }
    return NULL;
}

/**
 * @brief 获得swap槽
 * 
 * @param pgdir 根页表
 * @param va 虚地址
 * @return int swap槽位号
 */
static int reserve_swap_page(uintptr_t pgdir, uintptr_t va){
    int free_slot = -1;
    for(int i=0; i<SWAP_SLOT_COUNT; ++i){
        // 同一用户页不能重复创建
        if(swap_pages[i].pgdir==pgdir && swap_pages[i].va==va){
            return -1;
        }
        if(swap_pages[i].pgdir==0 && free_slot<0){
            free_slot = i;
        }
    }
    if(free_slot < 0){
        return -1;
    }
    swap_pages[free_slot].pgdir = pgdir;
    swap_pages[free_slot].va = va;
    swap_pages[free_slot].frame = 0;
    return free_slot;
}

// 从当前地址空间中选择换出页，返回可复用页框的kva
static uintptr_t clock_evict(swap_space_t *space){
    if(space->resident == 0){
        return 0;
    }

    // 至多两圈
    for(int scanned=0; scanned<2*SWAP_SLOT_COUNT; ++scanned){
        int slot = space->hand;
        space->hand = (space->hand+1)%SWAP_SLOT_COUNT;
        swap_page_t *page = &swap_pages[slot];
        if(page->pgdir!=space->pgdir || page->frame==0){
            continue;
        }

        PTE *entry = walk_user_pte(page->pgdir, page->va, 0);
        assert(entry!=NULL && (*entry&_PAGE_PRESENT));
        assert(pa2kva(get_pa(*entry))==page->frame);
        if(*entry&_PAGE_ACCESSED){ // clock: A=1，再给一次机会
            *entry &= ~(uint64_t)_PAGE_ACCESSED;
            local_flush_tlb_all();
            continue;
        }

        PTE old_entry = *entry; // 暂存，便于恢复
        *entry = make_swap_pte(slot); // 撤销用户映射
        local_flush_tlb_all();
        unsigned pa = (unsigned)kva2pa(page->frame);
        if(bios_sd_write(pa, SWAP_SECTORS_PER_PAGE, swap_sector(slot)) < 0){
            // 写盘失败，原数据仍在物理页中，恢复原映射
            *entry = old_entry;
            local_flush_tlb_all();
            return 0;
        }

        uintptr_t frame = page->frame;
        page->frame = 0;
        space->resident--;
        space->swap_outs++;

        return frame;
    }
    return 0;
}

/**
 * @brief 处理缺页情况，驻留页面未达上限则分配，其他情况使用clock换页
 * 
 * @param space 换页空间
 * @return uintptr_t 页框号
 */
static uintptr_t obtain_swap_frame(swap_space_t *space){
    if(space->resident < SWAP_RESIDENT_LIMIT){
        uintptr_t frame = allocPage(1);
        if(frame != 0){
            return frame;
        }
    }
    return clock_evict(space);
}

int swap_handle_fault(uintptr_t pgdir, uintptr_t va, uint64_t access){
    if(!swap_ready || pgdir==0 || va>=USER_VA_LIMIT || current_running->pid==0 || pgdir!=current_running->pgdir){
        return -1;
    }
    if(access!=_PAGE_READ && access!=_PAGE_WRITE){
        return -1;
    }
    va = ROUNDDOWN(va, PAGE_SIZE);
    PTE *entry = walk_user_pte(pgdir, va, 1);
    if(entry == NULL){
        return -1;
    }
    if(*entry&_PAGE_PRESENT){
        return -1;
    }

    swap_space_t *space = find_swap_space(pgdir, 1);
    if(space == NULL){
        return -1;
    }

    int slot;
    int from_disk = *entry!=0;

    if(from_disk){ // 此处仅分配槽位
        uint64_t encoded_slot = (uint64_t)get_pfn(*entry);
        if(!(*entry&_PAGE_SWAPPED) || encoded_slot==0 || encoded_slot>SWAP_SLOT_COUNT){
            return -1;
        }
        slot = (int)encoded_slot-1;
        if(*entry != make_swap_pte(slot)){
            return -1;
        }

        swap_page_t *page = &swap_pages[slot];
        if(page->pgdir!=pgdir || page->va!=va || page->frame!=0){
            return -1;
        }
    }else{
        slot = reserve_swap_page(pgdir, va);
        if(slot < 0){
            return -1;
        }
    }

    uintptr_t frame = obtain_swap_frame(space);
    if(frame == 0){
        if(!from_disk){
            memset(&swap_pages[slot], 0, sizeof(swap_pages[slot]));
        }
        return -1;
    }
    if(from_disk){
        unsigned pa = (unsigned)kva2pa(frame);
        if(bios_sd_read(pa, SWAP_SECTORS_PER_PAGE, swap_sector(slot)) < 0){
            freePage(frame);
            return -1;
        }
    }else{
        memset((void*)frame, 0, PAGE_SIZE); // 首次分配清零
    }

    swap_pages[slot].frame = frame;
    space->resident++;
    if(from_disk){
        space->swap_ins++;
    }

    *entry = 0;
    set_pfn(entry, kva2pa(frame)>>NORMAL_PAGE_SHIFT);
    set_attribute(entry, _PAGE_PRESENT|_PAGE_USER|_PAGE_READ|
                        _PAGE_WRITE|_PAGE_ACCESSED|_PAGE_DIRTY);
    local_flush_tlb_all();
    return 0;
}

// 清理软件记录及swap槽位
void swap_forget_pgdir(uintptr_t pgdir){
    swap_space_t *space = find_swap_space(pgdir, 0);

#if SWAP_DEBUG
    if(space != NULL){
        printk("> [SWAP] pid=%d out=%lu in=%lu\n",
               space->pid, space->swap_outs, space->swap_ins);
    }
#endif

    for(int i=0; i<SWAP_SLOT_COUNT; ++i){
        if(swap_pages[i].pgdir == pgdir){
            memset(&swap_pages[i], 0, sizeof(swap_pages[i]));
        }
    }
    if(space != NULL){
        memset(space, 0, sizeof(*space));
    }
}

uintptr_t shm_page_get(int key)
{
    // TODO [P4-task4] shm_page_get:
}

void shm_page_dt(uintptr_t addr)
{
    // TODO [P4-task4] shm_page_dt:
}