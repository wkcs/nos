/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
static unsigned char page_pool[PAGE_POOL_PAGES * NOS_PAGE_SIZE] __attribute__((aligned(NOS_PAGE_SIZE)));
static unsigned short allocation[PAGE_POOL_PAGES];
static struct spinlock page_lock;
#ifndef NOS_MMU
static uintptr_t shared_heap, shared_break;
#endif
void page_init(void) { k_memset(allocation, 0, sizeof(allocation)); }
uintptr_t page_alloc(unsigned count)
{
    uintptr_t irq = spin_lock_irqsave(&page_lock), result = 0;
    if (count && count <= PAGE_POOL_PAGES) {
        for (unsigned i = 0; i + count <= PAGE_POOL_PAGES; i++) {
#ifdef NOS_MPU
            if ((count & (count - 1)) == 0 &&
                (((uintptr_t)page_pool + i * NOS_PAGE_SIZE) & (count * NOS_PAGE_SIZE - 1))) continue;
#endif
            unsigned j = 0;
            while (j < count && !allocation[i + j]) j++;
            if (j == count) {
                allocation[i] = (unsigned short)count;
                for (j = 1; j < count; j++) allocation[i + j] = 0xffff;
                result = (uintptr_t)page_pool + i * NOS_PAGE_SIZE;
                k_memset((void *)result, 0, count * NOS_PAGE_SIZE);
                break;
            }
            i += j;
        }
    }
    spin_unlock_irqrestore(&page_lock, irq);
    return result;
}
int page_free(uintptr_t pa, unsigned count)
{
    if (pa < (uintptr_t)page_pool || pa >= (uintptr_t)page_pool + sizeof(page_pool) ||
        (pa & (NOS_PAGE_SIZE - 1)) || !count) return -NOS_EINVAL;
    unsigned i = (unsigned)((pa - (uintptr_t)page_pool) / NOS_PAGE_SIZE);
    uintptr_t irq = spin_lock_irqsave(&page_lock);
    int rc = -NOS_EINVAL;
    if (i + count <= PAGE_POOL_PAGES && allocation[i] == count) {
        for (unsigned j = 0; j < count; j++) allocation[i + j] = 0;
        rc = 0;
    }
    spin_unlock_irqrestore(&page_lock, irq);
    return rc;
}
unsigned page_available(void)
{
    unsigned free = 0;
    for (unsigned i = 0; i < PAGE_POOL_PAGES; i++) free += !allocation[i];
    return free;
}
/* Address spaces outlive their founding task.  This prevents a reaped thread
 * leader's slot from invalidating mappings still used by another thread. */
struct memory_space {
    unsigned refs, owner, template_text;
    uintptr_t memory, heap, heap_limit, brk;
    struct vma vma[VMA_SLOTS];
};
static struct memory_space memory_spaces[NOS_TASKS];
static int page_free_range(uintptr_t pa, unsigned count);
static struct memory_space *task_space(struct task *task)
{
    return task->space_id < NOS_TASKS && memory_spaces[task->space_id].refs ?
        &memory_spaces[task->space_id] : NULL;
}
static void sync_space(struct task *task, struct memory_space *s)
{
    task->memory = s->memory; task->heap = s->heap;
    task->heap_limit = s->heap_limit; task->brk = s->brk;
    task->space_owner = s->owner;
}
static int space_init(struct task *task, int builtin)
{
    unsigned id;
    for (id = 0; id < NOS_TASKS; id++) if (!memory_spaces[id].refs) break;
    if (id == NOS_TASKS) return -NOS_ENOMEM;
    struct memory_space *s = &memory_spaces[id];
    k_memset(s, 0, sizeof(*s));
    s->memory = page_alloc(USER_BYTES / NOS_PAGE_SIZE);
    if (!s->memory) return -NOS_ENOMEM;
    s->refs = 1; s->owner = task->id; s->template_text = builtin;
    task->space_id = id;
    arch_space_init(id);
#ifdef NOS_MMU
    if (builtin) {
        size_t data = (size_t)(__user_data_end - __user_data_start);
        size_t image = (size_t)(__user_bss_end - __user_data_start);
        if (image > HEAP_OFFSET) kernel_fail("user image exceeds heap boundary");
        k_memcpy((void *)s->memory, __user_data_load, data);
        for (uintptr_t v = (uintptr_t)__user_text_start; v < (uintptr_t)__user_text_end; v += NOS_PAGE_SIZE) {
            if (arch_map(id, v, (uintptr_t)__user_text_load + v - (uintptr_t)__user_text_start,
                         NOS_PROT_READ | NOS_PROT_EXEC)) goto failed;
        }
    }
    for (unsigned n = 0; n < USER_BYTES / NOS_PAGE_SIZE; n++) {
        /* Leave a page between heap and stack unmapped. */
        if (n * NOS_PAGE_SIZE == HEAP_END) continue;
        if (arch_map(id, USER_DATA + n * NOS_PAGE_SIZE, s->memory + n * NOS_PAGE_SIZE,
                     NOS_PROT_READ | NOS_PROT_WRITE)) goto failed;
    }
    s->heap = USER_DATA + HEAP_OFFSET;
    s->heap_limit = USER_DATA + HEAP_END;
#else
    if (!shared_heap) {
        shared_heap = page_alloc(2);
        if (!shared_heap) goto failed;
        shared_break = shared_heap;
    }
    s->heap = shared_heap; s->heap_limit = shared_heap + 2 * NOS_PAGE_SIZE;
    task->tls_memory = s->memory;
#endif
    s->brk = s->heap;
    sync_space(task, s);
    return 0;
failed:
    /* Discard all page-table entries before releasing their physical pages. */
    arch_space_init(id);
    page_free(s->memory, USER_BYTES / NOS_PAGE_SIZE);
    k_memset(s, 0, sizeof(*s)); task->memory = 0;
    return -NOS_ENOMEM;
}
int space_create(struct task *task) { return space_init(task, 1); }
int space_create_empty(struct task *task) { return space_init(task, 0); }
int space_clone(struct task *child, struct task *parent, uintptr_t stack_top)
{
    struct memory_space *s = task_space(parent);
    if (!s || stack_top < 64 || !user_access(parent, stack_top - 64, 64, 1)) return -NOS_EFAULT;
#ifndef NOS_MMU
    child->tls_memory = page_alloc(1);
    if (!child->tls_memory) return -NOS_ENOMEM;
#endif
    child->space_id = parent->space_id;
    s->refs++; sync_space(child, s);
    return 0;
}
void space_destroy(struct task *task)
{
    if (!task->memory) return;
    struct memory_space *s = task_space(task);
    if (!s) return;
#ifndef NOS_MMU
    if (task->tls_memory && task->tls_memory != s->memory) page_free(task->tls_memory, 1);
    task->tls_memory = 0;
#endif
    task->memory = 0;
    if (--s->refs) return;
    /* The departing task may still be running on these tables.  Preserve
     * privileged roots until the next context switch; clear only user PTEs. */
#ifdef NOS_MMU
    if (s->template_text) for (uintptr_t va = (uintptr_t)__user_text_start;
        va < (uintptr_t)__user_text_end; va += NOS_PAGE_SIZE) arch_unmap(task->space_id, va);
    for (unsigned n = 0; n < USER_BYTES / NOS_PAGE_SIZE; n++)
        arch_unmap(task->space_id, USER_DATA + n * NOS_PAGE_SIZE);
#endif
    for (unsigned i = 0; i < VMA_SLOTS; i++) if (s->vma[i].pa)
        for (size_t n = 0; n < s->vma[i].size; n += NOS_PAGE_SIZE)
            arch_unmap(task->space_id, s->vma[i].va + n);
    arch_tlb_sync();
    for (unsigned i = 0; i < VMA_SLOTS; i++) {
        if (s->vma[i].pa) page_free_range(s->vma[i].pa, (unsigned)(s->vma[i].size / NOS_PAGE_SIZE));
    }
    page_free(s->memory, USER_BYTES / NOS_PAGE_SIZE);
    k_memset(s, 0, sizeof(*s));
}
static int inside(uintptr_t ptr, size_t count, uintptr_t base, size_t size)
{
    return ptr >= base && ptr - base <= size && count <= size - (ptr - base);
}
static uintptr_t translate(struct task *task, uintptr_t p, int write)
{
    struct memory_space *s = task_space(task);
    if (!s) return 0;
    uintptr_t text = (uintptr_t)__user_text_start;
    if (s->template_text && !write && inside(p, 1, text, (size_t)(__user_text_end - __user_text_start))) {
#ifdef NOS_MMU
        return (uintptr_t)__user_text_load + p - text;
#else
        return p;
#endif
    }
#ifdef NOS_MMU
    if (inside(p, 1, USER_DATA, USER_BYTES) &&
        !inside(p, 1, USER_DATA + HEAP_END, NOS_PAGE_SIZE)) return s->memory + p - USER_DATA;
#else
    if (inside(p, 1, s->memory, USER_BYTES) ||
        (task->tls_memory && inside(p, 1, task->tls_memory, NOS_PAGE_SIZE)) ||
        inside(p, 1, task->heap, task->heap_limit - task->heap) ||
        inside(p, 1, (uintptr_t)__user_data_start, (size_t)(__user_bss_end - __user_data_start))) return p;
#endif
    for (unsigned i = 0; i < VMA_SLOTS; i++) {
        struct vma *v = &s->vma[i];
        if (v->pa && inside(p, 1, v->va, v->size) &&
            (v->prot & (write ? NOS_PROT_WRITE : NOS_PROT_READ))) return v->pa + p - v->va;
    }
    return 0;
}
intptr_t vm_brk(struct task *task, uintptr_t address)
{
    struct memory_space *s = task_space(task);
    if (!s) return -NOS_EINVAL;
#ifndef NOS_MMU
    s->brk = shared_break;
#endif
    if (!address) return (intptr_t)s->brk;
    if (address < s->heap || address > s->heap_limit) return -NOS_ENOMEM;
    s->brk = address; sync_space(task, s);
#ifndef NOS_MMU
    shared_break = address;
#endif
    return (intptr_t)address;
}
int user_access(struct task *task, uintptr_t ptr, size_t size, int write)
{
    if (!size) return 1;
    if (ptr > UINTPTR_MAX - size) return 0;
    while (size) {
        size_t n = NOS_PAGE_SIZE - (ptr & (NOS_PAGE_SIZE - 1));
        if (n > size) n = size;
        if (!translate(task, ptr, write) || !translate(task, ptr + n - 1, write)) return 0;
        ptr += n;
        size -= n;
    }
    return 1;
}
static int user_copy(struct task *task, uintptr_t user, void *kernel, size_t count, int out)
{
    if (!user_access(task, user, count, out)) return -NOS_EFAULT;
    unsigned char *k = kernel;
    while (count) {
        size_t n = NOS_PAGE_SIZE - (user & (NOS_PAGE_SIZE - 1));
        if (n > count) n = count;
        void *physical = (void *)translate(task, user, out);
        if (out) k_memcpy(physical, k, n); else k_memcpy(k, physical, n);
        count -= n;
        user += n;
        k += n;
    }
    return 0;
}
int user_copy_in(void *dst, uintptr_t src, size_t count) { return user_copy(current_task, src, dst, count, 0); }
int user_copy_out(uintptr_t dst, const void *src, size_t count) { return user_copy(current_task, dst, (void *)src, count, 1); }
int task_copy_out(struct task *task, uintptr_t dst, const void *src, size_t count) { return user_copy(task, dst, (void *)src, count, 1); }
int task_copy_in(struct task *task, void *dst, uintptr_t src, size_t count)
{ return user_copy(task, src, dst, count, 0); }
int task_copy_load(struct task *task, uintptr_t dst, const void *src, size_t count)
{
    if (!user_access(task, dst, count, 0)) return -NOS_EFAULT;
    const unsigned char *k = src;
    while (count) {
        size_t n = NOS_PAGE_SIZE - (dst & (NOS_PAGE_SIZE - 1));
        if (n > count) n = count;
        k_memcpy((void *)translate(task, dst, 0), k, n);
        dst += n; k += n; count -= n;
    }
    arch_cache_sync();
    return 0;
}
static int valid_prot(unsigned prot)
{
    return (!(prot & (NOS_PROT_WRITE | NOS_PROT_EXEC)) || (prot & NOS_PROT_READ)) && !(prot & ~7u) &&
        (prot & (NOS_PROT_WRITE | NOS_PROT_EXEC)) != (NOS_PROT_WRITE | NOS_PROT_EXEC);
}
static int rounded_size(size_t bytes, size_t *result)
{
    if (!bytes || bytes >
#ifdef NOS_MMU
        524288
#else
        65536
#endif
    ) return -NOS_EINVAL;
    *result = (bytes + NOS_PAGE_SIZE - 1) & ~(size_t)(NOS_PAGE_SIZE - 1);

    return 0;
}
static int overlap(uintptr_t a, size_t as, uintptr_t b, size_t bs)
{ return a <= UINTPTR_MAX - as && b <= UINTPTR_MAX - bs && a < b + bs && b < a + as; }
#ifdef NOS_SELFTEST
static int map_fail_after = -1;
#endif
static int map_install(unsigned id, uintptr_t va, uintptr_t pa, unsigned prot)
{
#ifdef NOS_SELFTEST
    if (!map_fail_after) { map_fail_after = -1; return -NOS_ENOMEM; }
    if (map_fail_after > 0) map_fail_after--;
#endif
    return arch_map(id, va, pa, prot);
}
intptr_t vm_map_at(struct task *task, uintptr_t va, unsigned prot, size_t bytes)
{
    struct memory_space *s = task_space(task);
    if (!s || !valid_prot(prot)) return -NOS_EINVAL;
    if (rounded_size(bytes, &bytes)) return -NOS_ENOMEM;
#ifdef NOS_MPU
    size_t power = NOS_PAGE_SIZE;
    while (power < bytes) power <<= 1;
    bytes = power;
#endif
#ifdef NOS_MMU
    if ((va & (NOS_PAGE_SIZE - 1)) || !va || va > UINTPTR_MAX - bytes ||
        overlap(va, bytes, USER_DATA, USER_BYTES) ||
        (overlap(va, bytes, (uintptr_t)__user_text_start,
                       (size_t)(__user_text_end - __user_text_start)))) return -NOS_EINVAL;
#else
    if (va) return -NOS_ENOTSUP;
#endif
#ifdef NOS_MPU
    unsigned used = 3 + (task->tls_memory && task->tls_memory != s->memory);
    for (unsigned j = 0; j < VMA_SLOTS; j++) used += s->vma[j].pa && s->vma[j].prot;
    if (used >= 7) return -NOS_ENOMEM;
#endif
    unsigned slot;
    for (slot = 0; slot < VMA_SLOTS; slot++) if (!s->vma[slot].pa) break;
    if (slot == VMA_SLOTS) return -NOS_ENOMEM;
    for (unsigned i = 0; i < VMA_SLOTS; i++) if (s->vma[i].pa &&
        overlap(va, bytes, s->vma[i].va, s->vma[i].size)) return -NOS_EINVAL;
    uintptr_t pa = page_alloc((unsigned)(bytes / NOS_PAGE_SIZE));
    if (!pa) return -NOS_ENOMEM;
#ifndef NOS_MMU
    va = pa;
#endif
    size_t offset;
    for (offset = 0; offset < bytes; offset += NOS_PAGE_SIZE) {
        if (map_install(task->space_id, va + offset, pa + offset, prot)) {
            /* Never leave a stale PTE referencing pages returned to the pool. */
            while (offset) { offset -= NOS_PAGE_SIZE; arch_unmap(task->space_id, va + offset); }
            arch_tlb_sync(); page_free(pa, (unsigned)(bytes / NOS_PAGE_SIZE));
            return -NOS_ENOMEM;
        }
    }
    struct vma *v = &s->vma[slot];
    v->va = va; v->pa = pa; v->size = bytes; v->prot = prot;
    arch_tlb_sync();
    return (intptr_t)va;
}
intptr_t vm_map(struct task *task, unsigned prot, size_t bytes)
{
    if (!valid_prot(prot)) return -NOS_EINVAL;
    if (!bytes) bytes = NOS_PAGE_SIZE;
#ifdef NOS_MMU
    struct memory_space *s = task_space(task);
    if (!s) return -NOS_EINVAL;
    size_t length;
    if (rounded_size(bytes, &length)) return -NOS_ENOMEM;
    uintptr_t va = 0x02000000u;
    for (;;) {
        if (length > 0x02200000u - va) return -NOS_ENOMEM;
        unsigned moved = 0;
        for (unsigned i = 0; i < VMA_SLOTS; i++) if (s->vma[i].pa &&
            overlap(va, length, s->vma[i].va, s->vma[i].size)) {
            va = s->vma[i].va + s->vma[i].size; moved = 1; break;
        }
        if (!moved) return vm_map_at(task, va, prot, length);
    }
#else
    struct memory_space *s = task_space(task);
    unsigned mappings = 0;
    if (!s) return -NOS_EINVAL;
    for (unsigned i = 0; i < VMA_SLOTS; i++) mappings += !!s->vma[i].pa;
    if (mappings >= 4) return -NOS_ENOMEM;
    return vm_map_at(task, 0, prot, bytes);
#endif
}
/* Split allocator metadata when releasing only part of a contiguous block. */
static int page_free_range(uintptr_t pa, unsigned count)
{
    if (pa < (uintptr_t)page_pool || (pa & (NOS_PAGE_SIZE - 1)) || !count) return -NOS_EINVAL;
    unsigned first = (unsigned)((pa - (uintptr_t)page_pool) / NOS_PAGE_SIZE);
    if (first >= PAGE_POOL_PAGES || count > PAGE_POOL_PAGES - first) return -NOS_EINVAL;
    uintptr_t irq = spin_lock_irqsave(&page_lock);
    unsigned head = first;
    while (head && allocation[head] == 0xffff) head--;
    unsigned total = allocation[head], end = first + count;
    if (!total || total == 0xffff || first < head || end > head + total) {
        spin_unlock_irqrestore(&page_lock, irq); return -NOS_EINVAL;
    }
    if (first > head) allocation[head] = (unsigned short)(first - head);
    for (unsigned i = first; i < end; i++) allocation[i] = 0;
    if (end < head + total) allocation[end] = (unsigned short)(head + total - end);
    spin_unlock_irqrestore(&page_lock, irq);
    return 0;
}
static void invalidate_tls(struct task *task, uintptr_t va, size_t bytes)
{
    for (unsigned i = 0; i < NOS_TASKS; i++) if (tasks[i].memory &&
        tasks[i].space_id == task->space_id && tasks[i].tls_slot >= va &&
        tasks[i].tls_slot - va < bytes) tasks[i].tls_slot = 0;
}
/* Range mutations operate within one allocation, preflight metadata, and
 * commit only after all permissions have been installed successfully. */
static intptr_t vm_change(struct task *task, uintptr_t va, size_t bytes, unsigned prot, int remove)
{
    struct memory_space *s = task_space(task);
    if (!s || (va & (NOS_PAGE_SIZE - 1))) return -NOS_EINVAL;
    if (!remove && !valid_prot(prot)) return -NOS_EINVAL;
    if (!bytes) {
        for (unsigned i = 0; i < VMA_SLOTS; i++) if (s->vma[i].pa && va >= s->vma[i].va &&
            va - s->vma[i].va < s->vma[i].size) { bytes = s->vma[i].size - (va - s->vma[i].va); break; }
    }
    if (rounded_size(bytes, &bytes) || va > UINTPTR_MAX - bytes) return -NOS_EINVAL;
    uintptr_t end = va + bytes, cursor = va;
    /* A thread stack can contain a PROT_NONE guard and several permission
     * intervals.  Validate and mutate the complete range transactionally. */
    while (cursor < end) {
        unsigned found = 0;
        for (unsigned i = 0; i < VMA_SLOTS; i++) if (s->vma[i].pa && cursor >= s->vma[i].va &&
            cursor - s->vma[i].va < s->vma[i].size) {
            uintptr_t next = s->vma[i].va + s->vma[i].size;
            cursor = next < end ? next : end; found = 1; break;
        }
        if (!found) return -NOS_EINVAL;
    }
    struct vma replacement[VMA_SLOTS]; unsigned used = 0;
    k_memset(replacement, 0, sizeof(replacement));
    for (unsigned i = 0; i < VMA_SLOTS; i++) {
        struct vma old = s->vma[i];
        if (!old.pa) continue;
        if (!overlap(va, bytes, old.va, old.size)) {
            if (used == VMA_SLOTS) return -NOS_ENOMEM;
            replacement[used++] = old; continue;
        }
        uintptr_t first = va > old.va ? va : old.va;
        uintptr_t last = end < old.va + old.size ? end : old.va + old.size;
        if (first > old.va) {
            if (used == VMA_SLOTS) return -NOS_ENOMEM;
            replacement[used] = old; replacement[used++].size = first - old.va;
        }
        if (!remove) {
            if (used == VMA_SLOTS) return -NOS_ENOMEM;
            replacement[used] = old; replacement[used].va = first;
            replacement[used].pa += first - old.va; replacement[used].size = last - first;
            replacement[used++].prot = prot;
        }
        if (last < old.va + old.size) {
            if (used == VMA_SLOTS) return -NOS_ENOMEM;
            replacement[used] = old; replacement[used].va = last;
            replacement[used].pa += last - old.va; replacement[used++].size = old.va + old.size - last;
        }
    }
#ifdef NOS_MPU
    unsigned regions = 3 + (task->tls_memory && task->tls_memory != s->memory);
    for (unsigned i = 0; i < used; i++) if (replacement[i].prot) {
        if (!arch_mpu_valid_range(replacement[i].va, replacement[i].size)) return -NOS_ENOTSUP;
        regions++;
    }
    if (regions > 7) return -NOS_ENOMEM;
#endif
    for (unsigned i = 0; i < VMA_SLOTS; i++) {
        struct vma *old = &s->vma[i];
        if (!old->pa || !overlap(va, bytes, old->va, old->size)) continue;
        uintptr_t first = va > old->va ? va : old->va;
        uintptr_t last = end < old->va + old->size ? end : old->va + old->size;
        for (uintptr_t address = first; address < last; address += NOS_PAGE_SIZE) {
            if (remove) arch_unmap(task->space_id, address);
            else if (map_install(task->space_id, address, old->pa + address - old->va, prot)) {
                /* Roll back every affected interval, including earlier VMAs. */
                for (unsigned j = 0; j < VMA_SLOTS; j++) {
                    struct vma *v = &s->vma[j];
                    if (!v->pa || !overlap(va, bytes, v->va, v->size)) continue;
                    uintptr_t begin = va > v->va ? va : v->va;
                    uintptr_t finish = end < v->va + v->size ? end : v->va + v->size;
                    for (uintptr_t p = begin; p < finish; p += NOS_PAGE_SIZE)
                        if (map_install(task->space_id, p, v->pa + p - v->va, v->prot))
                            kernel_fail("protection rollback failed");
                }
                arch_tlb_sync(); return -NOS_ENOMEM;
            }
        }
    }
    /* Complete remote invalidation before any physical page is recycled. */
    arch_tlb_sync();
    if (remove) for (unsigned i = 0; i < VMA_SLOTS; i++) {
        struct vma *old = &s->vma[i];
        if (!old->pa || !overlap(va, bytes, old->va, old->size)) continue;
        uintptr_t first = va > old->va ? va : old->va;
        uintptr_t last = end < old->va + old->size ? end : old->va + old->size;
        if (page_free_range(old->pa + first - old->va, (unsigned)((last - first) / NOS_PAGE_SIZE)))
            kernel_fail("range free failed");
    }
    k_memcpy(s->vma, replacement, sizeof(replacement));
    if (remove || !(prot & NOS_PROT_WRITE)) invalidate_tls(task, va, bytes);
    return 0;
}
intptr_t vm_unmap(struct task *task, uintptr_t va, size_t bytes)
{ return vm_change(task, va, bytes, 0, 1); }
intptr_t vm_protect(struct task *task, uintptr_t va, unsigned prot, size_t bytes)
{
#ifndef NOS_MMU
    (void)task; (void)va; (void)prot; (void)bytes; return -NOS_ENOTSUP;
#else
    return vm_change(task, va, bytes, prot, 0);
#endif
}
uintptr_t task_user_phys(struct task *task, uintptr_t va)
{ return user_access(task, va, 1, 1) ? translate(task, va, 1) : 0; }
uintptr_t task_user_phys_read(struct task *task, uintptr_t va)
{ return user_access(task, va, 1, 0) ? translate(task, va, 0) : 0; }

intptr_t vm_load_protect(struct task *task, uintptr_t va, unsigned prot, size_t bytes)
{ return vm_change(task, va, bytes, prot, 0); }
int user_exec_access(struct task *task, uintptr_t pc)
{
#ifdef NOS_ARM_M
    if (!(pc & 1)) return 0;
    pc &= ~(uintptr_t)1;
#elif defined(NOS_RISCV)
    if (pc & 1) return 0;
#else
    if (pc & 3) return 0;
#endif
    struct memory_space *s = task_space(task);
    if (!s) return 0;
    if (s->template_text && inside(pc, 1, (uintptr_t)__user_text_start,
        (size_t)(__user_text_end - __user_text_start))) return 1;
    for (unsigned i = 0; i < VMA_SLOTS; i++) if (s->vma[i].pa &&
        (s->vma[i].prot & NOS_PROT_EXEC) && inside(pc, 1, s->vma[i].va, s->vma[i].size)) return 1;
    return 0;
}

#ifdef NOS_MPU
unsigned task_mpu_regions(struct task *task, uintptr_t *bases, size_t *sizes, unsigned *flags)
{
    struct memory_space *s = task_space(task);
    if (!s) return 0;
    unsigned n = 0;
    bases[n] = s->memory; sizes[n] = USER_BYTES; flags[n++] = 3;
    bases[n] = s->heap; sizes[n] = s->heap_limit - s->heap; flags[n++] = 3;
    bases[n] = (uintptr_t)__user_data_start;
    sizes[n] = ((size_t)(__user_bss_end - __user_data_start) + NOS_PAGE_SIZE - 1) & ~(size_t)(NOS_PAGE_SIZE - 1);
    flags[n++] = 3;
    if (task->tls_memory && task->tls_memory != s->memory) {
        bases[n] = task->tls_memory; sizes[n] = NOS_PAGE_SIZE; flags[n++] = 3;
    }
    for (unsigned i = 0; i < VMA_SLOTS; i++) if (s->vma[i].pa && s->vma[i].prot) {
        if (n == 7) kernel_fail("MPU region capacity exceeded");
        bases[n] = s->vma[i].va; sizes[n] = s->vma[i].size; flags[n++] = s->vma[i].prot;
    }
    return n;
}
#endif

#ifdef NOS_SELFTEST
int memory_transaction_selftest(void)
{
    unsigned initial = page_available();
    struct task temporary;
    k_memset(&temporary, 0, sizeof(temporary)); temporary.id = UINT32_MAX;
    if (space_create_empty(&temporary)) return 0;
    unsigned allocated = page_available();
    map_fail_after = 1;
    intptr_t mapped = vm_map(&temporary, 3, 2 * NOS_PAGE_SIZE);
    int ok = mapped == -NOS_ENOMEM && page_available() == allocated;
#ifdef NOS_MMU
    ok &= !arch_mapping_flags(temporary.space_id, 0x02000000u);
#endif
    mapped = vm_map(&temporary, 3, 2 * NOS_PAGE_SIZE);
    if ((uintptr_t)mapped >= (uintptr_t)-4095) ok = 0;
    else {
        map_fail_after = 1;
        ok &= vm_change(&temporary, (uintptr_t)mapped, 2 * NOS_PAGE_SIZE, 1, 0) == -NOS_ENOMEM;
        ok &= user_access(&temporary, (uintptr_t)mapped, 2 * NOS_PAGE_SIZE, 1);
#ifdef NOS_MMU
        ok &= (arch_mapping_flags(temporary.space_id, (uintptr_t)mapped) & 3) == 3;
        ok &= (arch_mapping_flags(temporary.space_id, (uintptr_t)mapped + NOS_PAGE_SIZE) & 3) == 3;
#endif
        ok &= !vm_unmap(&temporary, (uintptr_t)mapped, 2 * NOS_PAGE_SIZE);
    }
    map_fail_after = -1;
    space_destroy(&temporary);
    return ok && page_available() == initial;
}
#endif
