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

static void vmo_page_get(struct nos_vmo_page *vp)
{
    if (vp != NULL) {
        (void)nos_refcount_get(&vp->refcount);
    }
}

static void vmo_page_put(struct nos_vmo_page *vp)
{
    if (vp != NULL && nos_refcount_put(&vp->refcount)) {
        vmo_free_4k_page(vp->kvaddr);
        vp->kvaddr = 0U;
        vp->paddr = 0U;
        kfree(vp);
    }
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

    for (i = 0U; i < page_count; i++) {
        vmo->pages[i] = vmo_page_alloc();
        if (vmo->pages[i] == NULL) {
            uint32_t j;
            for (j = 0U; j < i; j++) {
                vmo_page_put(vmo->pages[j]);
                vmo->pages[j] = NULL;
            }
            kfree(vmo->pages);
            vmo->pages = NULL;
            kfree(vmo);
            return NULL;
        }
    }

    return vmo;
}

int nos_vmo_seal(struct nos_vmo *vmo)
{
    if (vmo == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&vmo->lock);
    vmo->sealed = true;
    smp_wmb();
    spin_unlock_irq(&vmo->lock);
    return 0;
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
            struct nos_vmo_page *pin_old = vp;
            struct nos_vmo_page *new_vp;
            struct nos_vmo_page *replaced_vp = NULL;

            if (pin_old != NULL) {
                vmo_page_get(pin_old);
            }
            spin_unlock_irq(&vmo->lock);

            /* 锁外分配新私有物理页执行 CoW 拷贝断裂 */
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
            if (vmo->pages[pidx] == NULL || nos_refcount_read(&vmo->pages[pidx]->refcount) > 1U) {
                replaced_vp = vmo->pages[pidx];
                vmo->pages[pidx] = new_vp;
                new_vp = NULL;
            }
            vp = vmo->pages[pidx];
            memcpy((uint8_t *)vp->kvaddr + page_off, src + done, chunk);
            smp_wmb();
            spin_unlock_irq(&vmo->lock);

            vmo_page_put(new_vp);
            vmo_page_put(replaced_vp);
        } else {
            memcpy((uint8_t *)vp->kvaddr + page_off, src + done, chunk);
            smp_wmb();
            spin_unlock_irq(&vmo->lock);
        }

        done += chunk;
    }

    return 0;
}

int nos_vmo_map(struct nos_vmo *vmo,
                struct nos_vm_space *space,
                uintptr_t vaddr,
                size_t offset,
                size_t len,
                uint32_t prot_flags,
                struct nos_cell *charge_cell)
{
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

    /* 先在锁外分配临时物理页快照数组，不在自旋锁内映射页表或做分配 */
    paddrs = kmalloc((uint32_t)(sizeof(uintptr_t) * num_pages), GFP_KERNEL);
    if (paddrs == NULL) {
        return -ENOMEM;
    }

    spin_lock_irq(&vmo->lock);
    if (vmo->sealed && (prot_flags & HAL_VM_PROT_WRITE) != 0U) {
        spin_unlock_irq(&vmo->lock);
        kfree(paddrs);
        return -EACCES;
    }
    for (i = 0U; i < num_pages; i++) {
        struct nos_vmo_page *vp = vmo->pages[first_page + i];
        if (vp == NULL) {
            spin_unlock_irq(&vmo->lock);
            kfree(paddrs);
            return -ENOENT;
        }
        paddrs[i] = vp->paddr;
    }
    spin_unlock_irq(&vmo->lock);

    /* 映射前先通过绑定的 Agent Cell 原子扣除 mem_bytes 硬配额 */
    if (charge_cell != NULL) {
        rc = nos_cell_charge_mem(charge_cell, (uint64_t)len);
        if (rc != 0) {
            kfree(paddrs);
            return rc;
        }
    }

    /* 合并物理连续页区间以批处理形式写入底层页表 */
    i = 0U;
    while (i < num_pages) {
        uintptr_t batch_va = vaddr + (uintptr_t)i * PAGE_SIZE;
        uintptr_t batch_pa = paddrs[i];
        uint32_t run = 1U;

        while ((i + run) < num_pages &&
               paddrs[i + run] == batch_pa + (uintptr_t)run * PAGE_SIZE) {
            run++;
        }

        rc = hal_vm_map(space, batch_va, batch_pa, (size_t)run * PAGE_SIZE, prot_flags);
        if (rc != 0) {
            if (mapped_bytes > 0U) {
                (void)hal_vm_unmap(space, vaddr, mapped_bytes);
            }
            if (charge_cell != NULL) {
                nos_cell_uncharge_mem(charge_cell, (uint64_t)len);
            }
            kfree(paddrs);
            return rc;
        }

        mapped_bytes += (size_t)run * PAGE_SIZE;
        i += run;
    }

    kfree(paddrs);
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
