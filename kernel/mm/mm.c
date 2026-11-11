#include <os/mm.h>
#include <assert.h>

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

    ptr_t ret = ROUND(kernMemCurr, PAGE_SIZE);
    kernMemCurr = ret+numPage*PAGE_SIZE;
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

void *kmalloc(size_t size)
{
    // TODO [P4-task1] (design you 'kmalloc' here if you need):
}


/* this is used for mapping kernel virtual address into user page table */
void share_pgtable(uintptr_t dest_pgdir, uintptr_t src_pgdir)
{
    // TODO [P4-task1] share_pgtable:
}

/* allocate physical page for `va`, mapping it into `pgdir`,
   return the kernel virtual address for the page
   */
uintptr_t alloc_page_helper(uintptr_t va, uintptr_t pgdir)
{
    // TODO [P4-task1] alloc_page_helper:
}

uintptr_t shm_page_get(int key)
{
    // TODO [P4-task4] shm_page_get:
}

void shm_page_dt(uintptr_t addr)
{
    // TODO [P4-task4] shm_page_dt:
}