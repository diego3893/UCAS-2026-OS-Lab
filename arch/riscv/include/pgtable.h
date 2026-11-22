#ifndef PGTABLE_H
#define PGTABLE_H

#include <type.h>

#define SATP_MODE_SV39 8 // sv39
#define SATP_MODE_SV48 9

#define SATP_ASID_SHIFT 44lu
#define SATP_MODE_SHIFT 60lu

#define NORMAL_PAGE_SHIFT 12lu // 4KB正常页内偏移长度
#define NORMAL_PAGE_SIZE (1lu << NORMAL_PAGE_SHIFT) 
#define LARGE_PAGE_SHIFT 21lu // 2MB大页内偏移长度
#define LARGE_PAGE_SIZE (1lu << LARGE_PAGE_SHIFT)

/*
 * Flush entire local TLB.  'sfence.vma' implicitly fences with the instruction
 * cache as well, so a 'fence.i' is not necessary.
 * 刷新TLB
 */
static inline void local_flush_tlb_all(void)
{
    __asm__ __volatile__ ("sfence.vma" : : : "memory");
}

/* Flush one page from local TLB */
static inline void local_flush_tlb_page(unsigned long addr)
{
    __asm__ __volatile__ ("sfence.vma %0" : : "r" (addr) : "memory");
}

static inline void local_flush_icache_all(void)
{
    asm volatile ("fence.i" ::: "memory");
}

static inline void set_satp(
    unsigned mode, unsigned asid, unsigned long ppn)
{
    unsigned long __v =
        (unsigned long)(((unsigned long)mode << SATP_MODE_SHIFT) | ((unsigned long)asid << SATP_ASID_SHIFT) | ppn);
    __asm__ __volatile__("sfence.vma\ncsrw satp, %0" : : "rK"(__v) : "memory");
}

#define PGDIR_PA 0x51000000lu  // use 51000000 page as PGDIR

/*
 * PTE format:
 * | XLEN-1  10 | 9             8 | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0
 *       PFN      reserved for SW   D   A   G   U   X   W   R   V
 */

#define _PAGE_ACCESSED_OFFSET 6

// 控制位
#define _PAGE_PRESENT (1 << 0)
#define _PAGE_READ (1 << 1)     /* Readable */
#define _PAGE_WRITE (1 << 2)    /* Writable */
#define _PAGE_EXEC (1 << 3)     /* Executable */
#define _PAGE_USER (1 << 4)     /* User */
#define _PAGE_GLOBAL (1 << 5)   /* Global */
#define _PAGE_ACCESSED (1 << 6) /* Set by hardware on any access \
                                 */
#define _PAGE_DIRTY (1 << 7)    /* Set by hardware on any write */
#define _PAGE_SOFT (1 << 8)     /* Reserved for software */

#define _PAGE_PFN_SHIFT 10lu // 页框号从第10位开始

#define VA_MASK ((1lu << 39) - 1) // 取虚地址三级索引和页内偏移

#define PPN_BITS 9lu // 每一级索引长度
#define NUM_PTE_ENTRY (1 << PPN_BITS) // 每一级页表大小

typedef uint64_t PTE;

#define KERNEL_VA_OFFSET 0xffffffc000000000UL // kva固定偏移
#define PTE_PFN_MASK (((1UL<<44)-1)<<_PAGE_PFN_SHIFT) // 高44位取页框号

/* Translation between physical addr and kernel virtual addr */
static inline uintptr_t kva2pa(uintptr_t kva){
    /* TODO: [P4-task1] */
    return kva-KERNEL_VA_OFFSET;
}

static inline uintptr_t pa2kva(uintptr_t pa){
    /* TODO: [P4-task1] */
    return pa+KERNEL_VA_OFFSET;
}

/* get physical page addr from PTE 'entry'，取得可以用于拼接的页框号 */
static inline uint64_t get_pa(PTE entry){
    /* TODO: [P4-task1] */
    return ((entry&PTE_PFN_MASK)>>_PAGE_PFN_SHIFT)<<NORMAL_PAGE_SHIFT; 
}

/* Get/Set page frame number of the `entry`，获得页框号 */
static inline long get_pfn(PTE entry){
    /* TODO: [P4-task1] */
    return (long)((entry&PTE_PFN_MASK)>>_PAGE_PFN_SHIFT);
}

// 在PTE中设置pfn
static inline void set_pfn(PTE *entry, uint64_t pfn){
    /* TODO: [P4-task1] */
    *entry = (*entry&~PTE_PFN_MASK)|((pfn<<_PAGE_PFN_SHIFT)&PTE_PFN_MASK);
}

/* Get/Set attribute(s) of the `entry` */
static inline long get_attribute(PTE entry, uint64_t mask){
    /* TODO: [P4-task1] */
    return (long)(entry&mask);
}

// 末10位按照bits置位
static inline void set_attribute(PTE *entry, uint64_t bits){
    /* TODO: [P4-task1] */
    *entry = *entry|(bits&0x3ffUL);
}

// 清空4KB页表
static inline void clear_pgdir(uintptr_t pgdir_addr){
    /* TODO: [P4-task1] */
    volatile PTE *pgdir = (volatile PTE*)pgdir_addr;

    for(int i=0; i<NUM_PTE_ENTRY; ++i){
        pgdir[i] = 0;
    }
}

#endif  // PGTABLE_H
