/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS AI-Native 统一零拷贝内存对象 (VMO / TensorObj) 内核实现
 */

#define pr_fmt(fmt) "[VMO]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/vmo.h>
#include <nos/cell.h>
#include <nos/notification.h>
#include <nos/hal_arch.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <asm-generic/mmu.h>
#include <string.h>

#ifdef __HOST_TEST__
#include <stdlib.h>
#endif

/* 单个 VMO 最大允许 65536 页（256MB），防止整数溢出与恶意配额冲击 */
#define NOS_VMO_MAX_PAGES   65536U

/* 可选架构批量 ASID 刷新钩子（主机单测与未实现架构安全回退到逐页 HAL） */
__attribute__((weak)) void hal_vm_flush_asid(struct nos_vm_space *space)
{
    (void)space;
}

__attribute__((weak)) int hal_vm_unmap_noflush(struct nos_vm_space *space,
                                               uintptr_t vaddr,
                                               size_t size)
{
    return hal_vm_unmap(space, vaddr, size);
}

__attribute__((weak)) int hal_vm_map_noflush(struct nos_vm_space *space,
                                             uintptr_t vaddr,
                                             uintptr_t paddr,
                                             size_t size,
                                             uint32_t prot_flags)
{
    return hal_vm_map(space, vaddr, paddr, size, prot_flags);
}

__attribute__((weak)) void hal_dcache_sync(uintptr_t va, size_t size, uint32_t dir)
{
    (void)va;
    (void)size;
    (void)dir;
}

__attribute__((weak)) int hal_vm_test_and_clear_af(struct nos_vm_space *space,
                                                   uintptr_t vaddr,
                                                   bool *was_accessed)
{
    (void)space;
    (void)vaddr;
    if (was_accessed != NULL) {
        *was_accessed = false;
    }
    return -ENOSYS;
}

/* 可驱逐 RECOMPUTABLE VMO 全局跟踪链表 */
static spinlock_t g_reclaim_lock;
static struct nos_vmo *g_reclaim_head;
static bool g_reclaim_linked_list_ready;

static void vmo_reclaim_register(struct nos_vmo *vmo)
{
    if (vmo == NULL || (vmo->flags & NOS_VMO_FLAG_RECOMPUTABLE) == 0U) {
        return;
    }
    if (!g_reclaim_linked_list_ready) {
        spin_lock_init(&g_reclaim_lock);
        g_reclaim_linked_list_ready = true;
    }
    spin_lock_irq(&g_reclaim_lock);
    vmo->reclaim_prev = NULL;
    vmo->reclaim_next = g_reclaim_head;
    if (g_reclaim_head != NULL) {
        g_reclaim_head->reclaim_prev = vmo;
    }
    g_reclaim_head = vmo;
    spin_unlock_irq(&g_reclaim_lock);
}

static void vmo_reclaim_unregister(struct nos_vmo *vmo)
{
    if (vmo == NULL || (vmo->flags & NOS_VMO_FLAG_RECOMPUTABLE) == 0U ||
        !g_reclaim_linked_list_ready) {
        return;
    }
    spin_lock_irq(&g_reclaim_lock);
    if (vmo->reclaim_prev != NULL) {
        vmo->reclaim_prev->reclaim_next = vmo->reclaim_next;
    } else if (g_reclaim_head == vmo) {
        g_reclaim_head = vmo->reclaim_next;
    }
    if (vmo->reclaim_next != NULL) {
        vmo->reclaim_next->reclaim_prev = vmo->reclaim_prev;
    }
    vmo->reclaim_prev = NULL;
    vmo->reclaim_next = NULL;
    spin_unlock_irq(&g_reclaim_lock);
}

static int vmo_alloc_4k_page(uintptr_t *out_kv, uintptr_t *out_pa)
{
#ifdef __HOST_TEST__
    void *mem = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    if (mem == NULL) {
        return -ENOMEM;
    }
    memset(mem, 0, PAGE_SIZE);
    *out_kv = (uintptr_t)mem;
    *out_pa = (uintptr_t)mem;
    return 0;
#else
    addr_t va = alloc_page(GFP_KERNEL | GFP_ZERO);
    uintptr_t pa;

    if (va == 0U) {
        return -ENOMEM;
    }
    pa = (uintptr_t)arch_virt_to_phys((unsigned long)va);
    if (pa == 0U) {
        (void)free_page(va);
        return -EFAULT;
    }
    *out_kv = (uintptr_t)va;
    *out_pa = pa;
    return 0;
#endif
}

static void vmo_free_4k_page(uintptr_t kv)
{
    if (kv == 0U) {
        return;
    }
#ifdef __HOST_TEST__
    free((void *)kv);
#else
    (void)free_page((addr_t)kv);
#endif
}

static struct nos_vmo_page *vmo_page_alloc(void)
{
    struct nos_vmo_page *vp;
    uintptr_t kv = 0U;
    uintptr_t pa = 0U;

    vp = kzalloc(sizeof(*vp), GFP_KERNEL);
    if (vp == NULL) {
        return NULL;
    }
    if (vmo_alloc_4k_page(&kv, &pa) != 0) {
        kfree(vp);
        return NULL;
    }

    nos_refcount_init(&vp->refcount, 1U);
    vp->kvaddr = kv;
    vp->paddr = pa;
    return vp;
}

static void vmo_free_phys_block(struct nos_vmo_phys_block *blk)
{
    if (blk == NULL) {
        return;
    }
    if (blk->kvaddr != 0U) {
#ifdef __HOST_TEST__
        free((void *)blk->kvaddr);
#else
        (void)free_pages((addr_t)blk->kvaddr, (u32)blk->order);
#endif
    }
    kfree(blk);
}

static struct nos_vmo_phys_block *vmo_alloc_phys_block(uint8_t order)
{
    struct nos_vmo_phys_block *blk;
    size_t bytes = (size_t)PAGE_SIZE << order;
    uintptr_t kv = 0U;
    uintptr_t pa = 0U;

    blk = kzalloc(sizeof(*blk), GFP_KERNEL);
    if (blk == NULL) {
        return NULL;
    }
#ifdef __HOST_TEST__
    void *mem = aligned_alloc(bytes, bytes);
    if (mem == NULL) {
        kfree(blk);
        return NULL;
    }
    memset(mem, 0, bytes);
    kv = (uintptr_t)mem;
    pa = (uintptr_t)mem;
#else
    addr_t va = alloc_pages(GFP_KERNEL | GFP_ZERO, (u32)order);
    if (va == 0U) {
        kfree(blk);
        return NULL;
    }
    pa = (uintptr_t)arch_virt_to_phys((unsigned long)va);
    if (pa == 0U) {
        (void)free_pages(va, (u32)order);
        kfree(blk);
        return NULL;
    }
    kv = (uintptr_t)va;
#endif
    nos_refcount_init(&blk->refcount, 1U);
    blk->kvaddr = kv;
    blk->paddr = pa;
    blk->order = order;
    return blk;
}

static void vmo_page_get(struct nos_vmo_page *vp)
{
    if (vp != NULL) {
        (void)nos_refcount_get(&vp->refcount);
    }
}

static void vmo_page_put(struct nos_vmo_page *vp)
{
    if (vp != NULL && nos_refcount_put(&vp->refcount)) {
        struct nos_vmo_phys_block *blk = vp->backing_block;
        vp->backing_block = NULL;
        if (blk != NULL) {
            if (nos_refcount_put(&blk->refcount)) {
                vmo_free_phys_block(blk);
            }
        } else {
            vmo_free_4k_page(vp->kvaddr);
        }
        vp->kvaddr = 0U;
        vp->paddr = 0U;
        kfree(vp);
    }
}

/* 将单个 order=4/9 连续物理块拆解进 page 数组，任一子页元分配失败则精确回滚 */
static uint32_t vmo_alloc_contig_pages(struct nos_vmo_page **pages,
                                       uint32_t start_idx,
                                       uint8_t order)
{
    struct nos_vmo_phys_block *blk = vmo_alloc_phys_block(order);
    uint32_t count = (1U << order);
    uint32_t i;

    if (blk == NULL) {
        return 0U;
    }

    for (i = 0U; i < count; i++) {
        struct nos_vmo_page *vp = kzalloc(sizeof(*vp), GFP_KERNEL);
        if (vp == NULL) {
            for (uint32_t j = 0U; j < i; j++) {
                vmo_page_put(pages[start_idx + j]);
                pages[start_idx + j] = NULL;
            }
            if (nos_refcount_put(&blk->refcount)) {
                vmo_free_phys_block(blk);
            }
            return 0U;
        }
        (void)nos_refcount_get(&blk->refcount);
        nos_refcount_init(&vp->refcount, 1U);
        vp->kvaddr = blk->kvaddr + ((uintptr_t)i * PAGE_SIZE);
        vp->paddr = blk->paddr + ((uintptr_t)i * PAGE_SIZE);
        vp->backing_block = blk;
        pages[start_idx + i] = vp;
    }

    /* 退还初始创建者引用，仅由各 4KB vmo_page 持有该连续块 */
    if (nos_refcount_put(&blk->refcount)) {
        vmo_free_phys_block(blk);
    }
    return count;
}

static void vmo_release(struct nos_object *obj)
{
    struct nos_vmo *vmo = (struct nos_vmo *)obj;
    struct nos_vmo_page **pages;
    uint32_t page_count;
    struct nos_notification *notif;
    uint32_t i;

    if (vmo == NULL) {
        return;
    }

    vmo_reclaim_unregister(vmo);

    pages = vmo->pages;
    page_count = vmo->page_count;
    notif = vmo->evict_notif;
    vmo->pages = NULL;
    vmo->page_count = 0U;
    vmo->evict_notif = NULL;

    if (pages != NULL) {
        for (i = 0U; i < page_count; i++) {
            vmo_page_put(pages[i]);
            pages[i] = NULL;
        }
        kfree(pages);
    }

    if (notif != NULL) {
        (void)nos_object_put(&notif->object);
    }

    kfree(vmo);
}

static bool vmo_range_valid(const struct nos_vmo *vmo, size_t offset, size_t len)
{
    if (vmo == NULL || offset > vmo->size_bytes) {
        return false;
    }
    if (len > vmo->size_bytes - offset) {
        return false;
    }
    return true;
}

static bool vmo_addr_range_valid(uintptr_t vaddr, size_t len)
{
    if (len == 0U || (vaddr & (PAGE_SIZE - 1U)) != 0U || (len & (PAGE_SIZE - 1U)) != 0U) {
        return false;
    }
    if (vaddr > UINTPTR_MAX - (uintptr_t)len) {
        return false;
    }
    return true;
}

static struct nos_vmo *vmo_alloc_shell(uint32_t kind,
                                       uint32_t flags,
                                       uint32_t block_bytes,
                                       uint32_t page_count,
                                       size_t size_bytes)
{
    struct nos_vmo *vmo;
    struct nos_vmo_page **pages;

    vmo = kzalloc(sizeof(*vmo), GFP_KERNEL);
    if (vmo == NULL) {
        return NULL;
    }

    pages = kzalloc((uint32_t)(sizeof(struct nos_vmo_page *) * page_count), GFP_KERNEL);
    if (pages == NULL) {
        kfree(vmo);
        return NULL;
    }

    (void)nos_object_init(&vmo->header, false);
    nos_object_set_release(&vmo->header, vmo_release);
    spin_lock_init(&vmo->lock);
    vmo->kind = kind;
    vmo->flags = flags;
    vmo->block_bytes = block_bytes;
    vmo->page_count = page_count;
    vmo->size_bytes = size_bytes;
    vmo->sealed = false;
    vmo->pages = pages;
    vmo->evict_notif = NULL;
    return vmo;
}

struct nos_vmo *nos_vmo_create(const struct nos_vmo_create_args *args)
{
    struct nos_vmo *vmo;
    uint32_t page_count;
    uint32_t block_bytes;
    uint32_t i;

    if (args == NULL || args->size_bytes == 0U) {
        return NULL;
    }
    if ((args->size_bytes % PAGE_SIZE) != 0U) {
        return NULL;
    }
    if (args->kind > (uint32_t)NOS_VMO_DMA_TENSOR) {
        return NULL;
    }
    if ((args->size_bytes / PAGE_SIZE) > NOS_VMO_MAX_PAGES) {
        return NULL;
    }

    page_count = (uint32_t)(args->size_bytes / PAGE_SIZE);
    if (args->block_bytes != 0U) {
        block_bytes = args->block_bytes;
    } else if (args->kind == (uint32_t)NOS_VMO_KVCACHE) {
        block_bytes = 65536U;
    } else {
        block_bytes = (uint32_t)PAGE_SIZE;
    }

    vmo = vmo_alloc_shell(args->kind, args->flags, block_bytes,
                          page_count, (size_t)args->size_bytes);
    if (vmo == NULL) {
        return NULL;
    }

    if ((args->flags & NOS_VMO_FLAG_LAZY_ALLOC) == 0U) {
        i = 0U;
        while (i < page_count) {
            uint32_t remain = page_count - i;
            uint8_t try_order = 0U;

            if ((args->flags & NOS_VMO_FLAG_HUGEPAGE_2M) != 0U && remain >= 512U) {
                try_order = 9U;
            } else if ((args->flags & (NOS_VMO_FLAG_CONTIG_64K | NOS_VMO_FLAG_HUGEPAGE_2M)) != 0U &&
                       remain >= 16U) {
                try_order = 4U;
            }

            if (try_order > 0U) {
                uint32_t blk_pages = vmo_alloc_contig_pages(vmo->pages, i, try_order);
                if (blk_pages > 0U) {
                    i += blk_pages;
                    continue;
                }
                if (try_order == 9U && remain >= 16U) {
                    blk_pages = vmo_alloc_contig_pages(vmo->pages, i, 4U);
                    if (blk_pages > 0U) {
                        i += blk_pages;
                        continue;
                    }
                }
            }

            vmo->pages[i] = vmo_page_alloc();
            if (vmo->pages[i] == NULL) {
                for (uint32_t j = 0U; j < i; j++) {
                    vmo_page_put(vmo->pages[j]);
                    vmo->pages[j] = NULL;
                }
                kfree(vmo->pages);
                vmo->pages = NULL;
                kfree(vmo);
                return NULL;
            }
            i++;
        }
    }

    vmo_reclaim_register(vmo);
    return vmo;
}

static uint64_t vmo_compute_fnv1a_locked(const struct nos_vmo *vmo)
{
    uint64_t hash = 14695981039346656037ULL;
    uint32_t i;

    for (i = 0U; i < vmo->page_count; i++) {
        const struct nos_vmo_page *vp = vmo->pages[i];
        if (vp == NULL || vp->kvaddr == 0U) {
            continue;
        }
        const uint8_t *p = (const uint8_t *)vp->kvaddr;
        for (size_t b = 0U; b < PAGE_SIZE; b++) {
            hash ^= (uint64_t)p[b];
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

int nos_vmo_seal(struct nos_vmo *vmo)
{
    if (vmo == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&vmo->lock);
    if (!vmo->sealed) {
        vmo->content_hash = vmo_compute_fnv1a_locked(vmo);
        vmo->sealed = true;
    }
    smp_wmb();
    spin_unlock_irq(&vmo->lock);
    return 0;
}

uint64_t nos_vmo_content_hash(const struct nos_vmo *vmo)
{
    if (vmo == NULL) {
        return 0ULL;
    }
    return __atomic_load_n(&vmo->content_hash, __ATOMIC_ACQUIRE);
}

struct nos_vmo *nos_vmo_clone_cow(struct nos_vmo *src)
{
    struct nos_vmo *dst;
    uint32_t kind;
    uint32_t flags;
    uint32_t block_bytes;
    uint32_t page_count;
    size_t size_bytes;
    uint32_t i;

    if (src == NULL) {
        return NULL;
    }

    spin_lock_irq(&src->lock);
    kind = src->kind;
    flags = src->flags;
    block_bytes = src->block_bytes;
    page_count = src->page_count;
    size_bytes = src->size_bytes;
    spin_unlock_irq(&src->lock);

    if (page_count == 0U) {
        return NULL;
    }

    /* 先在锁外分配容器内存，彻底避免持 spin_lock_irq 调用分配器 */
    dst = vmo_alloc_shell(kind, flags, block_bytes, page_count, size_bytes);
    if (dst == NULL) {
        return NULL;
    }

    spin_lock_irq(&src->lock);
    for (i = 0U; i < page_count; i++) {
        struct nos_vmo_page *vp = src->pages[i];
        if (vp != NULL) {
            vmo_page_get(vp);
        }
        dst->pages[i] = vp;
    }
    spin_unlock_irq(&src->lock);

    return dst;
}

struct nos_vmo *nos_vmo_clone_kv_prefix(struct nos_vmo *src, size_t prefix_bytes)
{
    struct nos_vmo *dst;
    uint32_t prefix_pages;
    uint32_t page_count;
    uint32_t kind;
    uint32_t flags;
    uint32_t block_bytes;
    size_t size_bytes;
    uint32_t i;

    if (src == NULL || prefix_bytes == 0U || (prefix_bytes % PAGE_SIZE) != 0U) {
        return NULL;
    }

    spin_lock_irq(&src->lock);
    kind = src->kind;
    flags = src->flags;
    block_bytes = src->block_bytes;
    page_count = src->page_count;
    size_bytes = src->size_bytes;
    spin_unlock_irq(&src->lock);

    if (prefix_bytes > size_bytes) {
        return NULL;
    }
    prefix_pages = (uint32_t)(prefix_bytes / PAGE_SIZE);

    dst = vmo_alloc_shell(kind, flags, block_bytes, page_count, size_bytes);
    if (dst == NULL) {
        return NULL;
    }

    /* 1. 锁外预分配 [prefix_pages .. page_count - 1] 的私有后缀 Token 页 */
    for (i = prefix_pages; i < page_count; i++) {
        dst->pages[i] = vmo_page_alloc();
        if (dst->pages[i] == NULL) {
            uint32_t j;
            for (j = prefix_pages; j < i; j++) {
                vmo_page_put(dst->pages[j]);
                dst->pages[j] = NULL;
            }
            kfree(dst->pages);
            dst->pages = NULL;
            kfree(dst);
            return NULL;
        }
    }

    /* 2. 加锁零拷贝共享 [0 .. prefix_pages - 1] 前缀物理页引用 */
    spin_lock_irq(&src->lock);
    for (i = 0U; i < prefix_pages; i++) {
        struct nos_vmo_page *vp = src->pages[i];
        if (vp != NULL) {
            vmo_page_get(vp);
        }
        dst->pages[i] = vp;
    }
    spin_unlock_irq(&src->lock);

    return dst;
}

int nos_vmo_read(struct nos_vmo *vmo, size_t offset, void *buf, size_t len)
{
    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0U;

    if (vmo == NULL || (len > 0U && buf == NULL)) {
        return -EINVAL;
    }
    if (!vmo_range_valid(vmo, offset, len)) {
        return -EINVAL;
    }
    if (len == 0U) {
        return 0;
    }

    spin_lock_irq(&vmo->lock);
    while (done < len) {
        size_t pos = offset + done;
        uint32_t pidx = (uint32_t)(pos / PAGE_SIZE);
        size_t page_off = pos % PAGE_SIZE;
        size_t chunk = min(len - done, PAGE_SIZE - page_off);
        struct nos_vmo_page *vp = vmo->pages[pidx];

        if (vp == NULL) {
            spin_unlock_irq(&vmo->lock);
            return -ENOENT;
        }
        memcpy(dst + done, (const uint8_t *)vp->kvaddr + page_off, chunk);
        done += chunk;
    }
    spin_unlock_irq(&vmo->lock);
    return 0;
}

int nos_vmo_cow_break_page(struct nos_vmo *vmo, uint32_t page_idx, uintptr_t *out_paddr)
{
    struct nos_vmo_page *pin_old;
    struct nos_vmo_page *new_vp;
    struct nos_vmo_page *replaced_vp = NULL;
    uintptr_t res_pa;

    if (vmo == NULL || page_idx >= vmo->page_count) {
        return -EINVAL;
    }

    spin_lock_irq(&vmo->lock);
    if (vmo->sealed) {
        spin_unlock_irq(&vmo->lock);
        return -EACCES;
    }
    pin_old = vmo->pages[page_idx];
    if (pin_old != NULL && nos_refcount_read(&pin_old->refcount) == 1U) {
        res_pa = pin_old->paddr;
        spin_unlock_irq(&vmo->lock);
        if (out_paddr != NULL) {
            *out_paddr = res_pa;
        }
        return 0;
    }
    if (pin_old != NULL) {
        vmo_page_get(pin_old);
    }
    spin_unlock_irq(&vmo->lock);

    new_vp = vmo_page_alloc();
    if (new_vp == NULL) {
        vmo_page_put(pin_old);
        return -ENOMEM;
    }
    if (pin_old != NULL) {
        memcpy((void *)new_vp->kvaddr, (const void *)pin_old->kvaddr, PAGE_SIZE);
        vmo_page_put(pin_old);
    }

    spin_lock_irq(&vmo->lock);
    if (vmo->sealed) {
        spin_unlock_irq(&vmo->lock);
        vmo_page_put(new_vp);
        return -EACCES;
    }
    if (vmo->pages[page_idx] == NULL || nos_refcount_read(&vmo->pages[page_idx]->refcount) > 1U) {
        replaced_vp = vmo->pages[page_idx];
        vmo->pages[page_idx] = new_vp;
        new_vp = NULL;
    }
    res_pa = vmo->pages[page_idx]->paddr;
    smp_wmb();
    spin_unlock_irq(&vmo->lock);

    vmo_page_put(new_vp);
    vmo_page_put(replaced_vp);

    if (out_paddr != NULL) {
        *out_paddr = res_pa;
    }
    return 0;
}

int nos_vmo_restore_from_snapshot(struct nos_vmo *dst, struct nos_vmo *snap)
{
    struct nos_vmo_page *stack_old[16];
    struct nos_vmo_page **old_pages;
    uint32_t page_count;
    uint32_t i;

    if (dst == NULL || snap == NULL || dst == snap || dst->page_count != snap->page_count) {
        return -EINVAL;
    }

    page_count = dst->page_count;
    if (page_count <= (uint32_t)(sizeof(stack_old) / sizeof(stack_old[0]))) {
        old_pages = stack_old;
        memset(old_pages, 0, sizeof(struct nos_vmo_page *) * page_count);
    } else {
        old_pages = kzalloc((uint32_t)(sizeof(struct nos_vmo_page *) * page_count), GFP_KERNEL);
        if (old_pages == NULL) {
            return -ENOMEM;
        }
    }

    /* 固定地址顺序锁双 VMO，彻底避免 AB-BA 死锁 */
    if ((uintptr_t)dst < (uintptr_t)snap) {
        spin_lock_irq(&dst->lock);
        spin_lock_irq(&snap->lock);
    } else {
        spin_lock_irq(&snap->lock);
        spin_lock_irq(&dst->lock);
    }

    if (dst->sealed) {
        spin_unlock_irq((uintptr_t)dst < (uintptr_t)snap ? &snap->lock : &dst->lock);
        spin_unlock_irq((uintptr_t)dst < (uintptr_t)snap ? &dst->lock : &snap->lock);
        if (old_pages != stack_old) {
            kfree(old_pages);
        }
        return -EACCES;
    }

    for (i = 0U; i < page_count; i++) {
        old_pages[i] = dst->pages[i];
        if (snap->pages[i] != NULL) {
            vmo_page_get(snap->pages[i]);
        }
        dst->pages[i] = snap->pages[i];
    }
    smp_wmb();

    if ((uintptr_t)dst < (uintptr_t)snap) {
        spin_unlock_irq(&snap->lock);
        spin_unlock_irq(&dst->lock);
    } else {
        spin_unlock_irq(&dst->lock);
        spin_unlock_irq(&snap->lock);
    }

    for (i = 0U; i < page_count; i++) {
        vmo_page_put(old_pages[i]);
    }
    if (old_pages != stack_old) {
        kfree(old_pages);
    }
    return 0;
}

int nos_vmo_write(struct nos_vmo *vmo, size_t offset, const void *buf, size_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0U;

    if (vmo == NULL || (len > 0U && buf == NULL)) {
        return -EINVAL;
    }
    if (!vmo_range_valid(vmo, offset, len)) {
        return -EINVAL;
    }
    if (len == 0U) {
        return 0;
    }

    while (done < len) {
        size_t pos = offset + done;
        uint32_t pidx = (uint32_t)(pos / PAGE_SIZE);
        size_t page_off = pos % PAGE_SIZE;
        size_t chunk = min(len - done, PAGE_SIZE - page_off);
        struct nos_vmo_page *vp;

        spin_lock_irq(&vmo->lock);
        if (vmo->sealed) {
            spin_unlock_irq(&vmo->lock);
            return -EACCES;
        }
        vp = vmo->pages[pidx];
        if (vp == NULL || nos_refcount_read(&vp->refcount) > 1U) {
            int rc;
            spin_unlock_irq(&vmo->lock);
            rc = nos_vmo_cow_break_page(vmo, pidx, NULL);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        memcpy((uint8_t *)vp->kvaddr + page_off, src + done, chunk);
        smp_wmb();
        spin_unlock_irq(&vmo->lock);

        done += chunk;
    }

    return 0;
}

static int vmo_map_pages_to_space(struct nos_vmo *vmo,
                                  struct nos_vm_space *space,
                                  uintptr_t vaddr,
                                  size_t offset,
                                  size_t len,
                                  uint32_t prot_flags,
                                  bool noflush)
{
    uintptr_t stack_paddrs[16];
    uint32_t first_page;
    uint32_t num_pages;
    uintptr_t *paddrs;
    uint32_t i;
    size_t mapped_bytes = 0U;
    int rc = 0;

    if (vmo == NULL || space == NULL) {
        return -EINVAL;
    }
    if (!vmo_addr_range_valid(vaddr, len) || (offset % PAGE_SIZE) != 0U) {
        return -EINVAL;
    }
    if (!vmo_range_valid(vmo, offset, len)) {
        return -EINVAL;
    }

    /* 硬件 W^X 铁律：严禁同时拥有可写与可执行权限 */
    if ((prot_flags & (HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC)) ==
        (HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC)) {
        return -EPERM;
    }

    first_page = (uint32_t)(offset / PAGE_SIZE);
    num_pages = (uint32_t)(len / PAGE_SIZE);

    if (num_pages <= (uint32_t)(sizeof(stack_paddrs) / sizeof(stack_paddrs[0]))) {
        paddrs = stack_paddrs;
    } else {
        paddrs = kmalloc((uint32_t)(sizeof(uintptr_t) * num_pages), GFP_KERNEL);
        if (paddrs == NULL) {
            return -ENOMEM;
        }
    }

    spin_lock_irq(&vmo->lock);
    if (vmo->sealed && (prot_flags & HAL_VM_PROT_WRITE) != 0U) {
        spin_unlock_irq(&vmo->lock);
        if (paddrs != stack_paddrs) {
            kfree(paddrs);
        }
        return -EACCES;
    }
    for (i = 0U; i < num_pages; i++) {
        struct nos_vmo_page *vp = vmo->pages[first_page + i];
        if (vp == NULL) {
            if ((vmo->flags & (NOS_VMO_FLAG_LAZY_ALLOC | NOS_VMO_FLAG_RECOMPUTABLE)) != 0U) {
                paddrs[i] = 0U;
                continue;
            }
            spin_unlock_irq(&vmo->lock);
            if (paddrs != stack_paddrs) {
                kfree(paddrs);
            }
            return -ENOENT;
        }
        paddrs[i] = vp->paddr;
    }
    spin_unlock_irq(&vmo->lock);

    i = 0U;
    while (i < num_pages) {
        uintptr_t batch_va = vaddr + (uintptr_t)i * PAGE_SIZE;
        uintptr_t batch_pa = paddrs[i];
        uint32_t run = 1U;

        if (batch_pa == 0U) {
            i++;
            continue;
        }

        while ((i + run) < num_pages &&
               paddrs[i + run] != 0U &&
               paddrs[i + run] == batch_pa + (uintptr_t)run * PAGE_SIZE) {
            run++;
        }

        rc = noflush
                 ? hal_vm_map_noflush(space, batch_va, batch_pa, (size_t)run * PAGE_SIZE, prot_flags)
                 : hal_vm_map(space, batch_va, batch_pa, (size_t)run * PAGE_SIZE, prot_flags);
        if (rc != 0) {
            if (mapped_bytes > 0U) {
                (void)hal_vm_unmap(space, vaddr, mapped_bytes);
            }
            if (paddrs != stack_paddrs) {
                kfree(paddrs);
            }
            return rc;
        }

        mapped_bytes += (size_t)run * PAGE_SIZE;
        i += run;
    }

    if (paddrs != stack_paddrs) {
        kfree(paddrs);
    }
    return 0;
}

int nos_vmo_remap_range(struct nos_vmo *vmo,
                        struct nos_vm_space *space,
                        uintptr_t vaddr,
                        size_t offset,
                        size_t len,
                        uint32_t prot_flags)
{
    int rc;

    if (vmo == NULL || space == NULL) {
        return -EINVAL;
    }
    if (!vmo_addr_range_valid(vaddr, len) || (offset % PAGE_SIZE) != 0U) {
        return -EINVAL;
    }
    if (!vmo_range_valid(vmo, offset, len)) {
        return -EINVAL;
    }
    if ((prot_flags & (HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC)) ==
        (HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC)) {
        return -EPERM;
    }

    /* 两阶段合法 BBM 批量重映射：先批量清空旧叶表项，再按 ASID 刷新 TLB 仅 1 次，最后回填新权位 */
    (void)hal_vm_unmap_noflush(space, vaddr, len);
    hal_vm_flush_asid(space);
    rc = vmo_map_pages_to_space(vmo, space, vaddr, offset, len, prot_flags, true);
    if (rc == 0) {
        hal_vm_flush_asid(space);
    }
    return rc;
}

int nos_vmo_map(struct nos_vmo *vmo,
                struct nos_vm_space *space,
                uintptr_t vaddr,
                size_t offset,
                size_t len,
                uint32_t prot_flags,
                struct nos_cell *charge_cell)
{
    int rc;

    if (vmo == NULL || space == NULL) {
        return -EINVAL;
    }
    if (!vmo_addr_range_valid(vaddr, len) || (offset % PAGE_SIZE) != 0U) {
        return -EINVAL;
    }
    if (!vmo_range_valid(vmo, offset, len)) {
        return -EINVAL;
    }
    if ((prot_flags & (HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC)) ==
        (HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC)) {
        return -EPERM;
    }

    size_t charge_bytes = len;

    /* 快速校验只读封存与页面存在，按已分配物理页扣配额再建页表，最后挂载 Cell VMO 跟踪表 */
    spin_lock_irq(&vmo->lock);
    if (vmo->sealed && (prot_flags & HAL_VM_PROT_WRITE) != 0U) {
        spin_unlock_irq(&vmo->lock);
        return -EACCES;
    }
    if ((vmo->flags & NOS_VMO_FLAG_LAZY_ALLOC) != 0U) {
        uint32_t first_p = (uint32_t)(offset / PAGE_SIZE);
        uint32_t num_p = (uint32_t)(len / PAGE_SIZE);
        uint32_t live = 0U;
        for (uint32_t k = 0U; k < num_p; k++) {
            if (vmo->pages[first_p + k] != NULL) {
                live++;
            }
        }
        charge_bytes = (size_t)live * PAGE_SIZE;
    }
    spin_unlock_irq(&vmo->lock);

    if (charge_cell != NULL && charge_bytes > 0U) {
        rc = nos_cell_charge_mem(charge_cell, (uint64_t)charge_bytes);
        if (rc != 0) {
            return rc;
        }
    }

    rc = vmo_map_pages_to_space(vmo, space, vaddr, offset, len, prot_flags, false);
    if (rc != 0) {
        if (charge_cell != NULL && charge_bytes > 0U) {
            nos_cell_uncharge_mem(charge_cell, (uint64_t)charge_bytes);
        }
        return rc;
    }

    if (charge_cell != NULL && space == nos_cell_vm_space(charge_cell)) {
        rc = nos_cell_track_vmo_map(charge_cell, vmo, vaddr, offset, len, prot_flags);
        if (rc != 0) {
            (void)hal_vm_unmap(space, vaddr, len);
            if (charge_bytes > 0U) {
                nos_cell_uncharge_mem(charge_cell, (uint64_t)charge_bytes);
            }
            return rc;
        }
    }

    return 0;
}

int nos_vmo_map_tracked_nocharge(struct nos_vmo *vmo,
                                 struct nos_cell *track_cell,
                                 uintptr_t vaddr,
                                 size_t offset,
                                 size_t len,
                                 uint32_t map_prot_flags,
                                 uint32_t track_prot_flags)
{
    struct nos_vm_space *space;
    int rc;

    if (vmo == NULL || track_cell == NULL) {
        return -EINVAL;
    }
    space = nos_cell_vm_space(track_cell);
    if (space == NULL) {
        return -EINVAL;
    }

    rc = vmo_map_pages_to_space(vmo, space, vaddr, offset, len, map_prot_flags, false);
    if (rc != 0) {
        return rc;
    }

    rc = nos_cell_track_vmo_map(track_cell, vmo, vaddr, offset, len, track_prot_flags);
    if (rc != 0) {
        (void)hal_vm_unmap(space, vaddr, len);
        return rc;
    }
    return 0;
}

int nos_vmo_unmap(struct nos_vm_space *space,
                  uintptr_t vaddr,
                  size_t len,
                  struct nos_cell *charge_cell)
{
    int rc;

    if (space == NULL || !vmo_addr_range_valid(vaddr, len)) {
        return -EINVAL;
    }

    rc = hal_vm_unmap(space, vaddr, len);
    if (rc == 0 && charge_cell != NULL) {
        if (space == nos_cell_vm_space(charge_cell)) {
            nos_cell_untrack_vmo_map(charge_cell, vaddr, len);
        }
        nos_cell_uncharge_mem(charge_cell, (uint64_t)len);
    }
    return rc;
}

int nos_vmo_bind_evict_notif(struct nos_vmo *vmo, struct nos_notification *notif)
{
    struct nos_notification *old_notif = NULL;

    if (vmo == NULL) {
        return -EINVAL;
    }
    if (notif != NULL && !nos_object_get(&notif->object)) {
        return -ESTALE;
    }

    spin_lock_irq(&vmo->lock);
    old_notif = vmo->evict_notif;
    vmo->evict_notif = notif;
    spin_unlock_irq(&vmo->lock);

    if (old_notif != NULL) {
        (void)nos_object_put(&old_notif->object);
    }
    return 0;
}

int nos_vmo_evict_pages(struct nos_vmo *vmo, size_t offset, size_t len)
{
    uint32_t first_page;
    uint32_t num_pages;
    struct nos_vmo_page **evicted;
    struct nos_notification *notif = NULL;
    uint32_t evicted_count = 0U;
    uint32_t i;

    if (vmo == NULL || (offset % PAGE_SIZE) != 0U || (len % PAGE_SIZE) != 0U || len == 0U) {
        return -EINVAL;
    }
    if (!vmo_range_valid(vmo, offset, len)) {
        return -EINVAL;
    }
    if (!(vmo->flags & NOS_VMO_FLAG_RECOMPUTABLE)) {
        return -EPERM;
    }

    first_page = (uint32_t)(offset / PAGE_SIZE);
    num_pages = (uint32_t)(len / PAGE_SIZE);

    evicted = kzalloc((uint32_t)(sizeof(struct nos_vmo_page *) * num_pages), GFP_KERNEL);
    if (evicted == NULL) {
        return -ENOMEM;
    }

    spin_lock_irq(&vmo->lock);
    if (vmo->sealed) {
        spin_unlock_irq(&vmo->lock);
        kfree(evicted);
        return -EACCES;
    }
    for (i = 0U; i < num_pages; i++) {
        if (vmo->pages[first_page + i] != NULL) {
            evicted[i] = vmo->pages[first_page + i];
            vmo->pages[first_page + i] = NULL;
            evicted_count++;
        }
    }
    if (evicted_count > 0U && vmo->evict_notif != NULL &&
        nos_object_get(&vmo->evict_notif->object)) {
        notif = vmo->evict_notif;
    }
    spin_unlock_irq(&vmo->lock);

    for (i = 0U; i < num_pages; i++) {
        vmo_page_put(evicted[i]);
    }
    kfree(evicted);

    if (notif != NULL) {
        (void)nos_notify_signal_obj(notif, 1ULL);
        (void)nos_object_put(&notif->object);
    }

    return 0;
}

int nos_vmo_sync_dma(struct nos_vmo *vmo, size_t offset, size_t len, uint32_t dir)
{
    size_t done = 0U;

    if (vmo == NULL || dir > NOS_DMA_BIDIRECTIONAL) {
        return -EINVAL;
    }
    if (!vmo_range_valid(vmo, offset, len)) {
        return -EINVAL;
    }
    if (len == 0U || (vmo->flags & NOS_VMO_FLAG_IO_COHERENT) != 0U) {
        return 0;
    }

    while (done < len) {
        size_t pos = offset + done;
        uint32_t pidx = (uint32_t)(pos / PAGE_SIZE);
        size_t page_off = pos % PAGE_SIZE;
        size_t chunk = min(len - done, PAGE_SIZE - page_off);
        uintptr_t kv = 0U;

        spin_lock_irq(&vmo->lock);
        if (vmo->pages[pidx] != NULL) {
            kv = vmo->pages[pidx]->kvaddr;
        }
        spin_unlock_irq(&vmo->lock);

        if (kv != 0U) {
            hal_dcache_sync(kv + page_off, chunk, dir);
        }
        done += chunk;
    }
    return 0;
}

int nos_vmo_ensure_page(struct nos_vmo *vmo,
                        uint32_t page_idx,
                        uintptr_t *out_paddr,
                        bool *out_newly_alloc)
{
    struct nos_vmo_page *new_vp = NULL;
    struct nos_vmo_page *existing;
    uintptr_t res_pa;
    bool newly = false;

    if (vmo == NULL || page_idx >= vmo->page_count) {
        return -EINVAL;
    }

    spin_lock_irq(&vmo->lock);
    existing = vmo->pages[page_idx];
    if (existing != NULL) {
        res_pa = existing->paddr;
        spin_unlock_irq(&vmo->lock);
        if (out_paddr != NULL) {
            *out_paddr = res_pa;
        }
        if (out_newly_alloc != NULL) {
            *out_newly_alloc = false;
        }
        return 0;
    }
    if (vmo->sealed) {
        spin_unlock_irq(&vmo->lock);
        return -EACCES;
    }
    spin_unlock_irq(&vmo->lock);

    new_vp = vmo_page_alloc();
    if (new_vp == NULL) {
        return -ENOMEM;
    }

    spin_lock_irq(&vmo->lock);
    if (vmo->sealed) {
        spin_unlock_irq(&vmo->lock);
        vmo_page_put(new_vp);
        return -EACCES;
    }
    if (vmo->pages[page_idx] == NULL) {
        vmo->pages[page_idx] = new_vp;
        res_pa = new_vp->paddr;
        new_vp = NULL;
        newly = true;
    } else {
        res_pa = vmo->pages[page_idx]->paddr;
    }
    smp_wmb();
    spin_unlock_irq(&vmo->lock);

    if (new_vp != NULL) {
        vmo_page_put(new_vp);
    }
    if (out_paddr != NULL) {
        *out_paddr = res_pa;
    }
    if (out_newly_alloc != NULL) {
        *out_newly_alloc = newly;
    }
    return 0;
}

int nos_vmo_reclaim_cold_pages(struct nos_vmo *vmo,
                               struct nos_vm_space *space,
                               uintptr_t map_vaddr,
                               uint32_t max_pages)
{
    uint32_t reclaimed = 0U;
    uint32_t i;

    if (vmo == NULL || space == NULL || (map_vaddr & (PAGE_SIZE - 1U)) != 0U) {
        return -EINVAL;
    }
    if ((vmo->flags & NOS_VMO_FLAG_RECOMPUTABLE) == 0U || vmo->sealed) {
        return -EPERM;
    }

    for (i = 0U; i < vmo->page_count; i++) {
        bool present = false;
        bool was_accessed = false;
        uintptr_t va = map_vaddr + ((uintptr_t)i * PAGE_SIZE);
        int rc;

        if (max_pages > 0U && reclaimed >= max_pages) {
            break;
        }

        spin_lock_irq(&vmo->lock);
        present = (vmo->pages[i] != NULL);
        spin_unlock_irq(&vmo->lock);
        if (!present) {
            continue;
        }

        rc = hal_vm_test_and_clear_af(space, va, &was_accessed);
        if (rc != 0 || was_accessed) {
            continue;
        }

        /* AF == 0 的冷页先解除硬件 PTE，再回收物理页并向绑定者发可重计算通知 */
        if (hal_vm_unmap(space, va, PAGE_SIZE) == 0 &&
            nos_vmo_evict_pages(vmo, (size_t)i * PAGE_SIZE, PAGE_SIZE) == 0) {
            reclaimed++;
        }
    }

    return (int)reclaimed;
}
