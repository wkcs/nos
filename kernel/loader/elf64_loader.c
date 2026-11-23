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
#include <nos/ring.h>
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
    uint64_t load_bias;
    uint64_t entry_va;
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

    if ((eh->e_type != ET_EXEC && eh->e_type != ET_DYN) || eh->e_machine != EM_AARCH64) {
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

    load_bias = (eh->e_type == ET_DYN) ? (uint64_t)NOS_ELF64_PIE_BASE : 0ULL;
    if (eh->e_entry > UINT64_MAX - load_bias) {
        return -EFAULT;
    }
    entry_va = eh->e_entry + load_bias;

    phdrs = (const Elf64_Phdr *)(bytes + eh->e_phoff);
    for (uint16_t i = 0U; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        uint64_t seg_va;
        uint64_t vstart;
        uint64_t vend;

        if (ph->p_type == PT_DYNAMIC) {
            if (ph->p_offset > (uint64_t)size || ph->p_filesz > ((uint64_t)size - ph->p_offset)) {
                return -ENOEXEC;
            }
            continue;
        }

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

        if (ph->p_vaddr > UINT64_MAX - load_bias) {
            return -EFAULT;
        }
        seg_va = ph->p_vaddr + load_bias;
        vstart = seg_va & ~(uint64_t)(PAGE_SIZE - 1U);
        if (seg_va > UINT64_MAX - ph->p_memsz ||
            seg_va + ph->p_memsz > UINT64_MAX - (uint64_t)(PAGE_SIZE - 1U)) {
            return -EFAULT;
        }
        vend = (seg_va + ph->p_memsz + (uint64_t)(PAGE_SIZE - 1U)) &
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
            entry_va >= seg_va &&
            entry_va < seg_va + ph->p_memsz) {
            entry_valid = true;
        }
        load_cnt++;
    }

    if (load_cnt == 0U || !entry_valid) {
        return -ENOEXEC;
    }

    if (out_entry != NULL) {
        *out_entry = (uintptr_t)entry_va;
    }
    return 0;
}

struct elf64_loaded_seg {
    uint64_t        vstart;
    uint64_t        vend;
    uint64_t        link_vaddr;
    uint64_t        p_offset;
    uint64_t        p_filesz;
    uint32_t        p_flags;
    struct nos_vmo *vmo;
};

static int elf64_apply_pie_relocs(const uint8_t *bytes,
                                  size_t size,
                                  const Elf64_Phdr *dyn_ph,
                                  const struct elf64_loaded_seg *segs,
                                  uint32_t seg_count,
                                  uint64_t load_bias)
{
    uint64_t dt_rela = 0ULL;
    uint64_t dt_relasz = 0ULL;
    uint64_t dt_relaent = 0ULL;
    bool has_rela = false;
    uint64_t rela_off = 0ULL;
    bool found_off = false;
    size_t dyn_cnt;
    size_t rela_cnt;

    if (dyn_ph == NULL || dyn_ph->p_filesz < sizeof(Elf64_Dyn)) {
        return 0;
    }

    dyn_cnt = (size_t)(dyn_ph->p_filesz / sizeof(Elf64_Dyn));
    for (size_t i = 0U; i < dyn_cnt; i++) {
        Elf64_Dyn dyn;
        memcpy(&dyn, bytes + dyn_ph->p_offset + (i * sizeof(Elf64_Dyn)), sizeof(dyn));
        if (dyn.d_tag == DT_NULL) {
            break;
        }
        if (dyn.d_tag == DT_RELA) {
            dt_rela = dyn.d_val;
            has_rela = true;
        } else if (dyn.d_tag == DT_RELASZ) {
            dt_relasz = dyn.d_val;
        } else if (dyn.d_tag == DT_RELAENT) {
            dt_relaent = dyn.d_val;
        }
    }

    if (!has_rela || dt_relasz == 0ULL) {
        return 0;
    }
    if (dt_relaent != sizeof(Elf64_Rela) || (dt_relasz % sizeof(Elf64_Rela)) != 0ULL) {
        return -ENOEXEC;
    }
    if (dt_rela > UINT64_MAX - dt_relasz) {
        return -ENOEXEC;
    }

    for (uint32_t s = 0U; s < seg_count; s++) {
        uint64_t sv0 = segs[s].link_vaddr;
        uint64_t sv1 = sv0 + segs[s].p_filesz;
        if (dt_rela >= sv0 && (dt_rela + dt_relasz) <= sv1) {
            rela_off = segs[s].p_offset + (dt_rela - sv0);
            found_off = true;
            break;
        }
    }
    if (!found_off || rela_off > (uint64_t)size || dt_relasz > ((uint64_t)size - rela_off)) {
        return -ENOEXEC;
    }

    rela_cnt = (size_t)(dt_relasz / sizeof(Elf64_Rela));
    for (size_t i = 0U; i < rela_cnt; i++) {
        Elf64_Rela rela;
        uint32_t rtype;
        uint64_t target_va;
        uint64_t reloc_val;
        int target_seg = -1;

        memcpy(&rela, bytes + rela_off + (i * sizeof(Elf64_Rela)), sizeof(rela));
        rtype = ELF64_R_TYPE(rela.r_info);
        if (rtype == R_AARCH64_NONE) {
            continue;
        }
        if (rtype != R_AARCH64_RELATIVE) {
            return -ENOEXEC;
        }

        if (rela.r_offset > UINT64_MAX - load_bias ||
            rela.r_offset + load_bias > UINT64_MAX - sizeof(uint64_t)) {
            return -EFAULT;
        }
        target_va = load_bias + rela.r_offset;
        reloc_val = (uint64_t)((int64_t)load_bias + rela.r_addend);

        for (uint32_t s = 0U; s < seg_count; s++) {
            if (target_va >= segs[s].vstart && (target_va + sizeof(uint64_t)) <= segs[s].vend) {
                target_seg = (int)s;
                break;
            }
        }
        if (target_seg < 0) {
            return -EFAULT;
        }
        /* W^X 严守：绝对禁止重定位修改含 PF_X 权限的代码页 */
        if ((segs[target_seg].p_flags & PF_X) != 0U) {
            return -EPERM;
        }

        int wrc = nos_vmo_write(segs[target_seg].vmo,
                                (size_t)(target_va - segs[target_seg].vstart),
                                &reloc_val,
                                sizeof(reloc_val));
        if (wrc != 0) {
            return wrc;
        }
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
    const Elf64_Phdr *dyn_ph = NULL;
    struct elf64_loaded_seg segs[NOS_ELF64_MAX_SEGS];
    uintptr_t entry = 0U;
    uint64_t load_bias;
    int rc;

    if (buf == NULL || cell == NULL || out_img == NULL) {
        return -EINVAL;
    }

    memset(out_img, 0, sizeof(*out_img));
    memset(segs, 0, sizeof(segs));
    rc = nos_elf64_validate(buf, size, &entry);
    if (rc != 0) {
        return rc;
    }

    eh = (const Elf64_Ehdr *)buf;
    phdrs = (const Elf64_Phdr *)(bytes + eh->e_phoff);
    load_bias = (eh->e_type == ET_DYN) ? (uint64_t)NOS_ELF64_PIE_BASE : 0ULL;
    out_img->entry_va = entry;
    out_img->load_bias = (uintptr_t)load_bias;
    out_img->is_pie = (eh->e_type == ET_DYN);

    /* 阶段一：分配并填充全部 PT_LOAD 段 VMO（暂不 Seal，预留 PIE 重定位写入窗口） */
    for (uint16_t i = 0U; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        struct nos_vmo_create_args vargs;
        struct nos_vmo *vmo;
        uint64_t seg_va;
        uint64_t vstart;
        uint64_t vend;
        size_t seg_size;
        size_t page_off;

        if (ph->p_type == PT_DYNAMIC) {
            dyn_ph = ph;
            continue;
        }
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0ULL) {
            continue;
        }

        seg_va = ph->p_vaddr + load_bias;
        vstart = seg_va & ~(uint64_t)(PAGE_SIZE - 1U);
        vend = (seg_va + ph->p_memsz + (uint64_t)(PAGE_SIZE - 1U)) &
               ~(uint64_t)(PAGE_SIZE - 1U);
        seg_size = (size_t)(vend - vstart);
        page_off = (size_t)(seg_va - vstart);

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

        segs[out_img->seg_count].vstart = vstart;
        segs[out_img->seg_count].vend = vend;
        segs[out_img->seg_count].link_vaddr = ph->p_vaddr;
        segs[out_img->seg_count].p_offset = ph->p_offset;
        segs[out_img->seg_count].p_filesz = ph->p_filesz;
        segs[out_img->seg_count].p_flags = ph->p_flags;
        segs[out_img->seg_count].vmo = vmo;
        out_img->seg_vmos[out_img->seg_count++] = vmo;
    }

    /* 阶段二：ET_DYN PIE 执行 R_AARCH64_RELATIVE 静态重定位 */
    if (eh->e_type == ET_DYN && dyn_ph != NULL) {
        rc = elf64_apply_pie_relocs(bytes, size, dyn_ph, segs, out_img->seg_count, load_bias);
        if (rc != 0) {
            nos_elf64_unload_image(out_img);
            return rc;
        }
    }

    /* 阶段三：统一刷 I-Cache、对只读段永久 Seal 并映射进 Cell 页表（含 BTI） */
    for (uint32_t s = 0U; s < out_img->seg_count; s++) {
        uint32_t prot = HAL_VM_PROT_USER;
        size_t seg_size = (size_t)(segs[s].vend - segs[s].vstart);

        if ((segs[s].p_flags & PF_R) != 0U) {
            prot |= HAL_VM_PROT_READ;
        }
        if ((segs[s].p_flags & PF_W) != 0U) {
            prot |= HAL_VM_PROT_WRITE;
        }
        if ((segs[s].p_flags & PF_X) != 0U) {
            prot |= HAL_VM_PROT_EXEC | HAL_VM_PROT_READ;
#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
            if (arm64_get_cpu_caps()->has_bti) {
                prot |= HAL_VM_PROT_BTI;
            }
#endif
        }

        /* 代码段与只读数据段永久 Seal 并刷 I-Cache */
        if ((segs[s].p_flags & PF_W) == 0U) {
#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
            if ((segs[s].p_flags & PF_X) != 0U) {
                for (uint32_t p = 0U; p < segs[s].vmo->page_count; p++) {
                    if (segs[s].vmo->pages[p] != NULL && segs[s].vmo->pages[p]->kvaddr != 0U) {
                        arm64_sync_icache_dcache(segs[s].vmo->pages[p]->kvaddr, PAGE_SIZE);
                    }
                }
            }
#endif
            rc = nos_vmo_seal(segs[s].vmo);
            if (rc != 0) {
                nos_elf64_unload_image(out_img);
                return rc;
            }
        }

        rc = nos_vmo_map(segs[s].vmo, nos_cell_vm_space(cell),
                         (uintptr_t)segs[s].vstart, 0U, seg_size, prot, cell);
        if (rc != 0) {
            nos_elf64_unload_image(out_img);
            return rc;
        }
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
    img->load_bias = 0U;
    img->is_pie = false;
}

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)

struct nos_elf64_proc {
    struct nos_completion    done;
    struct nos_elf64_image   img;
    struct nos_cell         *cell;
    struct nos_vmo          *stack_vmo;
    struct nos_vmo          *aux_vmo;
    struct nos_accel_device *accel_dev;
    struct task_struct      *task;
    uintptr_t                entry_va;
    uintptr_t                stack_top;
    uintptr_t                aux_va;
    int                      exit_code;
};

static void elf64_task_entry(void *param)
{
    struct nos_elf64_proc *proc = (struct nos_elf64_proc *)param;

    arm64_enter_user_mode(proc->entry_va, proc->stack_top, proc->aux_va);
}

static void elf64_task_cleanup(struct task_struct *task)
{
    struct nos_elf64_proc *proc;

    if (task == NULL || task->parameter == NULL) {
        return;
    }
    proc = (struct nos_elf64_proc *)task->parameter;
    proc->exit_code = (int)task->exit_code;
    (void)nos_complete(&proc->done);
}

static void elf64_proc_release(struct nos_elf64_proc *proc)
{
    if (proc == NULL) {
        return;
    }
    if (proc->accel_dev != NULL) {
        (void)nos_object_put(&proc->accel_dev->header);
        proc->accel_dev = NULL;
    }
    if (proc->aux_vmo != NULL) {
        (void)nos_object_close(&proc->aux_vmo->header);
        (void)nos_object_retire(&proc->aux_vmo->header);
        proc->aux_vmo = NULL;
    }
    if (proc->stack_vmo != NULL) {
        (void)nos_object_close(&proc->stack_vmo->header);
        (void)nos_object_retire(&proc->stack_vmo->header);
        proc->stack_vmo = NULL;
    }
    nos_elf64_unload_image(&proc->img);
    if (proc->cell != NULL) {
        (void)nos_cell_destroy(proc->cell);
        proc->cell = NULL;
    }
    kfree(proc);
}

static int elf64_pack_argv_stack(struct nos_vmo *stack_vmo,
                                 const char *path,
                                 int req_argc,
                                 const char *const *req_argv,
                                 uint32_t *out_argc,
                                 uint64_t *out_argv_va,
                                 uintptr_t *out_stack_top)
{
    const char *args[NOS_ELF64_MAX_ARGC];
    uint64_t arg_vas[NOS_ELF64_MAX_ARGC + 1U];
    uint32_t argc = 0U;
    size_t sp_off = NOS_ELF64_STACK_SIZE;
    size_t table_bytes;
    int rc;

    memset(arg_vas, 0, sizeof(arg_vas));
    if (req_argc > 0 && req_argv != NULL) {
        argc = ((uint32_t)req_argc > NOS_ELF64_MAX_ARGC) ?
               NOS_ELF64_MAX_ARGC : (uint32_t)req_argc;
        for (uint32_t i = 0U; i < argc; i++) {
            args[i] = (req_argv[i] != NULL) ? req_argv[i] : "";
        }
    } else {
        argc = 1U;
        args[0] = path;
    }

    for (uint32_t i = 0U; i < argc; i++) {
        size_t len = strlen(args[i]) + 1U;
        if (len > 256U || sp_off < len + 512U) {
            return -E2BIG;
        }
        sp_off -= len;
        rc = nos_vmo_write(stack_vmo, sp_off, args[i], len);
        if (rc != 0) {
            return rc;
        }
        arg_vas[i] = (uint64_t)NOS_ELF64_STACK_VA + (uint64_t)sp_off;
    }

    arg_vas[argc] = 0ULL;
    sp_off &= ~15ULL;
    table_bytes = ((size_t)argc + 1U) * sizeof(uint64_t);
    if (sp_off < table_bytes + 128U) {
        return -E2BIG;
    }
    sp_off -= table_bytes;
    sp_off &= ~15ULL;

    rc = nos_vmo_write(stack_vmo, sp_off, arg_vas, table_bytes);
    if (rc != 0) {
        return rc;
    }

    *out_argc = argc;
    *out_argv_va = (uint64_t)NOS_ELF64_STACK_VA + (uint64_t)sp_off;
    *out_stack_top = (uintptr_t)(NOS_ELF64_STACK_VA + sp_off);
    return 0;
}

int nos_elf64_start_from_vfs_ex(const char *path,
                                const struct nos_elf64_spawn_opts *opts,
                                struct nos_elf64_proc **out_proc)
{
    struct nos_cell_budget def_budget = {
        .cpu_ns_limit     = 5000000000ULL,
        .mem_bytes_limit  = 512ULL * 1024ULL,
        .tool_calls_limit = 16ULL,
        .accel_ops_limit  = 8ULL,
        .max_child_cells  = 0U,
        .flags            = 0U,
    };
    const struct nos_cell_budget *use_budget =
        (opts != NULL && opts->budget != NULL) ? opts->budget : &def_budget;
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
    struct nos_elf64_proc *proc = NULL;
    struct nos_user_boot_info boot_info;
    struct arm64_sec_status sec_st;
    const struct arm64_cpu_caps *cpu_caps;
    nos_handle_t accel_h = NOS_HANDLE_INVALID;
    nos_handle_t ring_h = NOS_HANDLE_INVALID;
    void *file_buf = NULL;
    loff_t file_len;
    size_t got = 0U;
    uint32_t eff_argc = 0U;
    uint64_t argv_va = 0ULL;
    uintptr_t stack_top = NOS_ELF64_STACK_VA + NOS_ELF64_STACK_SIZE;
    int fd;
    int rc;

    if (path == NULL || out_proc == NULL) {
        return -EINVAL;
    }
    *out_proc = NULL;

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

    proc = (struct nos_elf64_proc *)kzalloc(sizeof(*proc), GFP_KERNEL);
    if (proc == NULL) {
        kfree(file_buf);
        return -ENOMEM;
    }

    proc->cell = nos_cell_create(NULL, use_budget);
    if (proc->cell == NULL) {
        kfree(file_buf);
        kfree(proc);
        return -ENOMEM;
    }

    rc = nos_elf64_load_into_cell(file_buf, (size_t)file_len, proc->cell, &proc->img);
    kfree(file_buf);
    file_buf = NULL;
    if (rc != 0) {
        goto cleanup;
    }

    proc->stack_vmo = nos_vmo_create(&stack_args);
    proc->aux_vmo = nos_vmo_create(&aux_args);
    if (proc->stack_vmo == NULL || proc->aux_vmo == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }

    rc = elf64_pack_argv_stack(proc->stack_vmo,
                               path,
                               (opts != NULL) ? opts->argc : 0,
                               (opts != NULL) ? opts->argv : NULL,
                               &eff_argc,
                               &argv_va,
                               &stack_top);
    if (rc != 0) {
        goto cleanup;
    }

    rc = nos_vmo_map(proc->stack_vmo, nos_cell_vm_space(proc->cell),
                     NOS_ELF64_STACK_VA, 0U, NOS_ELF64_STACK_SIZE,
                     HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE, proc->cell);
    if (rc == 0) {
        rc = nos_cell_map_telemetry(proc->cell, NOS_ELF64_TELEM_VA);
    }
    if (rc != 0) {
        goto cleanup;
    }

    proc->accel_dev = nos_accel_get_default();
    if (proc->accel_dev != NULL) {
        (void)nos_cap_alloc(nos_cell_cap_table(proc->cell), NOS_CAP_ACCEL, proc->accel_dev,
                            NOS_CAP_RIGHT_READ | NOS_CAP_RIGHT_SEND, 0U, &accel_h);
    }

    if (opts != NULL && opts->shared_ring != NULL) {
        uint32_t rrights = (opts->ring_rights != 0U) ? opts->ring_rights :
                           NOS_CAP_RIGHT_ALL;
        rc = nos_cap_alloc(nos_cell_cap_table(proc->cell), NOS_CAP_RING,
                           opts->shared_ring, rrights, 0U, &ring_h);
        if (rc != 0) {
            goto cleanup;
        }
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
    boot_info.argc = eff_argc;
    boot_info.ring_cap = ring_h;
    boot_info.argv_va = argv_va;

    rc = nos_vmo_write(proc->aux_vmo, 0U, &boot_info, sizeof(boot_info));
    if (rc == 0) {
        rc = nos_vmo_seal(proc->aux_vmo);
    }
    if (rc == 0) {
        rc = nos_vmo_map(proc->aux_vmo, nos_cell_vm_space(proc->cell),
                         NOS_ELF64_AUX_VA, 0U, PAGE_SIZE,
                         HAL_VM_PROT_USER | HAL_VM_PROT_READ, NULL);
    }
    if (rc != 0) {
        goto cleanup;
    }

    (void)nos_completion_init(&proc->done);
    proc->entry_va = proc->img.entry_va;
    proc->stack_top = stack_top;
    proc->aux_va = NOS_ELF64_AUX_VA;
    proc->exit_code = -1;

    proc->task = task_create_ex("elf64_agent", elf64_task_entry, proc,
                                SCHED_POLICY_NORMAL, 8U, 0, NOS_CPU_MASK_ALL,
                                CONFIG_CORE_TASK_STACK_SIZE, 10U, elf64_task_cleanup);
    if (proc->task == NULL) {
        (void)nos_object_close(&proc->done.object);
        (void)nos_object_retire(&proc->done.object);
        rc = -ENOMEM;
        goto cleanup;
    }

    rc = nos_cell_attach_task(proc->task, proc->cell);
    if (rc != 0) {
        task_del(proc->task);
        (void)nos_completion_wait(&proc->done, 3000U);
        (void)nos_object_close(&proc->done.object);
        (void)nos_object_retire(&proc->done.object);
        goto cleanup;
    }

    (void)task_ready(proc->task);
    *out_proc = proc;
    return 0;

cleanup:
    elf64_proc_release(proc);
    return rc;
}

int nos_elf64_wait_proc(struct nos_elf64_proc *proc,
                        uint32_t timeout_ms,
                        int *out_exit_code,
                        struct nos_cell_telemetry *out_telemetry)
{
    nos_timeout_t wait_ticks;
    int rc;

    if (proc == NULL) {
        return -EINVAL;
    }

    wait_ticks = (timeout_ms == UINT32_MAX || timeout_ms == 0U) ?
                 10000U : (nos_timeout_t)timeout_ms;
    rc = nos_completion_wait(&proc->done, wait_ticks);
    if (rc != 0 && proc->task != NULL) {
        task_del(proc->task);
        (void)nos_completion_wait(&proc->done, 3000U);
    }
    (void)nos_object_close(&proc->done.object);
    (void)nos_object_retire(&proc->done.object);

    if (out_exit_code != NULL) {
        *out_exit_code = proc->exit_code;
    }
    if (out_telemetry != NULL && proc->cell != NULL) {
        const struct nos_cell_telemetry *tm = nos_cell_telemetry_view(proc->cell);
        if (tm != NULL) {
            *out_telemetry = *tm;
        } else {
            memset(out_telemetry, 0, sizeof(*out_telemetry));
        }
    }

    elf64_proc_release(proc);
    return rc;
}

int nos_elf64_spawn_from_vfs_argv(const char *path,
                                  int argc,
                                  const char *const *argv,
                                  const struct nos_cell_budget *budget,
                                  int *out_exit_code,
                                  struct nos_cell_telemetry *out_telemetry)
{
    struct nos_elf64_spawn_opts opts;
    struct nos_elf64_proc *proc = NULL;
    int rc;

    memset(&opts, 0, sizeof(opts));
    opts.argc = argc;
    opts.argv = argv;
    opts.budget = budget;

    rc = nos_elf64_start_from_vfs_ex(path, &opts, &proc);
    if (rc != 0) {
        return rc;
    }
    return nos_elf64_wait_proc(proc, 10000U, out_exit_code, out_telemetry);
}

int nos_elf64_spawn_from_vfs(const char *path,
                             const struct nos_cell_budget *budget,
                             int *out_exit_code,
                             struct nos_cell_telemetry *out_telemetry)
{
    return nos_elf64_spawn_from_vfs_argv(path, 0, NULL, budget,
                                         out_exit_code, out_telemetry);
}

#else

int nos_elf64_start_from_vfs_ex(const char *path,
                                const struct nos_elf64_spawn_opts *opts,
                                struct nos_elf64_proc **out_proc)
{
    (void)path;
    (void)opts;
    (void)out_proc;
    return -ENOSYS;
}

int nos_elf64_wait_proc(struct nos_elf64_proc *proc,
                        uint32_t timeout_ms,
                        int *out_exit_code,
                        struct nos_cell_telemetry *out_telemetry)
{
    (void)proc;
    (void)timeout_ms;
    (void)out_exit_code;
    (void)out_telemetry;
    return -ENOSYS;
}

int nos_elf64_spawn_from_vfs_argv(const char *path,
                                  int argc,
                                  const char *const *argv,
                                  const struct nos_cell_budget *budget,
                                  int *out_exit_code,
                                  struct nos_cell_telemetry *out_telemetry)
{
    (void)path;
    (void)argc;
    (void)argv;
    (void)budget;
    (void)out_exit_code;
    (void)out_telemetry;
    return -ENOSYS;
}

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
