#include <os/mm.h>
#include <assert.h>
#include <os/string.h>

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

    if(va<PAGE_SIZE || va>=USER_VA_LIMIT){
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

uintptr_t shm_page_get(int key)
{
    // TODO [P4-task4] shm_page_get:
}

void shm_page_dt(uintptr_t addr)
{
    // TODO [P4-task4] shm_page_dt:
}