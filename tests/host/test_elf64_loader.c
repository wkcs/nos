/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端 TDD 测试：ELF64 严格 W^X 校验、段加载、Sealed 只读及越界防御
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/nos/errno.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <nos/cap.h>
#include <nos/cell.h>
#include <nos/elf64.h>
#include <nos/hal_arch.h>
#include <nos/vmo.h>
#include <asm/mmu.h>

uintptr_t kernel_running = 1;

void *kmalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return malloc(size);
}

void *kzalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return calloc(1, size);
}

void kfree(void *ptr)
{
    free(ptr);
}

uint64_t hal_timer_monotonic_ns(void)
{
    return 1000000ULL;
}

#define PT_POOL_PAGES 64U
#define PT_POOL_PBASE 0x80000000ULL

static uint8_t g_pt_mem[PT_POOL_PAGES][4096];
static bool g_pt_used[PT_POOL_PAGES];
static uint32_t g_pt_active;

static uintptr_t pt_alloc(void *ctx)
{
    (void)ctx;
    for (uint32_t i = 0U; i < PT_POOL_PAGES; i++) {
        if (!g_pt_used[i]) {
            g_pt_used[i] = true;
            g_pt_active++;
            memset(g_pt_mem[i], 0, 4096U);
            return (uintptr_t)(PT_POOL_PBASE + (uint64_t)i * 4096ULL);
        }
    }
    return 0U;
}

static void pt_free(void *ctx, uintptr_t pa)
{
    (void)ctx;
    uint64_t idx = (pa - PT_POOL_PBASE) / 4096ULL;
    assert(idx < PT_POOL_PAGES && g_pt_used[idx]);
    g_pt_used[idx] = false;
    g_pt_active--;
}

static void *pt_p2v(void *ctx, uintptr_t pa)
{
    (void)ctx;
    return &g_pt_mem[(pa - PT_POOL_PBASE) / 4096ULL][0];
}

static const struct arm64_pt_backend g_be = {
    .alloc_pt_page   = pt_alloc,
    .free_pt_page    = pt_free,
    .phys_to_pt_virt = pt_p2v,
    .ctx             = NULL,
};

static uint16_t g_asid = 1U;

int hal_vm_space_init(struct nos_vm_space *space)
{
    memset(space, 0, sizeof(*space));
    space->root_pt_phys = pt_alloc(NULL);
    assert(space->root_pt_phys != 0U);
    space->asid = g_asid++;
    space->initialized = true;
    return 0;
}

void hal_vm_space_destroy(struct nos_vm_space *space)
{
    if (space != NULL && space->initialized) {
        arm64_pt_destroy(space->root_pt_phys, 39, &g_be);
        space->initialized = false;
    }
}

int hal_vm_map(struct nos_vm_space *space, uintptr_t vaddr, uintptr_t paddr,
               size_t size, uint32_t prot_flags)
{
    return arm64_pt_map_range(space->root_pt_phys, 39, vaddr, paddr, size,
                              prot_flags, true, &g_be);
}

int hal_vm_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    return arm64_pt_unmap_range(space->root_pt_phys, 39, vaddr, size, false, &g_be);
}

void hal_vm_switch(struct nos_vm_space *next_space) { (void)next_space; }
struct task_struct *task_current(void) { return NULL; }
int nos_notification_init(struct nos_notification *n) { (void)n; return 0; }
int nos_notification_close(struct nos_notification *n) { (void)n; return 0; }
int nos_notify_signal_obj(struct nos_notification *n, uint64_t bits) { (void)n; (void)bits; return 0; }
int nos_notify_wait_obj(struct nos_notification *n, uint64_t *b, nos_timeout_t t) { (void)n; (void)b; (void)t; return -EPIPE; }

static void build_test_elf(uint8_t *buf, size_t buf_sz, uint16_t machine,
                           uint32_t text_flags, uint64_t text_vaddr,
                           uint64_t ro_vaddr, uint64_t rw_vaddr)
{
    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
    Elf64_Phdr *ph = (Elf64_Phdr *)(buf + sizeof(Elf64_Ehdr));

    memset(buf, 0, buf_sz);
    eh->e_ident[0] = ELF_MAGIC0;
    eh->e_ident[1] = ELF_MAGIC1;
    eh->e_ident[2] = ELF_MAGIC2;
    eh->e_ident[3] = ELF_MAGIC3;
    eh->e_ident[4] = ELFCLASS64;
    eh->e_ident[5] = ELFDATA2LSB;
    eh->e_ident[6] = 1U;
    eh->e_type = ET_EXEC;
    eh->e_machine = machine;
    eh->e_version = 1U;
    eh->e_entry = text_vaddr;
    eh->e_phoff = sizeof(Elf64_Ehdr);
    eh->e_ehsize = sizeof(Elf64_Ehdr);
    eh->e_phentsize = sizeof(Elf64_Phdr);
    eh->e_phnum = 3U;

    /* 段 0: R-X .text (512B 偏移起) */
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = text_flags;
    ph[0].p_offset = 512U;
    ph[0].p_vaddr = text_vaddr;
    ph[0].p_filesz = 16U;
    ph[0].p_memsz = 16U;
    ph[0].p_align = 4096U;
    uint32_t insn[4] = { 0xD503201FU, 0xD2800000U, 0xD2800028U, 0xD4000001U };
    memcpy(buf + 512U, insn, sizeof(insn));

    /* 段 1: R-- .rodata (640B 偏移起) */
    ph[1].p_type = PT_LOAD;
    ph[1].p_flags = PF_R;
    ph[1].p_offset = 640U;
    ph[1].p_vaddr = ro_vaddr;
    ph[1].p_filesz = 8U;
    ph[1].p_memsz = 8U;
    ph[1].p_align = 4096U;
    memcpy(buf + 640U, "NOS_ELF6", 8U);

    /* 段 2: RW- .data + .bss (768B 偏移起, memsz=8192 验证零填充 BSS 页) */
    ph[2].p_type = PT_LOAD;
    ph[2].p_flags = PF_R | PF_W;
    ph[2].p_offset = 768U;
    ph[2].p_vaddr = rw_vaddr;
    ph[2].p_filesz = 4U;
    ph[2].p_memsz = 8192U;
    ph[2].p_align = 4096U;
    uint32_t data_magic = 0xCAFEBABEU;
    memcpy(buf + 768U, &data_magic, sizeof(data_magic));
}

int main(void)
{
    uint8_t elf[4096];
    uintptr_t entry = 0U;
    struct nos_cell_budget b = {
        .cpu_ns_limit = 1000000000ULL,
        .mem_bytes_limit = 262144ULL,
        .tool_calls_limit = 4ULL,
        .accel_ops_limit = 4ULL,
        .max_child_cells = 0U,
    };
    struct nos_cell *cell;
    struct nos_elf64_image img;
    uintptr_t pa = 0U;
    uint64_t pte = 0ULL;

    /* 1. 合法 3 段 ELF64 加载与 HW 页表权限/Sealed 状态校验 */
    build_test_elf(elf, sizeof(elf), EM_AARCH64, PF_R | PF_X,
                   0x400000ULL, 0x410000ULL, 0x420000ULL);
    assert(nos_elf64_validate(elf, sizeof(elf), &entry) == 0);
    assert(entry == 0x400000UL);

    cell = nos_cell_create(NULL, &b);
    assert(cell != NULL);
    assert(nos_elf64_load_into_cell(elf, sizeof(elf), cell, &img) == 0);
    assert(img.seg_count == 3U && img.entry_va == 0x400000UL);
    assert(img.seg_vmos[0]->sealed && img.seg_vmos[1]->sealed && !img.seg_vmos[2]->sealed);

    /* 验证代码段内容 & 页表 R-X（UXN=0, AP_RO=1） */
    uint32_t read_insn = 0U;
    assert(nos_vmo_read(img.seg_vmos[0], 0U, &read_insn, sizeof(read_insn)) == 0);
    assert(read_insn == 0xD503201FU);
    assert(arm64_pt_translate(nos_cell_vm_space(cell)->root_pt_phys, 39,
                              0x400000UL, &pa, &pte, &g_be) == 0);
    assert((pte & ARM64_PTE_AP_RO) != 0ULL && (pte & ARM64_PTE_UXN) == 0ULL);

    /* 验证只读段（AP_RO=1, UXN=1） */
    assert(arm64_pt_translate(nos_cell_vm_space(cell)->root_pt_phys, 39,
                              0x410000UL, &pa, &pte, &g_be) == 0);
    assert((pte & ARM64_PTE_AP_RO) != 0ULL && (pte & ARM64_PTE_UXN) != 0ULL);

    /* 验证可写 RW 段（AP_RO=0, UXN=1）及第二页 BSS 自动归零 */
    uint32_t read_data = 0U;
    uint32_t bss_zero = 0x12345678U;
    assert(nos_vmo_read(img.seg_vmos[2], 0U, &read_data, 4U) == 0);
    assert(read_data == 0xCAFEBABEU);
    assert(nos_vmo_read(img.seg_vmos[2], 4096U, &bss_zero, 4U) == 0);
    assert(bss_zero == 0U);
    assert(arm64_pt_translate(nos_cell_vm_space(cell)->root_pt_phys, 39,
                              0x421000UL, &pa, &pte, &g_be) == 0);
    assert((pte & ARM64_PTE_AP_RO) == 0ULL && (pte & ARM64_PTE_UXN) != 0ULL);

    /* 2. 安全拦截测试 */
    /* W^X 同时置位 PF_W | PF_X 必须拒绝 (-EPERM) */
    build_test_elf(elf, sizeof(elf), EM_AARCH64, PF_R | PF_W | PF_X,
                   0x400000ULL, 0x410000ULL, 0x420000ULL);
    assert(nos_elf64_validate(elf, sizeof(elf), NULL) == -EPERM);

    /* 非法高半核内核空间覆盖必须拒绝 (-EFAULT) */
    build_test_elf(elf, sizeof(elf), EM_AARCH64, PF_R | PF_X,
                   0x400000ULL, 0x410000ULL, 0xFFFFFFF000000000ULL);
    assert(nos_elf64_validate(elf, sizeof(elf), NULL) == -EFAULT);

    /* 侵占 EL0 栈/Aux 保留区必须拒绝 (-EPERM) */
    build_test_elf(elf, sizeof(elf), EM_AARCH64, PF_R | PF_X,
                   0x400000ULL, 0x410000ULL, NOS_ELF64_STACK_VA);
    assert(nos_elf64_validate(elf, sizeof(elf), NULL) == -EPERM);

    /* 共享同一 4KB 页的不同权限段必须拒绝 (-EPERM) */
    build_test_elf(elf, sizeof(elf), EM_AARCH64, PF_R | PF_X,
                   0x400000ULL, 0x400800ULL, 0x420000ULL);
    assert(nos_elf64_validate(elf, sizeof(elf), NULL) == -EPERM);

    /* 非 AArch64 架构机器码必须拒绝 (-ENOEXEC) */
    build_test_elf(elf, sizeof(elf), 62U /* x86_64 */, PF_R | PF_X,
                   0x400000ULL, 0x410000ULL, 0x420000ULL);
    assert(nos_elf64_validate(elf, sizeof(elf), NULL) == -ENOEXEC);

    /* 截断文件防御 (-ENOEXEC) */
    assert(nos_elf64_validate(elf, 32U, NULL) == -ENOEXEC);

    nos_elf64_unload_image(&img);
    assert(nos_cell_destroy(cell) == 0);

    /* 3. ET_DYN PIE + PT_DYNAMIC + R_AARCH64_RELATIVE 重定位与安全拦截测试 */
    {
        uint8_t pie[4096];
        Elf64_Ehdr *eh = (Elf64_Ehdr *)pie;
        Elf64_Phdr *ph = (Elf64_Phdr *)(pie + sizeof(Elf64_Ehdr));
        Elf64_Dyn *dyn;
        Elf64_Rela *rela;
        struct nos_elf64_image pie_img;
        struct nos_cell *pie_cell;
        uint64_t reloc_val = 0ULL;
        uintptr_t pie_entry = 0U;

        memset(pie, 0, sizeof(pie));
        eh->e_ident[0] = ELF_MAGIC0;
        eh->e_ident[1] = ELF_MAGIC1;
        eh->e_ident[2] = ELF_MAGIC2;
        eh->e_ident[3] = ELF_MAGIC3;
        eh->e_ident[4] = ELFCLASS64;
        eh->e_ident[5] = ELFDATA2LSB;
        eh->e_ident[6] = 1U;
        eh->e_type = ET_DYN;
        eh->e_machine = EM_AARCH64;
        eh->e_version = 1U;
        eh->e_entry = 0x0ULL;
        eh->e_phoff = sizeof(Elf64_Ehdr);
        eh->e_ehsize = sizeof(Elf64_Ehdr);
        eh->e_phentsize = sizeof(Elf64_Phdr);
        eh->e_phnum = 4U;

        /* 段 0: R-X .text (link_vaddr=0x0) */
        ph[0].p_type = PT_LOAD;
        ph[0].p_flags = PF_R | PF_X;
        ph[0].p_offset = 512U;
        ph[0].p_vaddr = 0x0ULL;
        ph[0].p_filesz = 16U;
        ph[0].p_memsz = 16U;
        ph[0].p_align = 4096U;
        uint32_t pie_insn[4] = { 0xD503245FU, 0xD2800000U, 0xD2800028U, 0xD4000001U };
        memcpy(pie + 512U, pie_insn, sizeof(pie_insn));

        /* 段 1: R-- .rodata + .rela.dyn (link_vaddr=0x10000) */
        ph[1].p_type = PT_LOAD;
        ph[1].p_flags = PF_R;
        ph[1].p_offset = 640U;
        ph[1].p_vaddr = 0x10000ULL;
        ph[1].p_filesz = 128U;
        ph[1].p_memsz = 128U;
        ph[1].p_align = 4096U;
        memcpy(pie + 640U, "PIE_MSG\0", 8U);
        /* 在 .rela.dyn (vaddr=0x10020) 放置 2 项 R_AARCH64_RELATIVE:
         * 项 0 更新 .rodata+0x10 指向 .rodata ("PIE_MSG")
         * 项 1 更新 .data+0x0 指向 .text+0x08
         */
        rela = (Elf64_Rela *)(pie + 640U + 0x20U);
        rela[0].r_offset = 0x10010ULL;
        rela[0].r_info = (uint64_t)R_AARCH64_RELATIVE;
        rela[0].r_addend = 0x10000LL;
        rela[1].r_offset = 0x20000ULL;
        rela[1].r_info = (uint64_t)R_AARCH64_RELATIVE;
        rela[1].r_addend = 8LL;

        /* 段 2: RW- .data + .dynamic (link_vaddr=0x20000) */
        ph[2].p_type = PT_LOAD;
        ph[2].p_flags = PF_R | PF_W;
        ph[2].p_offset = 1024U;
        ph[2].p_vaddr = 0x20000ULL;
        ph[2].p_filesz = 128U;
        ph[2].p_memsz = 4096U;
        ph[2].p_align = 4096U;

        dyn = (Elf64_Dyn *)(pie + 1024U + 32U);
        dyn[0].d_tag = DT_RELA;    dyn[0].d_val = 0x10020ULL;
        dyn[1].d_tag = DT_RELASZ;  dyn[1].d_val = 2ULL * sizeof(Elf64_Rela);
        dyn[2].d_tag = DT_RELAENT; dyn[2].d_val = sizeof(Elf64_Rela);
        dyn[3].d_tag = DT_NULL;    dyn[3].d_val = 0ULL;

        /* 段 3: PT_DYNAMIC */
        ph[3].p_type = PT_DYNAMIC;
        ph[3].p_flags = PF_R | PF_W;
        ph[3].p_offset = 1024U + 32U;
        ph[3].p_vaddr = 0x20020ULL;
        ph[3].p_filesz = 4ULL * sizeof(Elf64_Dyn);
        ph[3].p_memsz = 4ULL * sizeof(Elf64_Dyn);
        ph[3].p_align = 8U;

        assert(nos_elf64_validate(pie, sizeof(pie), &pie_entry) == 0);
        assert(pie_entry == NOS_ELF64_PIE_BASE);

        pie_cell = nos_cell_create(NULL, &b);
        assert(pie_cell != NULL);
        assert(nos_elf64_load_into_cell(pie, sizeof(pie), pie_cell, &pie_img) == 0);
        assert(pie_img.is_pie && pie_img.load_bias == NOS_ELF64_PIE_BASE);
        assert(pie_img.seg_vmos[0]->sealed && pie_img.seg_vmos[1]->sealed && !pie_img.seg_vmos[2]->sealed);

        /* 核验 R_AARCH64_RELATIVE 已在 Seal 前正确回填至 .rodata 与 .data */
        assert(nos_vmo_read(pie_img.seg_vmos[1], 0x10U, &reloc_val, sizeof(reloc_val)) == 0);
        assert(reloc_val == (uint64_t)(NOS_ELF64_PIE_BASE + 0x10000ULL));
        assert(nos_vmo_read(pie_img.seg_vmos[2], 0x0U, &reloc_val, sizeof(reloc_val)) == 0);
        assert(reloc_val == (uint64_t)(NOS_ELF64_PIE_BASE + 8ULL));

        nos_elf64_unload_image(&pie_img);

        /* 负向 1：禁止重定位写入 PF_X 代码页 (-EPERM) */
        rela[1].r_offset = 0x0ULL;
        assert(nos_elf64_load_into_cell(pie, sizeof(pie), pie_cell, &pie_img) == -EPERM);

        /* 负向 2：拒绝未知动态重定位类型 (-ENOEXEC) */
        rela[1].r_offset = 0x20000ULL;
        rela[1].r_info = 257ULL; /* R_AARCH64_ABS64 */
        assert(nos_elf64_load_into_cell(pie, sizeof(pie), pie_cell, &pie_img) == -ENOEXEC);

        assert(nos_cell_destroy(pie_cell) == 0);
    }

    assert(g_pt_active == 0U);

    printf("PASS: test_elf64_loader (3-seg W^X, ET_DYN PIE R_AARCH64_RELATIVE, Seal+RO/RW/RX PTE, BSS zero, -EPERM/-EFAULT guards)\n");
    return 0;
}
