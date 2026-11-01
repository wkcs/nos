/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <nos/mm.h>
#include <kernel/mm.h>
#include <uapi/nos/errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_pages_live;

addr_t alloc_pages(gfp_t flag, u32 order)
{
    void *ptr = NULL;
    size_t bytes = (size_t)CONFIG_PAGE_SIZE << order;
    if (posix_memalign(&ptr, CONFIG_PAGE_SIZE, bytes) != 0) {
        return 0;
    }
    if (flag & GFP_ZERO) {
        memset(ptr, 0, bytes);
    }
    __atomic_add_fetch(&g_pages_live, 1U << order, __ATOMIC_RELAXED);
    return (addr_t)ptr;
}

addr_t alloc_page(gfp_t flag)
{
    return alloc_pages(flag, 0);
}

int free_pages(addr_t addr, u32 order)
{
    if (addr == 0U) {
        return -EINVAL;
    }
    __atomic_sub_fetch(&g_pages_live, 1U << order, __ATOMIC_RELAXED);
    free((void *)addr);
    return 0;
}

int free_page(addr_t addr)
{
    return free_pages(addr, 0);
}

void *kmalloc(u32 size, gfp_t flag)
{
    void *p = malloc(size);
    if (p != NULL && (flag & GFP_ZERO)) {
        memset(p, 0, size);
    }
    return p;
}

void *kzalloc(u32 size, gfp_t flag)
{
    return kmalloc(size, flag | GFP_ZERO);
}

int kfree(void *addr)
{
    free(addr);
    return 0;
}

static void test_multilevel_page_table_walk(void)
{
    struct nos_vm_space space;
    uintptr_t paddr = 0;
    uint32_t prot = 0;

    assert(nos_vmm_space_init(&space) == 0);
    assert(nos_vmm_map_pages(&space, 0x00010000UL, 0x80010000UL, 4096U,
                             HAL_VM_PROT_READ | HAL_VM_PROT_EXEC | HAL_VM_PROT_USER) == 0);
    assert(nos_vmm_map_pages(&space, 0x40002000UL, 0x90002000UL, 8192U,
                             HAL_VM_PROT_READ | HAL_VM_PROT_WRITE) == 0);

    assert(nos_vmm_translate(&space, 0x00010000UL, &paddr, &prot) == 0);
    assert(paddr == 0x80010000UL);
    assert(prot == (HAL_VM_PROT_READ | HAL_VM_PROT_EXEC | HAL_VM_PROT_USER));

    assert(nos_vmm_translate(&space, 0x40003000UL, &paddr, &prot) == 0);
    assert(paddr == 0x90003000UL);
    assert(prot == (HAL_VM_PROT_READ | HAL_VM_PROT_WRITE));

    assert(nos_vmm_unmap_pages(&space, 0x40002000UL, 4096U) == 0);
    assert(nos_vmm_translate(&space, 0x40002000UL, &paddr, &prot) == -ENOENT);
    assert(nos_vmm_translate(&space, 0x40003000UL, &paddr, &prot) == 0);

    nos_vmm_space_destroy(&space);
    assert(nos_vmm_active_pt_pages() == 0U);
    assert(__atomic_load_n(&g_pages_live, __ATOMIC_RELAXED) == 0U);
}

static void test_vma_merge_and_split(void)
{
    struct nos_vm_space space;
    uintptr_t v1 = 0, v2 = 0;

    assert(nos_vmm_space_init(&space) == 0);
    assert(nos_vm_mmap(&space, 0x100000UL, 4096U, 0U,
                       HAL_VM_PROT_READ | HAL_VM_PROT_WRITE,
                       NOS_VM_FLAG_ANON | NOS_VM_FLAG_DEMAND | NOS_VM_FLAG_FIXED,
                       &v1) == 0);
    assert(nos_vm_mmap(&space, 0x101000UL, 8192U, 0U,
                       HAL_VM_PROT_READ | HAL_VM_PROT_WRITE,
                       NOS_VM_FLAG_ANON | NOS_VM_FLAG_DEMAND | NOS_VM_FLAG_FIXED,
                       &v2) == 0);
    /* Adjacent VMAs with identical flags/prot must merge into 1 VMA [0x100000, 0x103000) */
    assert(space.vma_count == 1U);

    /* Change middle page [0x101000, 0x102000) to Read-Only -> splits into 3 VMAs */
    assert(nos_vm_mprotect(&space, 0x101000UL, 4096U, HAL_VM_PROT_READ) == 0);
    assert(space.vma_count == 3U);
    assert(nos_vm_find_vma(&space, 0x100000UL)->vm_prot == (HAL_VM_PROT_READ | HAL_VM_PROT_WRITE));
    assert(nos_vm_find_vma(&space, 0x101000UL)->vm_prot == HAL_VM_PROT_READ);
    assert(nos_vm_find_vma(&space, 0x102000UL)->vm_prot == (HAL_VM_PROT_READ | HAL_VM_PROT_WRITE));

    /* Unmap middle page -> leaves 2 VMAs */
    assert(nos_vm_munmap(&space, 0x101000UL, 4096U) == 0);
    assert(space.vma_count == 2U);
    assert(nos_vm_find_vma(&space, 0x101000UL) == NULL);

    nos_vmm_space_destroy(&space);
    assert(__atomic_load_n(&g_pages_live, __ATOMIC_RELAXED) == 0U);
}

static void test_demand_paging_and_cow(void)
{
    struct nos_vm_space parent, child;
    uintptr_t vaddr = 0;
    uintptr_t parent_paddr = 0, child_paddr = 0;
    uint32_t prot = 0;

    assert(nos_vmm_space_init(&parent) == 0);
    assert(nos_vmm_space_init(&child) == 0);

    /* 1. Demand-paged anonymous mapping */
    assert(nos_vm_mmap(&parent, 0x200000UL, 4096U, 0U,
                       HAL_VM_PROT_READ | HAL_VM_PROT_WRITE | HAL_VM_PROT_USER,
                       NOS_VM_FLAG_ANON | NOS_VM_FLAG_DEMAND | NOS_VM_FLAG_FIXED | NOS_VM_FLAG_OWN_PAGES,
                       &vaddr) == 0);
    assert(nos_vmm_translate(&parent, vaddr, &parent_paddr, &prot) == -ENOENT);

    /* Trigger write page fault at vaddr + 0x40 -> allocates physical page on demand */
    assert(nos_vm_handle_fault(&parent, vaddr + 0x40UL, HAL_VM_PROT_WRITE) == 0);
    assert(nos_vmm_translate(&parent, vaddr, &parent_paddr, &prot) == 0);
    assert(parent_paddr != 0U);
    strcpy((char *)parent_paddr, "NOS-COW-PAYLOAD");

    /* 2. Clone parent -> child with Copy-on-Write */
    assert(nos_vm_clone(&child, &parent) == 0);
    assert(nos_vmm_translate(&parent, vaddr, &parent_paddr, &prot) == 0);
    assert((prot & HAL_VM_PROT_WRITE) == 0U); /* Downgraded to Read-Only for CoW */
    assert(nos_vmm_translate(&child, vaddr, &child_paddr, &prot) == 0);
    assert(child_paddr == parent_paddr);
    assert((prot & HAL_VM_PROT_WRITE) == 0U);

    /* 3. Child writes -> allocates private copy, copies "NOS-COW-PAYLOAD" */
    assert(nos_vm_handle_fault(&child, vaddr + 8UL, HAL_VM_PROT_WRITE) == 0);
    assert(nos_vmm_translate(&child, vaddr, &child_paddr, &prot) == 0);
    assert(child_paddr != parent_paddr);
    assert((prot & HAL_VM_PROT_WRITE) != 0U);
    assert(strcmp((const char *)child_paddr, "NOS-COW-PAYLOAD") == 0);
    strcpy((char *)child_paddr, "CHILD-MODIFIED");
    assert(strcmp((const char *)parent_paddr, "NOS-COW-PAYLOAD") == 0);

    /* 4. Parent writes -> refcount is now 1, so reuses parent_paddr in-place! */
    uintptr_t orig_parent_paddr = parent_paddr;
    assert(nos_vm_handle_fault(&parent, vaddr, HAL_VM_PROT_WRITE) == 0);
    assert(nos_vmm_translate(&parent, vaddr, &parent_paddr, &prot) == 0);
    assert(parent_paddr == orig_parent_paddr);
    assert((prot & HAL_VM_PROT_WRITE) != 0U);

    nos_vmm_space_destroy(&child);
    nos_vmm_space_destroy(&parent);
    assert(nos_vmm_active_pt_pages() == 0U);
    assert(__atomic_load_n(&g_pages_live, __ATOMIC_RELAXED) == 0U);
}

int main(void)
{
    test_multilevel_page_table_walk();
    test_vma_merge_and_split();
    test_demand_paging_and_cow();
    puts("MMU VMM and page table checks passed");
    return 0;
}
