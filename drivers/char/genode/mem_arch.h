
/*
 * architecure dependent memory checks are implemented here to keep the module code clean
 * those are taken from drivers/char/mem.c
 */

#ifndef _MEM_ARCH_H_
#define _MEM_ARCH_H_

#include <linux/mm.h>

#ifndef ARCH_HAS_VALID_PHYS_ADDR_RANGE

static inline int valid_mmap_phys_addr_range(unsigned long pfn, size_t size)
{
    return 1;
}

#endif //ARCH_HAS_VALID_PHYS_ADDR_RANGE

#ifndef CONFIG_MMU

static inline int private_mapping_ok(struct vm_area_struct *vma)
{
    return vma->vm_flags & VM_MAYSHARE;
}

#else

static inline int private_mapping_ok(struct vm_area_struct *vma)
{
    return 1;
}

#endif //CONFIG_MMU

#if defined(CONFIG_STRICT_DEVMEM) && !defined(MODULE)

static inline int range_is_allowed(unsigned long pfn, unsigned long size)
{
    u64 from = ((u64)pfn) << PAGE_SHIFT;
    u64 to = from + size;
    u64 cursor = from;

    while (cursor < to){
        
        if(!devmem_is_allowed(pfn))
            return 0;

        cursor += PAGE_SIZE;
        pfn++;
    }
    return 1;
}

#else

static inline int range_is_allowed(unsigned long pfn, unsigned long size)
{
    return 1;
}

#endif //CONFIG_STRICT_DEVMEM

int __weak phys_mem_access_prot_allowed(struct file *file, unsigned long pfn, unsigned long size, pgprot_t *vma_prot)
{
    return 1;
}

#endif //_MEM_ARCH_H_
