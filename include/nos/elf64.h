/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS ARM64 ELF64 严格 W^X 加载器与 EL0 沙箱启动契约
 */

#ifndef __NOS_ELF64_H__
#define __NOS_ELF64_H__

#include <uapi/nos/ai.h>
#include <uapi/nos/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nos_cell;
struct nos_vmo;

#define ELF_MAGIC0              0x7FU
#define ELF_MAGIC1              'E'
#define ELF_MAGIC2              'L'
#define ELF_MAGIC3              'F'

#define ELFCLASS64              2U
#define ELFDATA2LSB             1U
#define ET_EXEC                 2U
#define ET_DYN                  3U
#define EM_AARCH64              183U

#define PT_NULL                 0U
#define PT_LOAD                 1U
#define PT_GNU_STACK            0x6474E551U

#define PF_X                    (1U << 0)
#define PF_W                    (1U << 1)
#define PF_R                    (1U << 2)

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define NOS_ELF64_MAX_SEGS      8U

/* ELF64 用户进程在 Cell TTBR0 中的标准栈、Telemetry 与 BootInfo 地址约定 */
#define NOS_ELF64_USER_MIN_VA   0x0000000000010000UL
#define NOS_ELF64_USER_MAX_VA   0x0000008000000000UL
#define NOS_ELF64_STACK_VA      0x00000000007C0000UL
#define NOS_ELF64_STACK_SIZE    (4UL * 4096UL)
#define NOS_ELF64_TELEM_VA      0x00000000007E0000UL
#define NOS_ELF64_AUX_VA        0x00000000007F0000UL

#define NOS_USER_BOOT_MAGIC     0x4E4F5355U /* "NOSU" */
#define NOS_USER_FLAG_SVE       (1U << 0)
#define NOS_USER_FLAG_PAC       (1U << 1)
#define NOS_USER_FLAG_BTI       (1U << 2)
#define NOS_USER_FLAG_MTE       (1U << 3)

/* EL0 启动引导页结构（x0 指向此 64B 结构只读副本） */
struct nos_user_boot_info {
    uint32_t magic;
    uint32_t version;
    uint32_t accel_cap;
    uint32_t flags;
    uint64_t telemetry_va;
    uint64_t reserved[5];
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(Elf64_Ehdr) == 64, "Elf64_Ehdr 必须为 64 字节");
_Static_assert(sizeof(Elf64_Phdr) == 56, "Elf64_Phdr 必须为 56 字节");
_Static_assert(sizeof(struct nos_user_boot_info) == 64, "nos_user_boot_info 必须为 64 字节");
#endif

struct nos_elf64_image {
    uintptr_t       entry_va;
    uint32_t        seg_count;
    struct nos_vmo *seg_vmos[NOS_ELF64_MAX_SEGS];
};

int nos_elf64_validate(const void *buf, size_t size, uintptr_t *out_entry);
int nos_elf64_load_into_cell(const void *buf,
                             size_t size,
                             struct nos_cell *cell,
                             struct nos_elf64_image *out_img);
void nos_elf64_unload_image(struct nos_elf64_image *img);

int nos_elf64_spawn_from_vfs(const char *path,
                             const struct nos_cell_budget *budget,
                             int *out_exit_code,
                             struct nos_cell_telemetry *out_telemetry);

#endif /* __NOS_ELF64_H__ */
