/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
static unsigned char page_pool[PAGE_POOL_PAGES * NOS_PAGE_SIZE] __attribute__((aligned(NOS_PAGE_SIZE)));
/* Halfword accesses also work before ARM enables normal-memory mappings. */
static volatile unsigned short allocation[PAGE_POOL_PAGES];
static struct spinlock page_lock;
#ifndef NOS_MMU
static uintptr_t shared_heap, shared_break;
#endif
void page_init(void) { k_memset((void *)allocation, 0, sizeof(allocation)); }
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
    unsigned refs, owner, template_text, gateway;
    uintptr_t memory, heap, heap_limit, brk;
    struct vma vma[VMA_SLOTS];
    uintptr_t linux_brk;
};
static struct memory_space memory_spaces[NOS_TASKS];
/* Physical pages are page aligned.  Odd values mark an unbacked virtual
 * reservation; split reservations retain this tag when their offset changes. */
static int vma_backed(const struct vma *v) { return v->pa && !(v->pa & 1); }

#ifdef NOS_MMU
#define SHARED_OBJECTS 32u
struct shared_object {
    uintptr_t pa; size_t size;
    unsigned generation, creator, pinned;
};
#define SHARED_LEASES 128u
#define SHARED_LEASE_BASE 0x70000000u
#define SHARED_GENERATION_MAX 0xfffffu
struct shared_lease {
    unsigned generation, object, object_generation, prot, count, owners[NOS_TASKS];
};
static struct shared_object shared_objects[SHARED_OBJECTS];
static struct shared_lease shared_leases[SHARED_LEASES];
static struct shared_object *shared_from_backing(uintptr_t pa, size_t bytes)
{
    if (!pa || (pa & 1)) return NULL;
    for (unsigned i = 0; i < SHARED_OBJECTS; i++) {
        struct shared_object *o = &shared_objects[i];
        if (o->pa && pa >= o->pa && pa - o->pa <= o->size && bytes <= o->size - (pa - o->pa)) return o;
    }
    return NULL;
}
static int backing_shared(uintptr_t pa, size_t bytes) { return shared_from_backing(pa, bytes) != NULL; }
static int group_alive(unsigned owner)
{
    for (unsigned i = 0; i < NOS_TASKS; i++) if (memory_spaces[i].refs && memory_spaces[i].owner == owner) return 1;
    return 0;
}
static int lease_has_owner(const struct shared_lease *l, unsigned owner)
{
    for (unsigned i = 0; i < l->count; i++) if (l->owners[i] == owner) return 1;
    return 0;
}
static void lease_remove_owner(struct shared_lease *l, unsigned owner)
{
    for (unsigned i = 0; i < l->count; i++) if (l->owners[i] == owner) {
        l->owners[i] = l->owners[--l->count]; break;
    }
    if (!l->count) l->object = 0;
}
static void lease_collect(void)
{
    for (unsigned i = 0; i < SHARED_LEASES; i++) {
        struct shared_lease *l = &shared_leases[i];
        if (!l->object) continue;
        struct shared_object *o = &shared_objects[l->object - 1];
        if (!o->pa || o->generation != l->object_generation) { l->object = l->count = 0; continue; }
        unsigned n = 0;
        for (unsigned j = 0; j < l->count; j++) if (group_alive(l->owners[j])) l->owners[n++] = l->owners[j];
        l->count = n; if (!n) l->object = 0;
    }
}
static int lease_fork(unsigned parent, unsigned child)
{
    lease_collect();
    for (unsigned i = 0; i < SHARED_LEASES; i++) if (shared_leases[i].object &&
        lease_has_owner(&shared_leases[i], parent) && !lease_has_owner(&shared_leases[i], child) &&
        shared_leases[i].count == NOS_TASKS) return -NOS_ENOMEM;
    for (unsigned i = 0; i < SHARED_LEASES; i++) {
        struct shared_lease *l = &shared_leases[i];
        if (l->object && lease_has_owner(l, parent) && !lease_has_owner(l, child)) l->owners[l->count++] = child;
    }
    return 0;
}
static void lease_exec(unsigned owner)
{
    for (unsigned i = 0; i < SHARED_LEASES; i++) if (shared_leases[i].object) lease_remove_owner(&shared_leases[i], owner);
}
static void shared_collect(void)
{
    lease_collect();
    for (unsigned i = 0; i < SHARED_OBJECTS; i++) {
        struct shared_object *o = &shared_objects[i];
        if (!o->pa || o->pinned) continue;
        unsigned live = 0;
        for (unsigned j = 0; j < SHARED_LEASES; j++) if (shared_leases[j].object == i + 1 &&
            shared_leases[j].object_generation == o->generation && shared_leases[j].count) { live = 1; break; }
        /* A queued process-shared futex retains its backing identity even
         * when another thread removes the last user mapping. */
        for (unsigned j = 0; j < NOS_TASKS && !live; j++) if (tasks[j].state == TASK_FUTEX &&
            tasks[j].futex_shared && tasks[j].futex_key >= o->pa && tasks[j].futex_key - o->pa < o->size) live = 1;
        for (unsigned id = 0; id < NOS_TASKS && !live; id++) if (memory_spaces[id].refs)
            for (unsigned j = 0; j < VMA_SLOTS; j++) {
                struct vma *v = &memory_spaces[id].vma[j];
                if (vma_backed(v) && v->pa >= o->pa && v->pa - o->pa < o->size) { live = 1; break; }
            }
        if (!live) {
            if (page_free(o->pa, (unsigned)(o->size / NOS_PAGE_SIZE))) kernel_fail("shared backing release failed");
            o->pa = 0; o->size = 0; o->pinned = 0;
        }
    }
}
static void shared_owner_exit(unsigned owner)
{
    if (!group_alive(owner)) for (unsigned i = 0; i < SHARED_OBJECTS; i++)
        if (shared_objects[i].pa && shared_objects[i].creator == owner) shared_objects[i].pinned = 0;
}
#else
static int backing_shared(uintptr_t pa, size_t bytes) { (void)pa; (void)bytes; return 0; }
static void shared_collect(void) { }
static void shared_owner_exit(unsigned owner) { (void)owner; }
#endif
static int page_free_range(uintptr_t pa, unsigned count);
static void invalidate_tls(struct task *task, uintptr_t va, size_t bytes);
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
    for (unsigned i = 0; i < VMA_SLOTS; i++) if (vma_backed(&s->vma[i]))
        for (size_t n = 0; n < s->vma[i].size; n += NOS_PAGE_SIZE)
            arch_unmap(task->space_id, s->vma[i].va + n);
    arch_tlb_sync();
    for (unsigned i = 0; i < VMA_SLOTS; i++) {
        if (vma_backed(&s->vma[i]) && !backing_shared(s->vma[i].pa, s->vma[i].size))
            page_free_range(s->vma[i].pa, (unsigned)(s->vma[i].size / NOS_PAGE_SIZE));
    }
    page_free(s->memory, USER_BYTES / NOS_PAGE_SIZE);
    unsigned owner = s->owner;
    k_memset(s, 0, sizeof(*s));
    shared_owner_exit(owner); shared_collect();
}
int space_commit_exec(struct task *task, struct task *replacement)
{
#ifndef NOS_MMU
    (void)task; (void)replacement; return -NOS_ENOTSUP;
#else
    struct memory_space *old = task_space(task), *next = task_space(replacement);
    if (!task->memory || !replacement->memory || !old || !next || old == next) return -NOS_EINVAL;
    /* The caller has stopped the other threads before committing exec.
     * Nothing below validation can fail or discard the prepared new image. */
    if (old->refs != 1 || next->refs != 1) return -NOS_EBUSY;
    unsigned owner = old->owner;
    unsigned id = replacement->space_id;
    next->owner = owner; /* Keep broker ownership alive across exec. */
    lease_exec(owner);
    space_destroy(task);
    task->space_id = id; sync_space(task, next);
    task->tls_memory = replacement->tls_memory;
    task->tls_slot = 0; task->tls_value = 0;
    replacement->memory = replacement->heap = replacement->heap_limit = replacement->brk = 0;
    replacement->tls_memory = 0;
    return 0;
#endif
}
static int inside(uintptr_t ptr, size_t count, uintptr_t base, size_t size)
{
    return ptr >= base && ptr - base <= size && count <= size - (ptr - base);
}
static uintptr_t translate(struct task *task, uintptr_t p, int write)
{
    struct memory_space *s = task_space(task);
    if (!s) return 0;
    uintptr_t helper = !write ? arch_user_helper_phys(p) : 0;
    if (helper) return helper;
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
        if (vma_backed(v) && inside(p, 1, v->va, v->size) &&
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
    if (count && arch_user_helper_phys(dst)) return -NOS_EACCES;
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
        VM_MAP_MAX
#else
        65536
#endif
    ) return -NOS_EINVAL;
    *result = (bytes + NOS_PAGE_SIZE - 1) & ~(size_t)(NOS_PAGE_SIZE - 1);

    return 0;
}
static int mapping_size(size_t bytes, unsigned prot, size_t *result)
{
#ifdef NOS_MMU
    if (!prot) {
        if (!bytes || bytes > VM_USER_END - VM_USER_BASE) return -NOS_EINVAL;
        *result = (bytes + NOS_PAGE_SIZE - 1) & ~(size_t)(NOS_PAGE_SIZE - 1);
        return 0;
    }
#else
    (void)prot;
#endif
    return rounded_size(bytes, result);
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
    return arch_map(id, va, prot ? pa : 0, prot);
}
static int permitted_range(struct memory_space *s, uintptr_t va, size_t bytes, int gateway)
{
#ifdef NOS_MMU
    if ((va & (NOS_PAGE_SIZE - 1)) || va < VM_USER_BASE || bytes > VM_USER_END - va || va >= VM_USER_END)
        return 0;
    if (overlap(va, bytes, USER_DATA, USER_BYTES) || (s->template_text &&
        overlap(va, bytes, (uintptr_t)__user_text_start, (size_t)(__user_text_end - __user_text_start)))) return 0;
    if (!gateway && overlap(va, bytes, 0x01800000u, 0x00200000u)) return 0;
    static const uintptr_t reserved[][2] = {
        {0x03000000u, 0x00200000u}, /* Capability-only device mappings. */
        {0x04000000u, 0x00200000u}, /* CLINT supervisor alias. */
        {0x08000000u, 0x00200000u}, {0x09000000u, 0x00200000u},
        {0x0a000000u, 0x00200000u}, {0x0c000000u, 0x00800000u},
        {0x10000000u, 0x00200000u}
    };
    for (unsigned n = 0; n < sizeof(reserved) / sizeof(reserved[0]); n++)
        if (overlap(va, bytes, reserved[n][0], reserved[n][1])) return 0;
#else
    (void)s; (void)bytes; (void)gateway;
    if (va) return 0;
#endif
    return 1;
}
static intptr_t map_backing(struct task *task, uintptr_t va, unsigned prot, size_t bytes, int gateway, uintptr_t provided)
{
    struct memory_space *s = task_space(task);
    if (!s || !valid_prot(prot)) return -NOS_EINVAL;
    if (mapping_size(bytes, prot, &bytes)) return -NOS_ENOMEM;
#ifdef NOS_MPU
    size_t power = NOS_PAGE_SIZE;
    while (power < bytes) power <<= 1;
    bytes = power;
#endif
    if (!permitted_range(s, va, bytes, gateway)) return -NOS_EINVAL;
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
    uintptr_t pa;
#ifdef NOS_MMU
    pa = provided ? provided : (!prot ? 1 : page_alloc((unsigned)(bytes / NOS_PAGE_SIZE)));
#else
    (void)provided;
    pa = page_alloc((unsigned)(bytes / NOS_PAGE_SIZE));
#endif
    if (!pa) return -NOS_ENOMEM;
#ifndef NOS_MMU
    va = pa;
#endif
    size_t offset;
    for (offset = 0; !(pa & 1) && offset < bytes; offset += NOS_PAGE_SIZE) {
        if (map_install(task->space_id, va + offset, pa + offset, prot)) {
            /* Never leave a stale PTE referencing pages returned to the pool. */
            while (offset) { offset -= NOS_PAGE_SIZE; arch_unmap(task->space_id, va + offset); }
            arch_tlb_sync(); if (!provided) page_free(pa, (unsigned)(bytes / NOS_PAGE_SIZE));
            return -NOS_ENOMEM;
        }
    }
    struct vma *v = &s->vma[slot];
    v->va = va; v->pa = pa; v->size = bytes; v->prot = prot; v->maxprot = 7;
    arch_tlb_sync();
    return (intptr_t)va;
}
static intptr_t map_new(struct task *task, uintptr_t va, unsigned prot, size_t bytes, int gateway)
{ return map_backing(task, va, prot, bytes, gateway, 0); }
int space_fork(struct task *child, struct task *parent)
{
#ifndef NOS_MMU
    (void)child; (void)parent; return -NOS_ENOTSUP;
#else
    struct memory_space *source = task_space(parent);
    if (!source || child->memory) return -NOS_EINVAL;
    int rc = space_init(child, source->template_text);
    if (rc) return rc;
    struct memory_space *target = task_space(child);
    k_memcpy((void *)target->memory, (const void *)source->memory, USER_BYTES);
    target->heap = source->heap; target->heap_limit = source->heap_limit;
    target->brk = source->brk; target->linux_brk = source->linux_brk;
    /* Linux creates a fresh gateway stack for the sole surviving child
     * thread.  Old per-thread gateway stacks are runtime machinery rather
     * than application memory and must not occupy its new slot. */
    for (unsigned i = 0; i < VMA_SLOTS; i++) {
        const struct vma *old = &source->vma[i];
        if (!old->pa || (source->gateway && overlap(old->va, old->size, 0x01900000u, 0x00100000u))) continue;
        int gateway = source->gateway && overlap(old->va, old->size, 0x01800000u, 0x00100000u);
        int shared = backing_shared(old->pa, old->size);
        unsigned initial_prot = !old->prot && vma_backed(old) && !shared ? NOS_PROT_READ : old->prot;
        intptr_t mapped = map_backing(child, old->va, initial_prot, old->size, gateway, shared ? old->pa : 0);
        if ((uintptr_t)mapped >= (uintptr_t)-4095) { rc = (int)mapped; goto failed; }
        for (unsigned j = 0; j < VMA_SLOTS; j++) if (target->vma[j].pa && target->vma[j].va == old->va) {
            if (vma_backed(old) && !shared) k_memcpy((void *)target->vma[j].pa, (const void *)old->pa, old->size);
            target->vma[j].maxprot = old->maxprot;
            break;
        }
        if (initial_prot != old->prot && vm_load_protect(child, old->va, old->prot, old->size)) {
            rc = -NOS_ENOMEM; goto failed;
        }
    }
    rc = lease_fork(source->owner, target->owner);
    if (rc) goto failed;
    target->gateway = source->gateway;
    sync_space(child, target); arch_cache_sync(); return 0;
failed:
    space_destroy(child); return rc;
#endif
}
intptr_t vm_map_at(struct task *task, uintptr_t va, unsigned prot, size_t bytes)
{ return map_new(task, va, prot, bytes, 0); }
int vm_image_map(struct task *task, uintptr_t va, size_t bytes, unsigned prot)
{
    intptr_t result = map_new(task, va, prot, bytes, 0);
    return (uintptr_t)result >= (uintptr_t)-4095 ? (int)result : 0;
}
int vm_gateway_map(struct task *task, uintptr_t va, size_t bytes, unsigned prot)
{
#ifdef NOS_MMU
    if (va < 0x01800000u || va >= 0x01a00000u || bytes > 0x01a00000u - va) return -NOS_EINVAL;
    intptr_t result = map_new(task, va, prot, bytes, 1);
    if ((uintptr_t)result < (uintptr_t)-4095) task_space(task)->gateway = 1;
    return (uintptr_t)result >= (uintptr_t)-4095 ? (int)result : 0;
#else
    (void)task; (void)va; (void)bytes; (void)prot; return -NOS_ENOTSUP;
#endif
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
    unsigned end = first + count;
    /* A coalesced VMA may contain several adjacent allocator blocks.  Check
     * the entire range before changing any allocator metadata. */
    for (unsigned apply = 0; apply < 2; apply++) {
        unsigned cursor = first;
        while (cursor < end) {
            unsigned head = cursor;
            while (head && allocation[head] == 0xffff) head--;
            unsigned total = allocation[head];
            if (!total || total == 0xffff || cursor >= head + total) {
                spin_unlock_irqrestore(&page_lock, irq); return -NOS_EINVAL;
            }
            unsigned stop = end < head + total ? end : head + total;
            if (apply) {
                if (cursor > head) allocation[head] = (unsigned short)(cursor - head);
                for (unsigned i = cursor; i < stop; i++) allocation[i] = 0;
                if (stop < head + total) allocation[stop] = (unsigned short)(head + total - stop);
            }
            cursor = stop;
        }
    }
    spin_unlock_irqrestore(&page_lock, irq);
    return 0;
}
static void invalidate_tls(struct task *task, uintptr_t va, size_t bytes)
{
    for (unsigned i = 0; i < NOS_TASKS; i++) if (tasks[i].memory &&
        tasks[i].space_id == task->space_id && tasks[i].tls_slot >= va &&
        tasks[i].tls_slot - va < bytes) tasks[i].tls_slot = 0;
}
/* Range mutations preflight metadata and backing allocations, then commit
 * only after all permissions have been installed successfully. */
static intptr_t vm_change_mode(struct task *task, uintptr_t va, size_t bytes, unsigned prot, int remove, int holes)
{
    struct memory_space *s = task_space(task);
    if (!s || (va & (NOS_PAGE_SIZE - 1))) return -NOS_EINVAL;
    if (!remove && !valid_prot(prot)) return -NOS_EINVAL;
    if (!bytes) {
        for (unsigned i = 0; i < VMA_SLOTS; i++) if (s->vma[i].pa && va >= s->vma[i].va &&
            va - s->vma[i].va < s->vma[i].size) { bytes = s->vma[i].size - (va - s->vma[i].va); break; }
    }
    if (holes) {
        if (!bytes || bytes > SIZE_MAX - (NOS_PAGE_SIZE - 1)) return -NOS_EINVAL;
        bytes = (bytes + NOS_PAGE_SIZE - 1) & ~(size_t)(NOS_PAGE_SIZE - 1);
    } else if (mapping_size(bytes, remove ? 0 : prot, &bytes)) return -NOS_EINVAL;
    if (va > UINTPTR_MAX - bytes) return -NOS_EINVAL;
    uintptr_t end = va + bytes, cursor = va;
    /* A thread stack can contain a PROT_NONE guard and several permission
     * intervals.  Validate and mutate the complete range transactionally. */
    while (!holes && cursor < end) {
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
        if (!remove && (prot & ~old.maxprot)) return -NOS_EACCES;
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
    unsigned char fresh[VMA_SLOTS];
    k_memset(fresh, 0, sizeof(fresh));
    for (unsigned i = 0; i < used; i++) if ((replacement[i].pa & 1) && replacement[i].prot) {
        replacement[i].pa = page_alloc((unsigned)(replacement[i].size / NOS_PAGE_SIZE));
        if (!replacement[i].pa) {
            for (unsigned j = 0; j < i; j++) if (fresh[j])
                page_free(replacement[j].pa, (unsigned)(replacement[j].size / NOS_PAGE_SIZE));
            return -NOS_ENOMEM;
        }
        fresh[i] = 1;
    }
    for (unsigned i = 0; i < VMA_SLOTS; i++) {
        struct vma *old = &s->vma[i];
        if (!old->pa || !overlap(va, bytes, old->va, old->size)) continue;
        uintptr_t first = va > old->va ? va : old->va;
        uintptr_t last = end < old->va + old->size ? end : old->va + old->size;
        for (uintptr_t address = first; address < last; address += NOS_PAGE_SIZE) {
            uintptr_t physical = old->pa + address - old->va;
            if (!remove && prot && !vma_backed(old)) {
                for (unsigned j = 0; j < used; j++) if (address >= replacement[j].va &&
                    address - replacement[j].va < replacement[j].size) {
                    physical = replacement[j].pa + address - replacement[j].va; break;
                }
            }
            if (remove) arch_unmap(task->space_id, address);
            else if (map_install(task->space_id, address, physical, prot)) {
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
                arch_tlb_sync();
                for (unsigned j = 0; j < used; j++) if (fresh[j])
                    page_free(replacement[j].pa, (unsigned)(replacement[j].size / NOS_PAGE_SIZE));
                return -NOS_ENOMEM;
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
        if (vma_backed(old) && !backing_shared(old->pa, old->size) && page_free_range(old->pa + first - old->va, (unsigned)((last - first) / NOS_PAGE_SIZE)))
            kernel_fail("range free failed");
    }
    k_memcpy(s->vma, replacement, sizeof(replacement));
    shared_collect();
    if (remove || !(prot & NOS_PROT_WRITE)) invalidate_tls(task, va, bytes);
    return 0;
}
static intptr_t vm_change(struct task *task, uintptr_t va, size_t bytes, unsigned prot, int remove)
{ return vm_change_mode(task, va, bytes, prot, remove, 0); }
intptr_t vm_gateway_unmap(struct task *task, uintptr_t va, size_t bytes)
{
    if (va < 0x01800000u || va >= 0x01a00000u || !bytes || bytes > 0x01a00000u - va) return -NOS_EINVAL;
    return vm_change(task, va, bytes, 0, 1);
}
intptr_t vm_unmap(struct task *task, uintptr_t va, size_t bytes)
{
    struct memory_space *s = task_space(task);
    if (s && s->gateway && overlap(va, bytes ? bytes : NOS_PAGE_SIZE, 0x01800000u, 0x00200000u)) return -NOS_EPERM;
    return vm_change(task, va, bytes, 0, 1);
}
intptr_t vm_protect(struct task *task, uintptr_t va, unsigned prot, size_t bytes)
{
#ifndef NOS_MMU
    (void)task; (void)va; (void)prot; (void)bytes; return -NOS_ENOTSUP;
#else
    struct memory_space *s = task_space(task);
    if (s && s->gateway && overlap(va, bytes ? bytes : NOS_PAGE_SIZE, 0x01800000u, 0x00200000u)) return -NOS_EPERM;
    return vm_change(task, va, bytes, prot, 0);
#endif
}
uintptr_t task_user_phys(struct task *task, uintptr_t va)
{ return user_access(task, va, 1, 1) ? translate(task, va, 1) : 0; }
uintptr_t task_user_phys_read(struct task *task, uintptr_t va)
{ return user_access(task, va, 1, 0) ? translate(task, va, 0) : 0; }

uintptr_t task_shared_phys(struct task *task, uintptr_t va)
{
    uintptr_t physical = task_user_phys_read(task, va);
    return physical && backing_shared(physical, 1) ? physical : 0;
}
void vm_shared_collect(void) { shared_collect(); }

intptr_t vm_load_protect(struct task *task, uintptr_t va, unsigned prot, size_t bytes)
{ return vm_change(task, va, bytes, prot, 0); }
int user_exec_access(struct task *task, uintptr_t pc)
{
#ifdef NOS_ARM_M
    if (!(pc & 1)) return 0;
    pc &= ~(uintptr_t)1;
#elif defined(NOS_RISCV)
    if (pc & 1) return 0;
#elif defined(NOS_ARM)
    if (pc & 1) pc &= ~(uintptr_t)1;
    else if (pc & 3) return 0;
#else
    if (pc & 3) return 0;
#endif
    struct memory_space *s = task_space(task);
    if (!s) return 0;
    if (arch_user_helper_phys(pc)) return 1;
    if (s->template_text && inside(pc, 1, (uintptr_t)__user_text_start,
        (size_t)(__user_text_end - __user_text_start))) return 1;
    for (unsigned i = 0; i < VMA_SLOTS; i++) if (vma_backed(&s->vma[i]) &&
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
#if defined(NOS_MMU) && !defined(NOS_RISCV32)
    const uintptr_t image = 0x06000000u;
    const unsigned private_anon = 0x22, fixed = 0x10, noreplace = 0x100000;
    ok &= vm_map_linux(&temporary, image, 3 * NOS_PAGE_SIZE, 0, private_anon | noreplace) == (intptr_t)image;
    ok &= !user_access(&temporary, image, 1, 0) && !arch_mapping_flags(temporary.space_id, image);
    ok &= vm_map_linux(&temporary, image, NOS_PAGE_SIZE, 3, private_anon | noreplace) == -NOS_EEXIST;
    ok &= vm_map_linux(&temporary, image + NOS_PAGE_SIZE, NOS_PAGE_SIZE, 3, private_anon | fixed) == (intptr_t)(image + NOS_PAGE_SIZE);
    unsigned word = 0x51c0ffee, readback = 0;
    ok &= !task_copy_out(&temporary, image + NOS_PAGE_SIZE, &word, sizeof(word));
    unsigned reserved = page_available();
    map_fail_after = 1;
    ok &= vm_map_linux(&temporary, image, 3 * NOS_PAGE_SIZE, 5, private_anon | fixed) == -NOS_ENOMEM;
    ok &= page_available() == reserved;
    ok &= !arch_mapping_flags(temporary.space_id, image) &&
        (arch_mapping_flags(temporary.space_id, image + NOS_PAGE_SIZE) & 3) == 3 &&
        !arch_mapping_flags(temporary.space_id, image + 2 * NOS_PAGE_SIZE);
    ok &= !task_copy_in(&temporary, &readback, image + NOS_PAGE_SIZE, sizeof(readback)) && readback == word;
    ok &= vm_map_linux(&temporary, image, NOS_PAGE_SIZE, 7, private_anon | fixed) == -NOS_EINVAL;
    ok &= !vm_linux_unmap(&temporary, image, 4 * NOS_PAGE_SIZE); /* Includes a hole. */
    ok &= !vm_linux_unmap(&temporary, image, 4 * NOS_PAGE_SIZE);
    ok &= vm_map_linux(&temporary, image, NOS_PAGE_SIZE, 2, private_anon | noreplace) == (intptr_t)image;
    ok &= (arch_mapping_flags(temporary.space_id, image) & 7) == 3;
    ok &= !vm_linux_protect(&temporary, image, 4, NOS_PAGE_SIZE);
    ok &= (arch_mapping_flags(temporary.space_id, image) & 7) == 5;
    ok &= vm_linux_protect(&temporary, image, 6, NOS_PAGE_SIZE) == -NOS_EINVAL;
    ok &= !vm_linux_unmap(&temporary, image, NOS_PAGE_SIZE);
    ok &= vm_map_linux(&temporary, 0x0ffff000u, NOS_PAGE_SIZE, 3, private_anon | noreplace) == (intptr_t)0x0ffff000u;
    ok &= !vm_linux_unmap(&temporary, 0x0ffff000u, NOS_PAGE_SIZE);
    ok &= vm_map_linux(&temporary, USER_DATA, NOS_PAGE_SIZE, 3, private_anon | fixed) == -NOS_EINVAL;
    ok &= vm_map_linux(&temporary, 0x08000000u, NOS_PAGE_SIZE, 3, private_anon | fixed) == -NOS_EINVAL;
    ok &= !vm_gateway_map(&temporary, 0x01901000u, 2 * NOS_PAGE_SIZE, 3);
    ok &= vm_unmap(&temporary, 0x01901000u, 2 * NOS_PAGE_SIZE) == -NOS_EPERM;
    ok &= vm_map_linux(&temporary, 0x01901000u, NOS_PAGE_SIZE, 3, private_anon | fixed) == -NOS_EINVAL;
    ok &= !vm_gateway_unmap(&temporary, 0x01901000u, 2 * NOS_PAGE_SIZE);
    /* Reclaim sparse page tables: map more distinct blocks than the default
     * capacity, keeping only one transient mapping alive at a time. */
    unsigned exercised = 0;
    for (uintptr_t va = VM_USER_BASE; va < VM_USER_END && exercised < 48; va += 0x200000u) {
        if (!permitted_range(task_space(&temporary), va, NOS_PAGE_SIZE, 0)) continue;
        ok &= vm_map_linux(&temporary, va, NOS_PAGE_SIZE, 3, private_anon | noreplace) == (intptr_t)va;
        ok &= !vm_linux_unmap(&temporary, va, NOS_PAGE_SIZE); exercised++;
    }
    ok &= exercised == 48;
    uintptr_t start = (uintptr_t)vm_linux_brk(&temporary, 0);
    for (unsigned i = 1; i <= 64; i++)
        ok &= vm_linux_brk(&temporary, start + i * NOS_PAGE_SIZE) == (intptr_t)(start + i * NOS_PAGE_SIZE);
    ok &= vm_linux_brk(&temporary, start) == (intptr_t)start;
#ifdef NOS_LINUX_COMPAT
    unsigned before_reservation = page_available();
    intptr_t large_result = vm_map_linux(&temporary, 0, 128u * 1024u * 1024u, 0, private_anon | 0x4000u);
    uintptr_t large = (uintptr_t)large_result;
    int has_reservation = large < (uintptr_t)-4095;
    ok &= has_reservation && page_available() == before_reservation;
    if (has_reservation) {
        ok &= !user_access(&temporary, large, 1, 0) && !task_user_phys_read(&temporary, large);
        ok &= !vm_protect(&temporary, large + NOS_PAGE_SIZE, 3, 2 * NOS_PAGE_SIZE);
        ok &= page_available() == before_reservation - 2;
        ok &= !task_copy_out(&temporary, large + NOS_PAGE_SIZE, &word, sizeof(word));
        unsigned before_commit = page_available(); map_fail_after = 1;
        ok &= vm_protect(&temporary, large + 4 * NOS_PAGE_SIZE, 3, 2 * NOS_PAGE_SIZE) == -NOS_ENOMEM;
        ok &= page_available() == before_commit && !user_access(&temporary, large + 4 * NOS_PAGE_SIZE, 1, 0);
        ok &= !arch_mapping_flags(temporary.space_id, large + 4 * NOS_PAGE_SIZE);
        ok &= !vm_protect(&temporary, large + NOS_PAGE_SIZE, 0, 2 * NOS_PAGE_SIZE);
        ok &= !vm_protect(&temporary, large + NOS_PAGE_SIZE, 3, 2 * NOS_PAGE_SIZE);
        ok &= !task_copy_in(&temporary, &readback, large + NOS_PAGE_SIZE, sizeof(readback)) && readback == word;
        ok &= !vm_protect(&temporary, large + NOS_PAGE_SIZE, 0, 2 * NOS_PAGE_SIZE);
    }
    ok &= vm_map_linux(&temporary, VM_USER_END - NOS_PAGE_SIZE, NOS_PAGE_SIZE, 3, private_anon | noreplace) == (intptr_t)(VM_USER_END - NOS_PAGE_SIZE);
    ok &= !vm_linux_unmap(&temporary, VM_USER_END - NOS_PAGE_SIZE, NOS_PAGE_SIZE);
    ok &= vm_map_linux(&temporary, VM_USER_END, NOS_PAGE_SIZE, 3, private_anon | fixed) == -NOS_EINVAL;
    ok &= vm_map_linux(&temporary, 0x10000000u, NOS_PAGE_SIZE, 3, private_anon | fixed) == -NOS_EINVAL;
#endif
    /* Fork must preserve private bytes and all permissions, and reclaim a
     * partially constructed child without touching its parent's backing. */
    ok &= vm_map_linux(&temporary, image, 3 * NOS_PAGE_SIZE, 3, private_anon | noreplace) == (intptr_t)image;
    ok &= !task_copy_out(&temporary, image, &word, sizeof(word));
    ok &= !task_copy_out(&temporary, USER_DATA, &word, sizeof(word));
    ok &= !vm_protect(&temporary, image + NOS_PAGE_SIZE, 0, NOS_PAGE_SIZE);
    ok &= !vm_protect(&temporary, image + 2 * NOS_PAGE_SIZE, 5, NOS_PAGE_SIZE);
    struct task child; k_memset(&child, 0, sizeof(child)); child.id = UINT32_MAX - 1;
    reserved = page_available(); map_fail_after = 1;
    ok &= space_fork(&child, &temporary) == -NOS_ENOMEM && page_available() == reserved;
    int forked = space_fork(&child, &temporary);
    ok &= !forked;
    if (!forked) {
        ok &= task_user_phys_read(&child, image) != task_user_phys_read(&temporary, image);
        ok &= !task_copy_in(&child, &readback, image, sizeof(readback)) && readback == word;
        ok &= !task_copy_in(&child, &readback, USER_DATA, sizeof(readback)) && readback == word;
        ok &= !user_access(&child, image + NOS_PAGE_SIZE, 1, 0);
        ok &= (arch_mapping_flags(child.space_id, image + 2 * NOS_PAGE_SIZE) & 7) == 5;
        readback = word + 1;
        ok &= !task_copy_out(&child, image, &readback, sizeof(readback));
        ok &= !task_copy_in(&temporary, &readback, image, sizeof(readback)) && readback == word;
#ifdef NOS_LINUX_COMPAT
        if (has_reservation) {
            ok &= !user_access(&child, large, 1, 0);
            ok &= !vm_protect(&child, large + NOS_PAGE_SIZE, 3, 2 * NOS_PAGE_SIZE);
            ok &= !task_copy_in(&child, &readback, large + NOS_PAGE_SIZE, sizeof(readback)) && readback == word;
            ok &= !user_access(&temporary, large + NOS_PAGE_SIZE, 1, 0);
        }
#endif
        space_destroy(&child);
    }
    ok &= page_available() == reserved;
    ok &= !vm_linux_unmap(&temporary, image, 3 * NOS_PAGE_SIZE);
#ifdef NOS_LINUX_COMPAT
    if (has_reservation) ok &= !vm_linux_unmap(&temporary, large, 128u * 1024u * 1024u);
#endif
    /* File-style shared objects use FD-scoped leases, never raw PAs or
     * a process-wide OR of previously granted permissions. */
    unsigned shared_before = page_available(); temporary.caps |= NOS_CAP_SHARED_BROKER;
    intptr_t object = vm_shared_create(&temporary, 2 * NOS_PAGE_SIZE);
    ok &= (uintptr_t)object >= NOS_SHARED_BASE && (uintptr_t)object < SHARED_LEASE_BASE;
    if ((uintptr_t)object < (uintptr_t)-4095) {
        k_memset(&child, 0, sizeof(child)); child.id = UINT32_MAX - 1;
        int made = space_create_empty(&child); ok &= !made;
        if (!made) {
            ok &= vm_shared_map(&temporary, (unsigned)object, image, 2 * NOS_PAGE_SIZE, 0, 3, 1) == (intptr_t)image;
            ok &= vm_shared_map(&child, (unsigned)object, image, NOS_PAGE_SIZE, 0, 1, 1) == -NOS_EACCES;
            int rw = vm_shared_grant(&temporary, (unsigned)object, &child, 3);
            int ro = vm_shared_grant(&temporary, (unsigned)object, &child, 1);
            ok &= (unsigned)rw >= SHARED_LEASE_BASE && (unsigned)rw < NOS_SHARED_LIMIT && rw != ro;
            ok &= vm_shared_map(&child, (unsigned)rw, image + 0x200000u, 2 * NOS_PAGE_SIZE, 0, 1, 1) == (intptr_t)(image + 0x200000u);
            ok &= vm_linux_protect(&child, image + 0x200000u, 2, 2 * NOS_PAGE_SIZE) == 0;
            ok &= vm_shared_map(&child, (unsigned)ro, image + 0x400000u, NOS_PAGE_SIZE, 0, 1, 1) == (intptr_t)(image + 0x400000u);
            ok &= vm_linux_protect(&child, image + 0x400000u, 3, NOS_PAGE_SIZE) == -NOS_EACCES;
            ok &= vm_shared_map(&child, (unsigned)ro, 0, NOS_PAGE_SIZE, 0, 1, 1) == -NOS_EINVAL;
            ok &= task_user_phys_read(&temporary, image) == task_user_phys_read(&child, image + 0x200000u);
            ok &= !task_copy_out(&child, image + 0x200000u, &word, sizeof(word));
            ok &= !task_copy_in(&temporary, &readback, image, sizeof(readback)) && readback == word;
            ok &= !vm_linux_sync(&child, image + 0x200000u, 2 * NOS_PAGE_SIZE, 4);
            ok &= vm_linux_sync(&child, image + 0x200001u, NOS_PAGE_SIZE, 4) == -NOS_EINVAL;
            ok &= vm_linux_sync(&child, image + 0x200000u, 2 * NOS_PAGE_SIZE, 5) == -NOS_EINVAL;
            ok &= vm_linux_sync(&child, image + 0x200000u, 3 * NOS_PAGE_SIZE, 4) == -NOS_ENOMEM;
            int bad = vm_shared_grant(&temporary, (unsigned)object, &child, 3);
            ok &= vm_shared_map(&child, (unsigned)bad, 0, NOS_PAGE_SIZE, 1, 3, 1) == -NOS_EINVAL;
            ok &= vm_shared_map(&child, (unsigned)bad, 0, NOS_PAGE_SIZE, 0, 3, 1) == -NOS_EINVAL;
            bad = vm_shared_grant(&temporary, (unsigned)object, &child, 1);
            ok &= vm_shared_map(&temporary, (unsigned)bad, 0, NOS_PAGE_SIZE, 0, 1, 1) == -NOS_EACCES;
            ok &= !vm_shared_release(&temporary, (unsigned)bad);
            ok &= vm_shared_map(&child, (unsigned)bad, 0, NOS_PAGE_SIZE, 0, 1, 1) == -NOS_EINVAL;
            space_destroy(&child);
            int pending = vm_shared_grant(&temporary, (unsigned)object, &temporary, 1);
            k_memset(&child, 0, sizeof(child)); child.id = UINT32_MAX - 1;
            made = space_fork(&child, &temporary); ok &= !made;
            if (!made) {
                ok &= task_user_phys_read(&temporary, image) == task_user_phys_read(&child, image);
                ok &= vm_shared_map(&child, (unsigned)pending, image + 0x200000u, NOS_PAGE_SIZE, 0, 1, 1) == (intptr_t)(image + 0x200000u);
                ok &= vm_shared_map(&temporary, (unsigned)pending, image + 0x400000u, NOS_PAGE_SIZE, 0, 1, 1) == (intptr_t)(image + 0x400000u);
                ok &= !vm_shared_release(&temporary, (unsigned)object);
                ok &= !vm_linux_unmap(&temporary, image, 2 * NOS_PAGE_SIZE);
                ok &= !vm_linux_unmap(&temporary, image + 0x400000u, NOS_PAGE_SIZE);
                ok &= !task_copy_in(&child, &readback, image + 0x200000u, sizeof(readback)) && readback == word;
                ok &= vm_linux_protect(&child, image + 0x200000u, 3, NOS_PAGE_SIZE) == -NOS_EACCES;
                space_destroy(&child);
                ok &= vm_shared_map(&temporary, (unsigned)object, 0, NOS_PAGE_SIZE, 0, 1, 1) == -NOS_EINVAL;
            }
        }
    }
    ok &= page_available() == shared_before;
    /* Anonymous MAP_SHARED and fork must refer to the very same pages. */
    ok &= vm_map_linux(&temporary, image, 2 * NOS_PAGE_SIZE, 2, 0x21u | noreplace) == (intptr_t)image;
    k_memset(&child, 0, sizeof(child)); child.id = UINT32_MAX - 1;
    forked = space_fork(&child, &temporary); ok &= !forked;
    if (!forked) {
        ok &= task_user_phys(&child, image) == task_user_phys(&temporary, image);
        ok &= !task_copy_out(&child, image, &word, sizeof(word));
        ok &= !task_copy_in(&temporary, &readback, image, sizeof(readback)) && readback == word;
        ok &= !vm_linux_unmap(&temporary, image, NOS_PAGE_SIZE);
        ok &= !task_copy_in(&child, &readback, image, sizeof(readback)) && readback == word;
        space_destroy(&child);
    }
    /* Force a queued physical futex reference to outlive its final VMA:
     * the allocator must not recycle that physical identity until dequeue. */
    struct task *queued = NULL;
    for (unsigned i = 0; i < NOS_TASKS; i++) if (tasks[i].state == TASK_UNUSED) { queued = &tasks[i]; break; }
    unsigned saved_shared = queued ? queued->futex_shared : 0;
    uintptr_t saved_key = queued ? queued->futex_key : 0;
    ok &= queued != NULL;
    if (queued) {
        queued->futex_shared = 1; queued->futex_key = task_user_phys_read(&temporary, image + NOS_PAGE_SIZE);
        queued->state = TASK_FUTEX;
    }
    ok &= !vm_linux_unmap(&temporary, image + NOS_PAGE_SIZE, NOS_PAGE_SIZE);
    if (queued) {
        ok &= page_available() == shared_before - 2;
        queued->state = TASK_UNUSED; queued->futex_shared = saved_shared; queued->futex_key = saved_key;
        vm_shared_collect();
    }
    ok &= page_available() == shared_before;
    /* Exec transfers the prepared space reference while preserving identity.
     * A busy old space must remain unchanged rather than partially commit. */
    k_memset(&child, 0, sizeof(child)); child.id = UINT32_MAX - 1;
    if (space_create_empty(&child)) ok = 0;
    else {
        unsigned old_id = temporary.space_id, next_id = child.space_id;
        uintptr_t old_memory = temporary.memory;
        ok &= !task_copy_out(&child, USER_DATA, &word, sizeof(word));
        task_space(&temporary)->refs++;
        ok &= space_commit_exec(&temporary, &child) == -NOS_EBUSY && temporary.space_id == old_id && temporary.memory == old_memory;
        task_space(&temporary)->refs--;
        ok &= !space_commit_exec(&temporary, &child) && !child.memory && temporary.space_id == next_id && temporary.id == UINT32_MAX;
        ok &= !task_copy_in(&temporary, &readback, USER_DATA, sizeof(readback)) && readback == word;
        space_destroy(&child); /* The transferred reference is no longer owned. */
    }
#endif
    space_destroy(&temporary);
    return ok && page_available() == initial;
}
#endif

#ifdef NOS_MMU
static struct vma *find_vma(struct memory_space *s, uintptr_t address)
{
    for (unsigned n = 0; n < VMA_SLOTS; n++) if (s->vma[n].pa && address >= s->vma[n].va &&
        address - s->vma[n].va < s->vma[n].size) return &s->vma[n];
    return NULL;
}
static intptr_t fixed_backing(struct task *task, uintptr_t va, size_t bytes, unsigned prot, int replace, uintptr_t provided)
{
    struct memory_space *s = task_space(task);
    if (!s || !valid_prot(prot) || mapping_size(bytes, prot, &bytes) || !permitted_range(s, va, bytes, 0)) return -NOS_EINVAL;
    unsigned collided = 0;
    for (unsigned i = 0; i < VMA_SLOTS; i++) collided |= s->vma[i].pa && overlap(va, bytes, s->vma[i].va, s->vma[i].size);
    if (!collided) return map_backing(task, va, prot, bytes, 0, provided);
    if (!replace) return -NOS_EEXIST;
    uintptr_t end = va + bytes;
    struct vma replacement[VMA_SLOTS]; unsigned used = 0;
    k_memset(replacement, 0, sizeof(replacement));
    for (unsigned i = 0; i < VMA_SLOTS; i++) {
        struct vma old = s->vma[i];
        if (!old.pa) continue;
        if (!overlap(va, bytes, old.va, old.size)) {
            if (used == VMA_SLOTS) return -NOS_ENOMEM;
            replacement[used++] = old; continue;
        }
        if (old.va < va) {
            if (used == VMA_SLOTS) return -NOS_ENOMEM;
            replacement[used] = old; replacement[used++].size = va - old.va;
        }
        if (old.va + old.size > end) {
            if (used == VMA_SLOTS) return -NOS_ENOMEM;
            replacement[used] = old; replacement[used].va = end;
            replacement[used].pa += end - old.va; replacement[used++].size = old.va + old.size - end;
        }
    }
    if (used == VMA_SLOTS) return -NOS_ENOMEM;
    uintptr_t pa = provided ? provided : (!prot ? 1 : page_alloc((unsigned)(bytes / NOS_PAGE_SIZE)));
    if (!pa) return -NOS_ENOMEM;
    /* Changing an output address requires break-before-make on ARM/A64.
     * Complete the broadcast before publishing replacement PTEs or reusing
     * an empty page-table slot.  The old physical backing remains reserved. */
    for (size_t n = 0; n < bytes; n += NOS_PAGE_SIZE) arch_unmap(task->space_id, va + n);
    arch_tlb_sync();
    size_t offset;
    for (offset = 0; offset < bytes; offset += NOS_PAGE_SIZE) {
        if (map_install(task->space_id, va + offset, pa + offset, prot)) {
            for (size_t n = 0; n < offset; n += NOS_PAGE_SIZE) arch_unmap(task->space_id, va + n);
            arch_tlb_sync();
            /* Restore every original page, including ones not attempted. */
            for (size_t n = 0; n < bytes; n += NOS_PAGE_SIZE) {
                struct vma *old = find_vma(s, va + n);
                if (old && map_install(task->space_id, va + n, old->pa + va + n - old->va, old->prot))
                    kernel_fail("fixed-map rollback failed");
            }
            arch_tlb_sync(); if (!provided && !(pa & 1)) page_free(pa, (unsigned)(bytes / NOS_PAGE_SIZE)); return -NOS_ENOMEM;
        }
    }
    arch_tlb_sync();
    for (unsigned i = 0; i < VMA_SLOTS; i++) {
        struct vma *old = &s->vma[i];
        if (!old->pa || !overlap(va, bytes, old->va, old->size)) continue;
        uintptr_t first = va > old->va ? va : old->va;
        uintptr_t last = end < old->va + old->size ? end : old->va + old->size;
        if (vma_backed(old) && !backing_shared(old->pa, old->size) && page_free_range(old->pa + first - old->va, (unsigned)((last - first) / NOS_PAGE_SIZE)))
            kernel_fail("fixed-map backing release failed");
    }
    replacement[used] = (struct vma){ .va = va, .pa = pa, .size = bytes, .prot = prot, .maxprot = 7 };
    k_memcpy(s->vma, replacement, sizeof(replacement)); invalidate_tls(task, va, bytes);
    shared_collect(); return (intptr_t)va;
}
static intptr_t fixed_map(struct task *task, uintptr_t va, size_t bytes, unsigned prot, int replace)
{ return fixed_backing(task, va, bytes, prot, replace, 0); }

#endif
#ifdef NOS_MMU
static intptr_t linux_mapping(struct task *task, uintptr_t address, size_t length, unsigned prot, unsigned flags, uintptr_t provided)
{
    const unsigned fixed = 0x10, noreplace = 0x100000;
    if (flags & (fixed | noreplace)) return fixed_backing(task, address, length, prot, !!(flags & fixed) && !(flags & noreplace), provided);
    struct memory_space *s = task_space(task);
    if (!s) return -NOS_EINVAL;
    address &= ~(uintptr_t)(NOS_PAGE_SIZE - 1);
    if (address && permitted_range(s, address, length, 0)) {
        unsigned occupied = 0;
        for (unsigned n = 0; n < VMA_SLOTS; n++) occupied |= s->vma[n].pa && overlap(address, length, s->vma[n].va, s->vma[n].size);
        if (!occupied) return map_backing(task, address, prot, length, 0, provided);
    }
    for (uintptr_t candidate = 0x07000000u; candidate < VM_USER_END && length <= VM_USER_END - candidate;) {
        if (!permitted_range(s, candidate, length, 0)) { candidate = (candidate + 0x200000u) & ~(uintptr_t)0x1fffffu; continue; }
        uintptr_t next = candidate;
        for (unsigned n = 0; n < VMA_SLOTS; n++) if (s->vma[n].pa && overlap(candidate, length, s->vma[n].va, s->vma[n].size))
            if (s->vma[n].va + s->vma[n].size > next) next = s->vma[n].va + s->vma[n].size;
        if (next == candidate) return map_backing(task, candidate, prot, length, 0, provided);
        candidate = next;
    }
    return -NOS_ENOMEM;
}
static struct shared_object *shared_new(size_t bytes, unsigned creator, int pinned)
{
    for (unsigned i = 0; i < SHARED_OBJECTS; i++) {
        struct shared_object *o = &shared_objects[i];
        if (o->pa || o->generation == SHARED_GENERATION_MAX) continue;
        uintptr_t pa = page_alloc((unsigned)(bytes / NOS_PAGE_SIZE));
        if (!pa) return NULL;
        o->pa = pa; o->size = bytes; o->generation++; o->creator = creator;
        o->pinned = pinned;
        return o;
    }
    return NULL;
}
static struct shared_object *shared_handle(unsigned handle)
{
    if (handle < NOS_SHARED_BASE || handle >= SHARED_LEASE_BASE) return NULL;
    unsigned raw = handle - NOS_SHARED_BASE, slot = raw & 255;
    if (!slot || slot > SHARED_OBJECTS) return NULL;
    struct shared_object *o = &shared_objects[slot - 1];
    return o->pa && o->generation == (raw >> 8) ? o : NULL;
}
static struct shared_lease *lease_handle(unsigned handle)
{
    if (handle < SHARED_LEASE_BASE || handle >= NOS_SHARED_LIMIT) return NULL;
    unsigned raw = handle - SHARED_LEASE_BASE, slot = raw & 255;
    if (!slot || slot > SHARED_LEASES) return NULL;
    struct shared_lease *l = &shared_leases[slot - 1];
    if (!l->object || l->generation != (raw >> 8)) return NULL;
    struct shared_object *o = &shared_objects[l->object - 1];
    return o->pa && o->generation == l->object_generation ? l : NULL;
}
static unsigned shared_token(struct shared_object *o)
{ return NOS_SHARED_BASE + (o->generation << 8) + ((unsigned)(o - shared_objects) + 1); }
static unsigned lease_token(struct shared_lease *l)
{ return SHARED_LEASE_BASE + (l->generation << 8) + ((unsigned)(l - shared_leases) + 1); }
static void shared_mapping_ceiling(struct task *task, uintptr_t va, unsigned prot)
{
    struct memory_space *s = task_space(task);
    for (unsigned i = 0; i < VMA_SLOTS; i++) if (s->vma[i].pa && s->vma[i].va == va) {
        s->vma[i].maxprot = prot; break;
    }
}

#endif
intptr_t vm_shared_create(struct task *broker, size_t bytes)
{
#ifndef NOS_MMU
    (void)broker; (void)bytes; return -NOS_ENOTSUP;
#else
    if (!(broker->caps & NOS_CAP_SHARED_BROKER) || !task_space(broker)) return -NOS_EPERM;
    if (rounded_size(bytes, &bytes)) return -NOS_EINVAL;
    struct shared_object *o = shared_new(bytes, task_space(broker)->owner, 1);
    return o ? (intptr_t)shared_token(o) : -NOS_ENOMEM;
#endif
}
int vm_shared_grant(struct task *broker, unsigned handle, struct task *grantee, unsigned prot)
{
#ifndef NOS_MMU
    (void)broker; (void)handle; (void)grantee; (void)prot; return -NOS_ENOTSUP;
#else
    struct shared_object *o = shared_handle(handle);
    struct memory_space *from = task_space(broker), *to = task_space(grantee);
    if (!o || !from || !to || (prot & ~3u)) return -NOS_EINVAL;
    if (!(broker->caps & NOS_CAP_SHARED_BROKER) || o->creator != from->owner || !o->pinned) return -NOS_EPERM;
    if (prot & NOS_PROT_WRITE) prot |= NOS_PROT_READ;
    lease_collect(); unsigned pending = 0;
    for (unsigned i = 0; i < SHARED_LEASES; i++) pending += shared_leases[i].object && lease_has_owner(&shared_leases[i], to->owner);
    if (pending >= 8) return -NOS_ENOMEM;
    for (unsigned i = 0; i < SHARED_LEASES; i++) {
        struct shared_lease *l = &shared_leases[i];
        if (l->object || l->generation == SHARED_GENERATION_MAX) continue;
        l->generation++; l->object = (unsigned)(o - shared_objects) + 1;
        l->object_generation = o->generation; l->prot = prot;
        l->count = 1; l->owners[0] = to->owner;
        return (int)lease_token(l);
    }
    return -NOS_ENOMEM;
#endif
}
intptr_t vm_shared_map(struct task *task, unsigned handle, uintptr_t address, size_t bytes, size_t offset, unsigned prot, unsigned flags)
{
#ifndef NOS_MMU
    (void)task; (void)handle; (void)address; (void)bytes; (void)offset; (void)prot; (void)flags; return -NOS_ENOTSUP;
#else
    struct memory_space *s = task_space(task);
    if (!s) return -NOS_EINVAL;
    struct shared_lease *lease = lease_handle(handle);
    struct shared_object *o = lease ? &shared_objects[lease->object - 1] : shared_handle(handle);
    if (!o) return -NOS_EINVAL;
    if (lease ? !lease_has_owner(lease, s->owner) : o->creator != s->owner) return -NOS_EACCES;
    unsigned ceiling = lease ? lease->prot : 3;
    if (prot & 6) prot |= NOS_PROT_READ;
    intptr_t result;
    if (prot & ~ceiling) result = -NOS_EACCES;
    else if (!valid_prot(prot) || rounded_size(bytes, &bytes) || (offset & (NOS_PAGE_SIZE - 1)) ||
        offset > o->size || bytes > o->size - offset || (flags & ~(1u | 0x10u | 0x100000u | 0x4000u))) result = -NOS_EINVAL;
    else {
        result = linux_mapping(task, address, bytes, prot, flags, o->pa + offset);
        if ((uintptr_t)result < (uintptr_t)-4095) shared_mapping_ceiling(task, (uintptr_t)result, ceiling);
    }
    /* The authorization reference becomes a VMA reference on success.
     * A failed mapping consumes only this group's one-shot authorization. */
    if (lease) { lease_remove_owner(lease, s->owner); shared_collect(); }
    return result;
#endif
}
int vm_shared_release(struct task *broker, unsigned handle)
{
#ifndef NOS_MMU
    (void)broker; (void)handle; return -NOS_ENOTSUP;
#else
    struct memory_space *s = task_space(broker);
    if (!s) return -NOS_EINVAL;
    struct shared_lease *l = lease_handle(handle);
    if (l) {
        struct shared_object *o = &shared_objects[l->object - 1];
        if ((broker->caps & NOS_CAP_SHARED_BROKER) && o->creator == s->owner) l->object = l->count = 0;
        else if (lease_has_owner(l, s->owner)) lease_remove_owner(l, s->owner);
        else return -NOS_EPERM;
        shared_collect(); return 0;
    }
    struct shared_object *o = shared_handle(handle);
    if (!o) return -NOS_EINVAL;
    if (!(broker->caps & NOS_CAP_SHARED_BROKER) || o->creator != s->owner || !o->pinned) return -NOS_EPERM;
    o->pinned = 0; shared_collect(); return 0;
#endif
}
intptr_t vm_map_linux(struct task *task, uintptr_t address, size_t length, unsigned prot, unsigned flags)
{
#ifndef NOS_MMU
    (void)task; (void)address; (void)length; (void)prot; (void)flags; return -NOS_ENOTSUP;
#else
    if (prot & 6) prot |= NOS_PROT_READ;
    unsigned kind = flags & 3;
    if ((flags & ~(3u | 0x20u | 0x800u | 0x1000u | 0x4000u | 0x20000u | 0x10u | 0x100000u)) ||
        (kind != 1 && kind != 2) || !valid_prot(prot) || mapping_size(length, prot, &length)) return -NOS_EINVAL;
    if (kind == 2) return linux_mapping(task, address, length, prot, flags, 0);
    if (!(flags & 0x20) || length > VM_MAP_MAX) return -NOS_ENOTSUP;
    if (prot & NOS_PROT_EXEC) return -NOS_EACCES;
    struct memory_space *s = task_space(task);
    if (!s) return -NOS_EINVAL;
    struct shared_object *o = shared_new(length, s->owner, 0);
    if (!o) return -NOS_ENOMEM;
    intptr_t mapped = linux_mapping(task, address, length, prot, flags, o->pa);
    if ((uintptr_t)mapped >= (uintptr_t)-4095) shared_collect();
    else shared_mapping_ceiling(task, (uintptr_t)mapped, 3);
    return mapped;
#endif
}
intptr_t vm_linux_protect(struct task *task, uintptr_t va, unsigned prot, size_t bytes)
{
    if (prot & 6) prot |= NOS_PROT_READ;
    return vm_protect(task, va, prot, bytes);
}
intptr_t vm_linux_sync(struct task *task, uintptr_t va, size_t bytes, unsigned flags)
{
#ifndef NOS_MMU
    (void)task; (void)va; (void)bytes; (void)flags; return -NOS_ENOTSUP;
#else
    struct memory_space *s = task_space(task);
    if (!s || (va & (NOS_PAGE_SIZE - 1)) || (flags & ~7u) || ((flags & 5) == 5) ||
        bytes > SIZE_MAX - (NOS_PAGE_SIZE - 1)) return -NOS_EINVAL;
    bytes = (bytes + NOS_PAGE_SIZE - 1) & ~(size_t)(NOS_PAGE_SIZE - 1);
    if (va > UINTPTR_MAX - bytes) return -NOS_EINVAL;
    uintptr_t end = va + bytes;
    while (va < end) {
        struct vma *v = find_vma(s, va);
        if (!v) {
            size_t count = end - va < NOS_PAGE_SIZE ? end - va : NOS_PAGE_SIZE;
            if (!user_access(task, va, count, 0)) return -NOS_ENOMEM;
            va += count;
        } else {
            uintptr_t next = v->va + v->size;
            va = next < end ? next : end;
        }
    }
    /* RAM-backed files and mappings use identical physical pages. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST); return 0;
#endif
}
intptr_t vm_linux_unmap(struct task *task, uintptr_t va, size_t bytes)
{
#ifndef NOS_MMU
    (void)task; (void)va; (void)bytes; return -NOS_ENOTSUP;
#else
    struct memory_space *s = task_space(task);
    if (!s || !bytes || bytes > SIZE_MAX - (NOS_PAGE_SIZE - 1)) return -NOS_EINVAL;
    bytes = (bytes + NOS_PAGE_SIZE - 1) & ~(size_t)(NOS_PAGE_SIZE - 1);
    if (!permitted_range(s, va, bytes, 0)) return -NOS_EINVAL;
    /* Reuse the same whole-range metadata transaction as native unmap,
     * while accepting gaps and lengths larger than a single mapping. */
    return vm_change_mode(task, va, bytes, 0, 1, 1);
#endif
}
intptr_t vm_linux_brk(struct task *task, uintptr_t address)
{
#ifndef NOS_MMU
    (void)task; (void)address; return -NOS_ENOTSUP;
#else
    struct memory_space *s = task_space(task);
    if (!s) return -NOS_EINVAL;
    if (!s->linux_brk) s->linux_brk = 0x02800000u;
    if (!address) return (intptr_t)s->linux_brk;
    if (address < 0x02800000u || address > 0x03000000u) return (intptr_t)s->linux_brk;
    uintptr_t old_page = (s->linux_brk + NOS_PAGE_SIZE - 1) & ~(uintptr_t)(NOS_PAGE_SIZE - 1);
    uintptr_t new_page = (address + NOS_PAGE_SIZE - 1) & ~(uintptr_t)(NOS_PAGE_SIZE - 1);
    if (new_page > old_page) {
        intptr_t rc = fixed_map(task, old_page, new_page - old_page, 3, 0);
        if ((uintptr_t)rc >= (uintptr_t)-4095) return (intptr_t)s->linux_brk;
        /* Keep sequential sbrk growth from consuming one VMA per call.
         * Physical adjacency is required; the allocator can free a merged
         * range that spans its constituent contiguous allocations. */
        for (unsigned i = 0; i < VMA_SLOTS; i++) {
            struct vma *a = &s->vma[i];
            if (!a->pa || a->va < 0x02800000u || a->va + a->size > new_page || a->prot != 3) continue;
            for (unsigned j = 0; j < VMA_SLOTS; j++) {
                struct vma *b = &s->vma[j];
                if (b->pa && a->va + a->size == b->va && a->pa + a->size == b->pa && b->prot == 3 &&
                    b->va + b->size <= new_page) {
                    a->size += b->size; k_memset(b, 0, sizeof(*b)); j = UINT32_MAX;
                }
            }
        }
    } else if (new_page < old_page && vm_linux_unmap(task, new_page, old_page - new_page)) return (intptr_t)s->linux_brk;
    s->linux_brk = address; return (intptr_t)address;
#endif
}
