/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Multi-Level Page Table Walker & VMA Virtual Memory Manager (MMU Subsystem)
 * Supports 3-level 4KB page tables (512 entries/table), interval VMA merge/split,
 * on-demand anonymous paging, and Copy-on-Write (CoW) address space cloning.
 */

#include <nos/mm.h>
#include <kernel/mm.h>
#include <uapi/nos/errno.h>
#include <string.h>

#define NOS_PT_ENTRIES          512U
#define NOS_PT_INDEX_MASK       0x1FFULL
#define NOS_PAGE_OFFSET_MASK    ((uint64_t)CONFIG_PAGE_SIZE - 1ULL)
#define NOS_PTE_ADDR_MASK       (~0xFFFULL)

#define NOS_PTE_VALID           (1ULL << 0)
#define NOS_PTE_TABLE           (1ULL << 1)
#define NOS_PTE_READ            (1ULL << 2)
#define NOS_PTE_WRITE           (1ULL << 3)
#define NOS_PTE_EXEC            (1ULL << 4)
#define NOS_PTE_USER            (1ULL << 5)
#define NOS_PTE_NOCACHE         (1ULL << 6)
#define NOS_PTE_COW             (1ULL << 7)
#define NOS_PTE_OWNED           (1ULL << 8)

#define NOS_MMAP_DEFAULT_BASE   0x00100000UL
#define NOS_MMAP_DEFAULT_LIMIT  0x70000000UL
#define NOS_COW_REF_SLOTS       256U

struct nos_page_ref_entry {
    uintptr_t paddr;
    uint32_t  refcount;
};

static struct nos_page_ref_entry g_page_refs[NOS_COW_REF_SLOTS];
static SPINLOCK(g_page_ref_lock);
static uint32_t g_active_pt_pages;
static uint16_t g_next_asid = 1U;

static uintptr_t vmm_align_up(uintptr_t val, size_t align)
{
    uintptr_t mask = (uintptr_t)align - 1U;
    return (val + mask) & ~mask;
}

static uint64_t vmm_prot_to_pte_flags(uint32_t prot)
{
    uint64_t flags = 0ULL;
    if (prot & HAL_VM_PROT_READ) {
        flags |= NOS_PTE_READ;
    }
    if (prot & HAL_VM_PROT_WRITE) {
        flags |= NOS_PTE_WRITE;
    }
    if (prot & HAL_VM_PROT_EXEC) {
        flags |= NOS_PTE_EXEC;
    }
    if (prot & HAL_VM_PROT_USER) {
        flags |= NOS_PTE_USER;
    }
    if (prot & HAL_VM_PROT_DEV) {
        flags |= NOS_PTE_NOCACHE;
    }
    return flags;
}

static uint32_t vmm_pte_flags_to_prot(uint64_t pte)
{
    uint32_t prot = 0U;
    if (pte & NOS_PTE_READ) {
        prot |= HAL_VM_PROT_READ;
    }
    if (pte & NOS_PTE_WRITE) {
        prot |= HAL_VM_PROT_WRITE;
    }
    if (pte & NOS_PTE_EXEC) {
        prot |= HAL_VM_PROT_EXEC;
    }
    if (pte & NOS_PTE_USER) {
        prot |= HAL_VM_PROT_USER;
    }
    if (pte & NOS_PTE_NOCACHE) {
        prot |= HAL_VM_PROT_DEV;
    }
    return prot;
}

static void vmm_page_ref_inc(uintptr_t paddr)
{
    uint32_t i;
    uint32_t free_idx = NOS_COW_REF_SLOTS;

    spin_lock_irq(&g_page_ref_lock);
    for (i = 0U; i < NOS_COW_REF_SLOTS; i++) {
        if (g_page_refs[i].refcount > 0U && g_page_refs[i].paddr == paddr) {
            g_page_refs[i].refcount++;
            spin_unlock_irq(&g_page_ref_lock);
            return;
        }
        if (g_page_refs[i].refcount == 0U && free_idx == NOS_COW_REF_SLOTS) {
            free_idx = i;
        }
    }
    if (free_idx < NOS_COW_REF_SLOTS) {
        g_page_refs[free_idx].paddr = paddr;
        g_page_refs[free_idx].refcount = 1U;
    }
    spin_unlock_irq(&g_page_ref_lock);
}

static uint32_t vmm_page_ref_get(uintptr_t paddr)
{
    uint32_t i;
    uint32_t count = 1U;

    spin_lock_irq(&g_page_ref_lock);
    for (i = 0U; i < NOS_COW_REF_SLOTS; i++) {
        if (g_page_refs[i].refcount > 0U && g_page_refs[i].paddr == paddr) {
            count = g_page_refs[i].refcount;
            break;
        }
    }
    spin_unlock_irq(&g_page_ref_lock);
    return count;
}

static void vmm_page_ref_dec_and_free(uintptr_t paddr)
{
    uint32_t i;
    bool should_free = true;

    spin_lock_irq(&g_page_ref_lock);
    for (i = 0U; i < NOS_COW_REF_SLOTS; i++) {
        if (g_page_refs[i].refcount > 0U && g_page_refs[i].paddr == paddr) {
            g_page_refs[i].refcount--;
            if (g_page_refs[i].refcount > 0U) {
                should_free = false;
            } else {
                g_page_refs[i].paddr = 0U;
            }
            break;
        }
    }
    spin_unlock_irq(&g_page_ref_lock);

    if (should_free) {
        free_page((addr_t)paddr);
    }
}

static uint64_t *vmm_alloc_pt_page(void)
{
    addr_t page = alloc_page(GFP_KERNEL | GFP_ZERO);
    if (page == 0U) {
        return NULL;
    }
    __atomic_add_fetch(&g_active_pt_pages, 1U, __ATOMIC_RELAXED);
    return (uint64_t *)(uintptr_t)page;
}

static void vmm_free_pt_page(uint64_t *pt)
{
    if (pt == NULL) {
        return;
    }
    __atomic_sub_fetch(&g_active_pt_pages, 1U, __ATOMIC_RELAXED);
    free_page((addr_t)(uintptr_t)pt);
}

static uint64_t *vmm_walk_pte(struct nos_vm_space *space, uintptr_t vaddr, bool alloc)
{
    uint64_t *l2;
    uint64_t *l1;
    uint64_t *l0;
    uint64_t v = (uint64_t)vaddr;
    uint32_t idx2 = (uint32_t)((v >> 30) & NOS_PT_INDEX_MASK);
    uint32_t idx1 = (uint32_t)((v >> 21) & NOS_PT_INDEX_MASK);
    uint32_t idx0 = (uint32_t)((v >> 12) & NOS_PT_INDEX_MASK);

    if (space == NULL || space->root_pt_phys == 0U) {
        return NULL;
    }

    l2 = (uint64_t *)space->root_pt_phys;
    if ((l2[idx2] & NOS_PTE_VALID) == 0ULL) {
        if (!alloc) {
            return NULL;
        }
        l1 = vmm_alloc_pt_page();
        if (l1 == NULL) {
            return NULL;
        }
        l2[idx2] = ((uint64_t)(uintptr_t)l1 & NOS_PTE_ADDR_MASK) | NOS_PTE_VALID | NOS_PTE_TABLE;
    } else {
        l1 = (uint64_t *)(uintptr_t)(l2[idx2] & NOS_PTE_ADDR_MASK);
    }

    if ((l1[idx1] & NOS_PTE_VALID) == 0ULL) {
        if (!alloc) {
            return NULL;
        }
        l0 = vmm_alloc_pt_page();
        if (l0 == NULL) {
            return NULL;
        }
        l1[idx1] = ((uint64_t)(uintptr_t)l0 & NOS_PTE_ADDR_MASK) | NOS_PTE_VALID | NOS_PTE_TABLE;
    } else {
        l0 = (uint64_t *)(uintptr_t)(l1[idx1] & NOS_PTE_ADDR_MASK);
    }

    return &l0[idx0];
}

static int vmm_map_page_raw(struct nos_vm_space *space,
                            uintptr_t vaddr,
                            uintptr_t paddr,
                            uint32_t prot_flags,
                            uint64_t extra_pte_flags)
{
    uint64_t *pte = vmm_walk_pte(space, vaddr, true);
    if (pte == NULL) {
        return -ENOMEM;
    }
    if ((*pte & NOS_PTE_VALID) != 0ULL && (*pte & NOS_PTE_OWNED) != 0ULL) {
        uintptr_t old_paddr = (uintptr_t)(*pte & NOS_PTE_ADDR_MASK);
        if (old_paddr != paddr) {
            vmm_page_ref_dec_and_free(old_paddr);
        }
    }
    *pte = ((uint64_t)paddr & NOS_PTE_ADDR_MASK) |
           NOS_PTE_VALID |
           vmm_prot_to_pte_flags(prot_flags) |
           extra_pte_flags;
    return 0;
}

int nos_vmm_space_init(struct nos_vm_space *space)
{
    uint64_t *root;

    if (space == NULL) {
        return -EINVAL;
    }
    memset(space, 0, sizeof(*space));
    spin_lock_init(&space->lock);
    INIT_LIST_HEAD(&space->vma_list);
    space->mmap_base = NOS_MMAP_DEFAULT_BASE;
    space->mmap_limit = NOS_MMAP_DEFAULT_LIMIT;

    root = vmm_alloc_pt_page();
    if (root == NULL) {
        return -ENOMEM;
    }
    space->root_pt_phys = (uintptr_t)root;
    space->asid = __atomic_fetch_add(&g_next_asid, 1U, __ATOMIC_RELAXED);
    space->initialized = true;
    return 0;
}

void nos_vmm_space_destroy(struct nos_vm_space *space)
{
    struct list_head *pos;
    struct list_head *n;
    uint64_t *l2;
    uint32_t i, j, k;

    if (space == NULL || !space->initialized) {
        return;
    }

    list_for_each_safe(pos, n, &space->vma_list) {
        struct nos_vm_area *vma = list_entry(pos, struct nos_vm_area, list);
        list_del(&vma->list);
        kfree(vma);
    }
    space->vma_count = 0U;

    if (space->root_pt_phys != 0U) {
        l2 = (uint64_t *)space->root_pt_phys;
        for (i = 0U; i < NOS_PT_ENTRIES; i++) {
            if ((l2[i] & NOS_PTE_VALID) == 0ULL) {
                continue;
            }
            uint64_t *l1 = (uint64_t *)(uintptr_t)(l2[i] & NOS_PTE_ADDR_MASK);
            for (j = 0U; j < NOS_PT_ENTRIES; j++) {
                if ((l1[j] & NOS_PTE_VALID) == 0ULL) {
                    continue;
                }
                uint64_t *l0 = (uint64_t *)(uintptr_t)(l1[j] & NOS_PTE_ADDR_MASK);
                for (k = 0U; k < NOS_PT_ENTRIES; k++) {
                    if ((l0[k] & NOS_PTE_VALID) != 0ULL && (l0[k] & NOS_PTE_OWNED) != 0ULL) {
                        vmm_page_ref_dec_and_free((uintptr_t)(l0[k] & NOS_PTE_ADDR_MASK));
                    }
                }
                vmm_free_pt_page(l0);
            }
            vmm_free_pt_page(l1);
        }
        vmm_free_pt_page(l2);
        space->root_pt_phys = 0U;
    }
    space->initialized = false;
}

int nos_vmm_map_pages(struct nos_vm_space *space,
                      uintptr_t vaddr,
                      uintptr_t paddr,
                      size_t size,
                      uint32_t prot_flags)
{
    size_t offset = 0U;

    if (space == NULL || !space->initialized || size == 0U) {
        return -EINVAL;
    }
    if (((vaddr | paddr | size) & (CONFIG_PAGE_SIZE - 1U)) != 0U) {
        return -EINVAL;
    }

    while (offset < size) {
        int ret = vmm_map_page_raw(space, vaddr + offset, paddr + offset, prot_flags, 0ULL);
        if (ret != 0) {
            if (offset > 0U) {
                (void)nos_vmm_unmap_pages(space, vaddr, offset);
            }
            return ret;
        }
        offset += CONFIG_PAGE_SIZE;
    }
    return 0;
}

int nos_vmm_unmap_pages(struct nos_vm_space *space,
                        uintptr_t vaddr,
                        size_t size)
{
    size_t offset = 0U;

    if (space == NULL || !space->initialized || size == 0U) {
        return -EINVAL;
    }
    if (((vaddr | size) & (CONFIG_PAGE_SIZE - 1U)) != 0U) {
        return -EINVAL;
    }

    while (offset < size) {
        uint64_t *pte = vmm_walk_pte(space, vaddr + offset, false);
        if (pte != NULL && (*pte & NOS_PTE_VALID) != 0ULL) {
            if ((*pte & NOS_PTE_OWNED) != 0ULL) {
                vmm_page_ref_dec_and_free((uintptr_t)(*pte & NOS_PTE_ADDR_MASK));
            }
            *pte = 0ULL;
        }
        offset += CONFIG_PAGE_SIZE;
    }
    return 0;
}

int nos_vmm_translate(struct nos_vm_space *space,
                      uintptr_t vaddr,
                      uintptr_t *out_paddr,
                      uint32_t *out_prot)
{
    uint64_t *pte;
    uintptr_t page_off;

    if (space == NULL || !space->initialized) {
        return -EINVAL;
    }
    pte = vmm_walk_pte(space, vaddr, false);
    if (pte == NULL || (*pte & NOS_PTE_VALID) == 0ULL) {
        return -ENOENT;
    }
    page_off = vaddr & (uintptr_t)(CONFIG_PAGE_SIZE - 1U);
    if (out_paddr != NULL) {
        *out_paddr = (uintptr_t)(*pte & NOS_PTE_ADDR_MASK) + page_off;
    }
    if (out_prot != NULL) {
        *out_prot = vmm_pte_flags_to_prot(*pte);
    }
    return 0;
}

struct nos_vm_area *nos_vm_find_vma(struct nos_vm_space *space, uintptr_t vaddr)
{
    struct list_head *pos;

    if (space == NULL || !space->initialized) {
        return NULL;
    }
    list_for_each(pos, &space->vma_list) {
        struct nos_vm_area *vma = list_entry(pos, struct nos_vm_area, list);
        if (vaddr >= vma->vm_start && vaddr < vma->vm_end) {
            return vma;
        }
    }
    return NULL;
}

static bool vmm_can_merge_vma(const struct nos_vm_area *left,
                              const struct nos_vm_area *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }
    if (left->vm_end != right->vm_start) {
        return false;
    }
    if (left->vm_prot != right->vm_prot || left->vm_flags != right->vm_flags) {
        return false;
    }
    if ((left->vm_flags & NOS_VM_FLAG_ANON) == 0U) {
        uintptr_t expected_phys = left->phys_base + (left->vm_end - left->vm_start);
        if (expected_phys != right->phys_base) {
            return false;
        }
    }
    return true;
}

static void vmm_merge_all_adjacent(struct nos_vm_space *space)
{
    struct list_head *pos = space->vma_list.next;

    while (pos != &space->vma_list && pos->next != &space->vma_list) {
        struct nos_vm_area *cur = list_entry(pos, struct nos_vm_area, list);
        struct nos_vm_area *nxt = list_entry(pos->next, struct nos_vm_area, list);
        if (vmm_can_merge_vma(cur, nxt)) {
            cur->vm_end = nxt->vm_end;
            list_del(&nxt->list);
            kfree(nxt);
            space->vma_count--;
        } else {
            pos = pos->next;
        }
    }
}

static void vmm_insert_vma_sorted(struct nos_vm_space *space, struct nos_vm_area *vma)
{
    struct list_head *pos;

    list_for_each(pos, &space->vma_list) {
        struct nos_vm_area *cur = list_entry(pos, struct nos_vm_area, list);
        if (vma->vm_start < cur->vm_start) {
            vma->list.next = pos;
            vma->list.prev = pos->prev;
            pos->prev->next = &vma->list;
            pos->prev = &vma->list;
            space->vma_count++;
            return;
        }
    }
    list_add_tail(&vma->list, &space->vma_list);
    space->vma_count++;
}

static uintptr_t vmm_find_free_range(struct nos_vm_space *space, uintptr_t hint, size_t size)
{
    uintptr_t candidate = (hint >= space->mmap_base) ? hint : space->mmap_base;
    struct list_head *pos;

    candidate = vmm_align_up(candidate, CONFIG_PAGE_SIZE);
    list_for_each(pos, &space->vma_list) {
        struct nos_vm_area *vma = list_entry(pos, struct nos_vm_area, list);
        if (candidate + size <= vma->vm_start) {
            return candidate;
        }
        if (candidate < vma->vm_end) {
            candidate = vma->vm_end;
        }
    }
    if (candidate + size <= space->mmap_limit) {
        return candidate;
    }
    return 0U;
}

int nos_vm_mmap(struct nos_vm_space *space,
                uintptr_t vaddr_hint,
                size_t size,
                uintptr_t paddr,
                uint32_t prot,
                uint32_t flags,
                uintptr_t *out_vaddr)
{
    struct nos_vm_area *vma;
    uintptr_t start;
    size_t aligned_size;

    if (space == NULL || !space->initialized || size == 0U) {
        return -EINVAL;
    }
    aligned_size = (size_t)vmm_align_up((uintptr_t)size, CONFIG_PAGE_SIZE);

    if ((flags & NOS_VM_FLAG_FIXED) != 0U) {
        if ((vaddr_hint & (CONFIG_PAGE_SIZE - 1U)) != 0U || vaddr_hint == 0U) {
            return -EINVAL;
        }
        start = vaddr_hint;
        (void)nos_vm_munmap(space, start, aligned_size);
    } else {
        start = vmm_find_free_range(space, vaddr_hint, aligned_size);
        if (start == 0U) {
            return -ENOMEM;
        }
    }

    vma = (struct nos_vm_area *)kzalloc(sizeof(*vma), GFP_KERNEL);
    if (vma == NULL) {
        return -ENOMEM;
    }
    vma->vm_start = start;
    vma->vm_end = start + aligned_size;
    vma->phys_base = paddr;
    vma->vm_prot = prot;
    vma->vm_flags = flags;

    if ((flags & NOS_VM_FLAG_DEMAND) == 0U) {
        size_t off = 0U;
        while (off < aligned_size) {
            uintptr_t page_phys;
            uint64_t extra = 0ULL;
            if ((flags & NOS_VM_FLAG_ANON) != 0U && paddr == 0U) {
                page_phys = (uintptr_t)alloc_page(GFP_KERNEL | GFP_ZERO);
                if (page_phys == 0U) {
                    (void)nos_vmm_unmap_pages(space, start, off);
                    kfree(vma);
                    return -ENOMEM;
                }
                vmm_page_ref_inc(page_phys);
                extra = NOS_PTE_OWNED;
            } else {
                page_phys = paddr + off;
            }
            if (vmm_map_page_raw(space, start + off, page_phys, prot, extra) != 0) {
                if ((extra & NOS_PTE_OWNED) != 0ULL) {
                    vmm_page_ref_dec_and_free(page_phys);
                }
                (void)nos_vmm_unmap_pages(space, start, off);
                kfree(vma);
                return -ENOMEM;
            }
            off += CONFIG_PAGE_SIZE;
        }
    }

    vmm_insert_vma_sorted(space, vma);
    vmm_merge_all_adjacent(space);

    if (out_vaddr != NULL) {
        *out_vaddr = start;
    }
    return 0;
}

int nos_vm_munmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    uintptr_t start;
    uintptr_t end;
    struct list_head *pos;
    struct list_head *n;

    if (space == NULL || !space->initialized || size == 0U) {
        return -EINVAL;
    }
    if ((vaddr & (CONFIG_PAGE_SIZE - 1U)) != 0U) {
        return -EINVAL;
    }
    start = vaddr;
    end = vaddr + vmm_align_up((uintptr_t)size, CONFIG_PAGE_SIZE);

    (void)nos_vmm_unmap_pages(space, start, (size_t)(end - start));

    list_for_each_safe(pos, n, &space->vma_list) {
        struct nos_vm_area *vma = list_entry(pos, struct nos_vm_area, list);
        if (vma->vm_end <= start || vma->vm_start >= end) {
            continue;
        }
        if (vma->vm_start >= start && vma->vm_end <= end) {
            list_del(&vma->list);
            kfree(vma);
            space->vma_count--;
        } else if (vma->vm_start < start && vma->vm_end > end) {
            struct nos_vm_area *right = (struct nos_vm_area *)kzalloc(sizeof(*right), GFP_KERNEL);
            if (right == NULL) {
                return -ENOMEM;
            }
            right->vm_start = end;
            right->vm_end = vma->vm_end;
            right->phys_base = (vma->phys_base != 0U) ? (vma->phys_base + (end - vma->vm_start)) : 0U;
            right->vm_prot = vma->vm_prot;
            right->vm_flags = vma->vm_flags;
            vma->vm_end = start;
            list_add(&right->list, &vma->list);
            space->vma_count++;
        } else if (vma->vm_start < start) {
            vma->vm_end = start;
        } else {
            if (vma->phys_base != 0U) {
                vma->phys_base += (end - vma->vm_start);
            }
            vma->vm_start = end;
        }
    }
    return 0;
}

int nos_vm_mprotect(struct nos_vm_space *space,
                    uintptr_t vaddr,
                    size_t size,
                    uint32_t new_prot)
{
    uintptr_t start;
    uintptr_t end;
    uintptr_t cur;
    struct list_head *pos;
    struct list_head *n;
    bool matched = false;

    if (space == NULL || !space->initialized || size == 0U) {
        return -EINVAL;
    }
    if ((vaddr & (CONFIG_PAGE_SIZE - 1U)) != 0U) {
        return -EINVAL;
    }
    start = vaddr;
    end = vaddr + vmm_align_up((uintptr_t)size, CONFIG_PAGE_SIZE);

    list_for_each_safe(pos, n, &space->vma_list) {
        struct nos_vm_area *vma = list_entry(pos, struct nos_vm_area, list);
        if (vma->vm_end <= start || vma->vm_start >= end) {
            continue;
        }
        matched = true;
        if (vma->vm_start >= start && vma->vm_end <= end) {
            vma->vm_prot = new_prot;
        } else if (vma->vm_start < start && vma->vm_end > end) {
            struct nos_vm_area *mid = (struct nos_vm_area *)kzalloc(sizeof(*mid), GFP_KERNEL);
            struct nos_vm_area *right = (struct nos_vm_area *)kzalloc(sizeof(*right), GFP_KERNEL);
            if (mid == NULL || right == NULL) {
                kfree(mid);
                kfree(right);
                return -ENOMEM;
            }
            mid->vm_start = start;
            mid->vm_end = end;
            mid->phys_base = (vma->phys_base != 0U) ? (vma->phys_base + (start - vma->vm_start)) : 0U;
            mid->vm_prot = new_prot;
            mid->vm_flags = vma->vm_flags;

            right->vm_start = end;
            right->vm_end = vma->vm_end;
            right->phys_base = (vma->phys_base != 0U) ? (vma->phys_base + (end - vma->vm_start)) : 0U;
            right->vm_prot = vma->vm_prot;
            right->vm_flags = vma->vm_flags;

            vma->vm_end = start;
            list_add(&mid->list, &vma->list);
            list_add(&right->list, &mid->list);
            space->vma_count += 2U;
        } else if (vma->vm_start < start) {
            struct nos_vm_area *right = (struct nos_vm_area *)kzalloc(sizeof(*right), GFP_KERNEL);
            if (right == NULL) {
                return -ENOMEM;
            }
            right->vm_start = start;
            right->vm_end = vma->vm_end;
            right->phys_base = (vma->phys_base != 0U) ? (vma->phys_base + (start - vma->vm_start)) : 0U;
            right->vm_prot = new_prot;
            right->vm_flags = vma->vm_flags;
            vma->vm_end = start;
            list_add(&right->list, &vma->list);
            space->vma_count++;
        } else {
            struct nos_vm_area *left = (struct nos_vm_area *)kzalloc(sizeof(*left), GFP_KERNEL);
            if (left == NULL) {
                return -ENOMEM;
            }
            left->vm_start = vma->vm_start;
            left->vm_end = end;
            left->phys_base = vma->phys_base;
            left->vm_prot = new_prot;
            left->vm_flags = vma->vm_flags;

            if (vma->phys_base != 0U) {
                vma->phys_base += (end - vma->vm_start);
            }
            vma->vm_start = end;
            left->list.next = &vma->list;
            left->list.prev = vma->list.prev;
            vma->list.prev->next = &left->list;
            vma->list.prev = &left->list;
            space->vma_count++;
        }
    }

    if (!matched) {
        return -ENOENT;
    }

    for (cur = start; cur < end; cur += CONFIG_PAGE_SIZE) {
        uint64_t *pte = vmm_walk_pte(space, cur, false);
        if (pte != NULL && (*pte & NOS_PTE_VALID) != 0ULL) {
            uint64_t keep = *pte & (NOS_PTE_ADDR_MASK | NOS_PTE_VALID | NOS_PTE_COW | NOS_PTE_OWNED);
            *pte = keep | vmm_prot_to_pte_flags(new_prot);
        }
    }

    vmm_merge_all_adjacent(space);
    return 0;
}

int nos_vm_clone(struct nos_vm_space *dst, struct nos_vm_space *src)
{
    struct list_head *pos;

    if (dst == NULL || src == NULL || !dst->initialized || !src->initialized) {
        return -EINVAL;
    }

    list_for_each(pos, &src->vma_list) {
        struct nos_vm_area *svma = list_entry(pos, struct nos_vm_area, list);
        struct nos_vm_area *dvma = (struct nos_vm_area *)kzalloc(sizeof(*dvma), GFP_KERNEL);
        uintptr_t addr;

        if (dvma == NULL) {
            return -ENOMEM;
        }
        *dvma = *svma;
        dvma->vm_flags |= NOS_VM_FLAG_COW;
        svma->vm_flags |= NOS_VM_FLAG_COW;
        list_add_tail(&dvma->list, &dst->vma_list);
        dst->vma_count++;

        for (addr = svma->vm_start; addr < svma->vm_end; addr += CONFIG_PAGE_SIZE) {
            uint64_t *spte = vmm_walk_pte(src, addr, false);
            uint64_t *dpte;
            uintptr_t paddr;

            if (spte == NULL || (*spte & NOS_PTE_VALID) == 0ULL) {
                continue;
            }
            paddr = (uintptr_t)(*spte & NOS_PTE_ADDR_MASK);
            if ((svma->vm_prot & HAL_VM_PROT_WRITE) != 0U) {
                *spte = (*spte & ~NOS_PTE_WRITE) | NOS_PTE_COW;
            }
            dpte = vmm_walk_pte(dst, addr, true);
            if (dpte == NULL) {
                return -ENOMEM;
            }
            *dpte = *spte;
            if ((*spte & NOS_PTE_OWNED) != 0ULL) {
                vmm_page_ref_inc(paddr);
            }
        }
    }
    return 0;
}

int nos_vm_handle_fault(struct nos_vm_space *space,
                        uintptr_t fault_vaddr,
                        uint32_t access_prot)
{
    struct nos_vm_area *vma;
    uintptr_t page_vaddr;
    uint64_t *pte;

    if (space == NULL || !space->initialized) {
        return -EINVAL;
    }
    vma = nos_vm_find_vma(space, fault_vaddr);
    if (vma == NULL) {
        return -EFAULT;
    }
    if ((access_prot & vma->vm_prot) != access_prot) {
        return -EACCES;
    }

    page_vaddr = fault_vaddr & ~(uintptr_t)(CONFIG_PAGE_SIZE - 1U);
    pte = vmm_walk_pte(space, page_vaddr, true);
    if (pte == NULL) {
        return -ENOMEM;
    }

    /* Case 1: Demand-paged unmapped page */
    if ((*pte & NOS_PTE_VALID) == 0ULL) {
        uintptr_t new_page;
        if ((vma->vm_flags & NOS_VM_FLAG_DEMAND) == 0U) {
            return -EFAULT;
        }
        new_page = (uintptr_t)alloc_page(GFP_KERNEL | GFP_ZERO);
        if (new_page == 0U) {
            return -ENOMEM;
        }
        vmm_page_ref_inc(new_page);
        *pte = ((uint64_t)new_page & NOS_PTE_ADDR_MASK) |
               NOS_PTE_VALID |
               NOS_PTE_OWNED |
               vmm_prot_to_pte_flags(vma->vm_prot);
        return 0;
    }

    /* Case 2: Copy-on-Write write fault */
    if ((access_prot & HAL_VM_PROT_WRITE) != 0U && (*pte & NOS_PTE_COW) != 0ULL) {
        uintptr_t old_page = (uintptr_t)(*pte & NOS_PTE_ADDR_MASK);
        uint32_t refs = vmm_page_ref_get(old_page);
        if (refs > 1U) {
            uintptr_t new_page = (uintptr_t)alloc_page(GFP_KERNEL | GFP_ZERO);
            if (new_page == 0U) {
                return -ENOMEM;
            }
            memcpy((void *)new_page, (const void *)old_page, CONFIG_PAGE_SIZE);
            vmm_page_ref_dec_and_free(old_page);
            vmm_page_ref_inc(new_page);
            *pte = ((uint64_t)new_page & NOS_PTE_ADDR_MASK) |
                   NOS_PTE_VALID |
                   NOS_PTE_OWNED |
                   vmm_prot_to_pte_flags(vma->vm_prot);
        } else {
            *pte = (*pte & ~NOS_PTE_COW) | vmm_prot_to_pte_flags(vma->vm_prot);
        }
        return 0;
    }

    return -EACCES;
}

uint32_t nos_vmm_active_pt_pages(void)
{
    return __atomic_load_n(&g_active_pt_pages, __ATOMIC_RELAXED);
}
