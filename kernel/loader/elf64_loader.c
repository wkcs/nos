/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS ARM64 ELF64 严格 W^X 加载器与 VFS Cell 沙箱运行器
 */

#define pr_fmt(fmt) "[ELF64]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/elf64.h>
#include <nos/accel.h>
#include <nos/cap.h>
#include <nos/cell.h>
#include <nos/completion.h>
#include <nos/hal_arch.h>
#include <nos/vmo.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/task.h>
#include <string.h>

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
#include <fs/fs.h>
#include <asm/cpufeature.h>
#include <asm/mmu.h>
#include <asm/security.h>
#include <asm/switch.h>
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096UL
#endif

#define NOS_ELF64_MAX_FILE_BYTES (4U * 1024U * 1024U)

static bool elf64_ranges_overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1)
{
    return (a0 < b1) && (b0 < a1);
}

int nos_elf64_validate(const void *buf, size_t size, uintptr_t *out_entry)
{
    const uint8_t *bytes = (const uint8_t *)buf;
    const Elf64_Ehdr *eh;
    const Elf64_Phdr *phdrs;
    uint64_t ph_bytes;
    uint32_t load_cnt = 0U;
    bool entry_valid = false;
    uint64_t seg_vstarts[NOS_ELF64_MAX_SEGS];
    uint64_t seg_vends[NOS_ELF64_MAX_SEGS];

    if (buf == NULL) {
        return -EINVAL;
    }
    if (size < sizeof(Elf64_Ehdr)) {
        return -ENOEXEC;
    }

    eh = (const Elf64_Ehdr *)buf;
    if (eh->e_ident[0] != ELF_MAGIC0 ||
        eh->e_ident[1] != ELF_MAGIC1 ||
        eh->e_ident[2] != ELF_MAGIC2 ||
        eh->e_ident[3] != ELF_MAGIC3 ||
        eh->e_ident[4] != ELFCLASS64 ||
        eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_ident[6] != 1U) {
        return -ENOEXEC;
    }

    if (eh->e_type != ET_EXEC || eh->e_machine != EM_AARCH64) {
        return -ENOEXEC;
    }

    if (eh->e_phentsize != sizeof(Elf64_Phdr) ||
        eh->e_phnum == 0U || eh->e_phnum > 32U) {
        return -ENOEXEC;
    }

    ph_bytes = (uint64_t)eh->e_phnum * (uint64_t)sizeof(Elf64_Phdr);
    if (eh->e_phoff > (uint64_t)size || ph_bytes > ((uint64_t)size - eh->e_phoff)) {
        return -ENOEXEC;
    }

    phdrs = (const Elf64_Phdr *)(bytes + eh->e_phoff);
    for (uint16_t i = 0U; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        uint64_t vstart;
        uint64_t vend;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0ULL) {
            continue;
        }
        if (load_cnt >= NOS_ELF64_MAX_SEGS) {
            return -E2BIG;
        }

        /* 硬件 W^X 铁律：严禁 ELF 加载具备可写且可执行权限的 PT_LOAD 段 */
        if ((ph->p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) {
            return -EPERM;
        }
        if (ph->p_filesz > ph->p_memsz ||
            ph->p_offset > (uint64_t)size ||
            ph->p_filesz > ((uint64_t)size - ph->p_offset)) {
            return -ENOEXEC;
        }

        vstart = ph->p_vaddr & ~(uint64_t)(PAGE_SIZE - 1U);
        if (ph->p_vaddr > UINT64_MAX - ph->p_memsz ||
            ph->p_vaddr + ph->p_memsz > UINT64_MAX - (uint64_t)(PAGE_SIZE - 1U)) {
            return -EFAULT;
        }
        vend = (ph->p_vaddr + ph->p_memsz + (uint64_t)(PAGE_SIZE - 1U)) &
               ~(uint64_t)(PAGE_SIZE - 1U);

        if (vstart < (uint64_t)NOS_ELF64_USER_MIN_VA ||
            vend > (uint64_t)NOS_ELF64_USER_MAX_VA) {
            return -EFAULT;
        }

        /* 禁止段跨越用户栈、只读 Telemetry 与 BootInfo 保留页区 */
        if (elf64_ranges_overlap(vstart, vend,
                                 (uint64_t)NOS_ELF64_STACK_VA,
                                 (uint64_t)NOS_ELF64_AUX_VA + (uint64_t)PAGE_SIZE)) {
            return -EPERM;
        }

        /* 页粒度 W^X 隔离要求不同 PT_LOAD 段不可重叠共享同一物理 4KB 页 */
        for (uint32_t k = 0U; k < load_cnt; k++) {
            if (elf64_ranges_overlap(vstart, vend, seg_vstarts[k], seg_vends[k])) {
                return -EPERM;
            }
        }
        seg_vstarts[load_cnt] = vstart;
        seg_vends[load_cnt] = vend;

        if ((ph->p_flags & PF_X) != 0U &&
            eh->e_entry >= ph->p_vaddr &&
            eh->e_entry < ph->p_vaddr + ph->p_memsz) {
            entry_valid = true;
        }
        load_cnt++;
    }

    if (load_cnt == 0U || !entry_valid) {
        return -ENOEXEC;
    }

    if (out_entry != NULL) {
        *out_entry = (uintptr_t)eh->e_entry;
    }
    return 0;
}

int nos_elf64_load_into_cell(const void *buf,
                             size_t size,
                             struct nos_cell *cell,
                             struct nos_elf64_image *out_img)
{
    const uint8_t *bytes = (const uint8_t *)buf;
    const Elf64_Ehdr *eh;
    const Elf64_Phdr *phdrs;
    uintptr_t entry = 0U;
    int rc;

    if (buf == NULL || cell == NULL || out_img == NULL) {
        return -EINVAL;
    }

    memset(out_img, 0, sizeof(*out_img));
    rc = nos_elf64_validate(buf, size, &entry);
    if (rc != 0) {
        return rc;
    }

    eh = (const Elf64_Ehdr *)buf;
    phdrs = (const Elf64_Phdr *)(bytes + eh->e_phoff);
    out_img->entry_va = entry;

    for (uint16_t i = 0U; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        struct nos_vmo_create_args vargs;
        struct nos_vmo *vmo;
        uint64_t vstart;
        uint64_t vend;
        size_t seg_size;
        size_t page_off;
        uint32_t prot = HAL_VM_PROT_USER;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0ULL) {
            continue;
        }

        vstart = ph->p_vaddr & ~(uint64_t)(PAGE_SIZE - 1U);
        vend = (ph->p_vaddr + ph->p_memsz + (uint64_t)(PAGE_SIZE - 1U)) &
               ~(uint64_t)(PAGE_SIZE - 1U);
        seg_size = (size_t)(vend - vstart);
        page_off = (size_t)(ph->p_vaddr - vstart);

        memset(&vargs, 0, sizeof(vargs));
        vargs.size_bytes = seg_size;
        vargs.kind = NOS_VMO_ANON;
        vmo = nos_vmo_create(&vargs);
        if (vmo == NULL) {
            nos_elf64_unload_image(out_img);
            return -ENOMEM;
        }

        if (ph->p_filesz > 0ULL) {
            rc = nos_vmo_write(vmo, page_off, bytes + ph->p_offset, (size_t)ph->p_filesz);
            if (rc != 0) {
                (void)nos_object_close(&vmo->header);
                (void)nos_object_retire(&vmo->header);
                nos_elf64_unload_image(out_img);
                return rc;
            }
        }

        if ((ph->p_flags & PF_R) != 0U) {
            prot |= HAL_VM_PROT_READ;
        }
        if ((ph->p_flags & PF_W) != 0U) {
            prot |= HAL_VM_PROT_WRITE;
        }
        if ((ph->p_flags & PF_X) != 0U) {
            prot |= HAL_VM_PROT_EXEC | HAL_VM_PROT_READ;
        }

        /* 代码段与只读数据段永久 Seal 并刷 I-Cache */
        if ((ph->p_flags & PF_W) == 0U) {
#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
            if ((ph->p_flags & PF_X) != 0U) {
                for (uint32_t p = 0U; p < vmo->page_count; p++) {
                    if (vmo->pages[p] != NULL && vmo->pages[p]->kvaddr != 0U) {
                        arm64_sync_icache_dcache(vmo->pages[p]->kvaddr, PAGE_SIZE);
                    }
                }
            }
#endif
            rc = nos_vmo_seal(vmo);
            if (rc != 0) {
                (void)nos_object_close(&vmo->header);
                (void)nos_object_retire(&vmo->header);
                nos_elf64_unload_image(out_img);
                return rc;
            }
        }

        rc = nos_vmo_map(vmo, nos_cell_vm_space(cell),
                         (uintptr_t)vstart, 0U, seg_size, prot, cell);
        if (rc != 0) {
            (void)nos_object_close(&vmo->header);
            (void)nos_object_retire(&vmo->header);
            nos_elf64_unload_image(out_img);
            return rc;
        }

        out_img->seg_vmos[out_img->seg_count++] = vmo;
    }

    return 0;
}

void nos_elf64_unload_image(struct nos_elf64_image *img)
{
    if (img == NULL) {
        return;
    }
    for (uint32_t i = 0U; i < img->seg_count; i++) {
        if (img->seg_vmos[i] != NULL) {
            (void)nos_object_close(&img->seg_vmos[i]->header);
            (void)nos_object_retire(&img->seg_vmos[i]->header);
            img->seg_vmos[i] = NULL;
        }
    }
    img->seg_count = 0U;
    img->entry_va = 0U;
}

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)

struct elf64_spawn_ctx {
    struct nos_completion done;
    uintptr_t             entry_va;
    uintptr_t             stack_top;
    uintptr_t             aux_va;
    int                   exit_code;
};

static void elf64_task_entry(void *param)
{
    struct elf64_spawn_ctx *ctx = (struct elf64_spawn_ctx *)param;

    arm64_enter_user_mode(ctx->entry_va, ctx->stack_top, ctx->aux_va);
}

static void elf64_task_cleanup(struct task_struct *task)
{
    struct elf64_spawn_ctx *ctx;

    if (task == NULL || task->parameter == NULL) {
        return;
    }
    ctx = (struct elf64_spawn_ctx *)task->parameter;
    ctx->exit_code = (int)task->exit_code;
    (void)nos_complete(&ctx->done);
}

int nos_elf64_spawn_from_vfs(const char *path,
                             const struct nos_cell_budget *budget,
                             int *out_exit_code,
                             struct nos_cell_telemetry *out_telemetry)
{
    struct nos_cell_budget def_budget = {
        .cpu_ns_limit     = 5000000000ULL,
        .mem_bytes_limit  = 512ULL * 1024ULL,
        .tool_calls_limit = 16ULL,
        .accel_ops_limit  = 8ULL,
        .max_child_cells  = 0U,
        .flags            = 0U,
    };
    const struct nos_cell_budget *use_budget = (budget != NULL) ? budget : &def_budget;
    struct nos_vmo_create_args stack_args = {
        .size_bytes = NOS_ELF64_STACK_SIZE,
        .kind       = NOS_VMO_ANON,
        .flags      = 0U,
    };
    struct nos_vmo_create_args aux_args = {
        .size_bytes = PAGE_SIZE,
        .kind       = NOS_VMO_ANON,
        .flags      = 0U,
    };
    struct elf64_spawn_ctx ctx;
    struct nos_elf64_image img;
    struct nos_user_boot_info boot_info;
    struct arm64_sec_status sec_st;
    const struct arm64_cpu_caps *cpu_caps;
    struct nos_cell *cell = NULL;
    struct nos_vmo *stack_vmo = NULL;
    struct nos_vmo *aux_vmo = NULL;
    struct nos_accel_device *accel_dev = NULL;
    struct task_struct *task = NULL;
    nos_handle_t accel_h = NOS_HANDLE_INVALID;
    void *file_buf = NULL;
    loff_t file_len;
    size_t got = 0U;
    int fd;
    int rc;
    bool task_started = false;

    if (path == NULL) {
        return -EINVAL;
    }

    memset(&img, 0, sizeof(img));
    memset(&ctx, 0, sizeof(ctx));

    fd = nos_open(path, O_RDONLY);
    if (fd < 0) {
        return fd;
    }

    file_len = nos_lseek(fd, 0, SEEK_END);
    if (file_len < (loff_t)sizeof(Elf64_Ehdr) ||
        file_len > (loff_t)NOS_ELF64_MAX_FILE_BYTES) {
        (void)nos_close(fd);
        return -ENOEXEC;
    }
    if (nos_lseek(fd, 0, SEEK_SET) != 0) {
        (void)nos_close(fd);
        return -EIO;
    }

    file_buf = kmalloc((size_t)file_len, GFP_KERNEL);
    if (file_buf == NULL) {
        (void)nos_close(fd);
        return -ENOMEM;
    }

    while (got < (size_t)file_len) {
        ssize_t n = nos_read(fd, (uint8_t *)file_buf + got, (size_t)file_len - got);
        if (n <= 0) {
            kfree(file_buf);
            (void)nos_close(fd);
            return -EIO;
        }
        got += (size_t)n;
    }
    (void)nos_close(fd);

    cell = nos_cell_create(NULL, use_budget);
    if (cell == NULL) {
        kfree(file_buf);
        return -ENOMEM;
    }

    rc = nos_elf64_load_into_cell(file_buf, (size_t)file_len, cell, &img);
    kfree(file_buf);
    file_buf = NULL;
    if (rc != 0) {
        goto cleanup;
    }

    stack_vmo = nos_vmo_create(&stack_args);
    aux_vmo = nos_vmo_create(&aux_args);
    if (stack_vmo == NULL || aux_vmo == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }

    rc = nos_vmo_map(stack_vmo, nos_cell_vm_space(cell),
                     NOS_ELF64_STACK_VA, 0U, NOS_ELF64_STACK_SIZE,
                     HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE, cell);
    if (rc == 0) {
        rc = nos_cell_map_telemetry(cell, NOS_ELF64_TELEM_VA);
    }
    if (rc != 0) {
        goto cleanup;
    }

    accel_dev = nos_accel_get_default();
    if (accel_dev != NULL) {
        (void)nos_cap_alloc(nos_cell_cap_table(cell), NOS_CAP_ACCEL, accel_dev,
                            NOS_CAP_RIGHT_READ | NOS_CAP_RIGHT_SEND, 0U, &accel_h);
    }

    cpu_caps = arm64_get_cpu_caps();
    arm64_security_get_status(&sec_st);
    memset(&boot_info, 0, sizeof(boot_info));
    boot_info.magic = NOS_USER_BOOT_MAGIC;
    boot_info.version = 1U;
    boot_info.accel_cap = accel_h;
    if (cpu_caps != NULL && cpu_caps->has_sve) {
        boot_info.flags |= NOS_USER_FLAG_SVE;
    }
    if (sec_st.pac_enabled) {
        boot_info.flags |= NOS_USER_FLAG_PAC;
    }
    if (sec_st.bti_enabled) {
        boot_info.flags |= NOS_USER_FLAG_BTI;
    }
    if (sec_st.mte_enabled) {
        boot_info.flags |= NOS_USER_FLAG_MTE;
    }
    boot_info.telemetry_va = (uint64_t)NOS_ELF64_TELEM_VA;

    rc = nos_vmo_write(aux_vmo, 0U, &boot_info, sizeof(boot_info));
    if (rc == 0) {
        rc = nos_vmo_seal(aux_vmo);
    }
    if (rc == 0) {
        rc = nos_vmo_map(aux_vmo, nos_cell_vm_space(cell),
                         NOS_ELF64_AUX_VA, 0U, PAGE_SIZE,
                         HAL_VM_PROT_USER | HAL_VM_PROT_READ, NULL);
    }
    if (rc != 0) {
        goto cleanup;
    }

    (void)nos_completion_init(&ctx.done);
    ctx.entry_va = img.entry_va;
    ctx.stack_top = NOS_ELF64_STACK_VA + NOS_ELF64_STACK_SIZE;
    ctx.aux_va = NOS_ELF64_AUX_VA;
    ctx.exit_code = -1;

    task = task_create_ex("elf64_agent", elf64_task_entry, &ctx,
                          SCHED_POLICY_NORMAL, 8U, 0, NOS_CPU_MASK_ALL,
                          CONFIG_CORE_TASK_STACK_SIZE, 10U, elf64_task_cleanup);
    if (task == NULL) {
        (void)nos_object_close(&ctx.done.object);
        (void)nos_object_retire(&ctx.done.object);
        rc = -ENOMEM;
        goto cleanup;
    }

    rc = nos_cell_attach_task(task, cell);
    if (rc != 0) {
        task_del(task);
        (void)nos_completion_wait(&ctx.done, 3000U);
        (void)nos_object_close(&ctx.done.object);
        (void)nos_object_retire(&ctx.done.object);
        goto cleanup;
    }

    (void)task_ready(task);
    task_started = true;

    rc = nos_completion_wait(&ctx.done, 10000U);
    if (rc != 0) {
        task_del(task);
        (void)nos_completion_wait(&ctx.done, 3000U);
    }
    (void)nos_object_close(&ctx.done.object);
    (void)nos_object_retire(&ctx.done.object);

    if (out_exit_code != NULL) {
        *out_exit_code = ctx.exit_code;
    }
    if (out_telemetry != NULL) {
        const struct nos_cell_telemetry *tm = nos_cell_telemetry_view(cell);
        if (tm != NULL) {
            *out_telemetry = *tm;
        } else {
            memset(out_telemetry, 0, sizeof(*out_telemetry));
        }
    }

cleanup:
    (void)task_started;
    if (accel_dev != NULL) {
        (void)nos_object_put(&accel_dev->header);
    }
    if (aux_vmo != NULL) {
        (void)nos_object_close(&aux_vmo->header);
        (void)nos_object_retire(&aux_vmo->header);
    }
    if (stack_vmo != NULL) {
        (void)nos_object_close(&stack_vmo->header);
        (void)nos_object_retire(&stack_vmo->header);
    }
    nos_elf64_unload_image(&img);
    if (cell != NULL) {
        (void)nos_cell_destroy(cell);
    }
    return rc;
}

#else

int nos_elf64_spawn_from_vfs(const char *path,
                             const struct nos_cell_budget *budget,
                             int *out_exit_code,
                             struct nos_cell_telemetry *out_telemetry)
{
    (void)path;
    (void)budget;
    (void)out_exit_code;
    (void)out_telemetry;
    return -ENOSYS;
}

#endif
