#include <os/mm.h>
#include <assert.h>

typedef struct free_page{
    struct free_page *next;
}free_page_t;

static ptr_t kernMemCurr = FREEMEM_KERNEL;
static ptr_t userMemCurr = FREEMEM_USER;

static free_page_t *kernel_free_pages = NULL;
static free_page_t *user_free_pages = NULL;

// 运行期间由当前Task4的大内核锁保护；启动时只有主核执行分配。
ptr_t allocKernelPage(int numPage){
    if(numPage <= 0){
        return 0;
    }
    // 空闲列表无法判断页的连续性，此处只对单页分配处理
    if(numPage==1 && kernel_free_pages!=NULL){ // 优先使用回收列表
        free_page_t *page = kernel_free_pages;
        kernel_free_pages = page->next;
        return (ptr_t)page;
    }

    // align PAGE_SIZE
    ptr_t ret = ROUND(kernMemCurr, PAGE_SIZE);
    kernMemCurr = ret+numPage*PAGE_SIZE;
    return ret;
}

ptr_t allocUserPage(int numPage){
    if(numPage <= 0){
        return 0;
    }

    if(numPage==1 && user_free_pages!=NULL){
        free_page_t *page = user_free_pages;
        user_free_pages = page->next;
        return (ptr_t)page;
    }

    // align PAGE_SIZE
    ptr_t ret = ROUND(userMemCurr, PAGE_SIZE);
    userMemCurr = ret+numPage*PAGE_SIZE;
    return ret;
}

/**
 * @brief 释放1页
 * 
 * @param base 页起始地址
 */
void freePage(ptr_t base){
    if(base == 0){
        return;
    }

    assert((base&(PAGE_SIZE-1))==0);

    free_page_t **free_list;

    if(base>=FREEMEM_KERNEL && base<kernMemCurr){ // 选取空闲列表
        free_list = &kernel_free_pages;
    }else if(base>=FREEMEM_USER && base<userMemCurr){
        free_list = &user_free_pages;
    }else{
        assert(0);
        return;
    }

    // 页已经不再使用，可以将其开头作为空闲链表节点。
    free_page_t *page = (free_page_t*)base;
    page->next = *free_list;
    *free_list = page;
}