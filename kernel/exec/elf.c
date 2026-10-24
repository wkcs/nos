/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
#include "elf.h"
#include "linux.h"
/* A native pointer with its high bit set is valid on RV32 physical RAM. */
static int pointer_error(intptr_t result) { return (uintptr_t)result >= (uintptr_t)-4095; }
intptr_t vm_load_protect(struct task *task, uintptr_t va, unsigned prot, size_t bytes);

#if UINTPTR_MAX == UINT64_MAX
struct elf_header { unsigned char ident[16]; uint16_t type, machine; uint32_t version;
    uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct elf_segment { uint32_t type, flags; uint64_t offset, va, pa, filesz, memsz, align; };
#define ELF_CLASS 2
#else
struct elf_header { unsigned char ident[16]; uint16_t type, machine; uint32_t version;
    uint32_t entry, phoff, shoff, flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct elf_segment { uint32_t type, offset, va, pa, filesz, memsz, flags, align; };
#define ELF_CLASS 1
#endif
#if defined(NOS_XTENSA)
#define ELF_MACHINE 94
#elif defined(NOS_ARM64)
#define ELF_MACHINE 183
#elif defined(NOS_RISCV)
#define ELF_MACHINE 243
#else
#define ELF_MACHINE 40
#endif
#define ELF_MAX_PHDR 16u
#ifdef NOS_MMU
#define ELF_MAX_IMAGE (512u * 1024u)
#else
#define ELF_MAX_IMAGE (64u * 1024u)
#endif
#define ROUND_PAGE(v) (((v) + NOS_PAGE_SIZE - 1u) & ~(uintptr_t)(NOS_PAGE_SIZE - 1u))
static int extent(uintptr_t offset, size_t count, size_t bound)
{ return offset <= bound && count <= bound - offset; }
#ifndef NOS_MMU
static int segment_range(const struct elf_segment *loads, unsigned count, uintptr_t address,
                          size_t bytes, unsigned flags, int file_backed)
{
    for (unsigned i = 0; i < count; i++) {
        const struct elf_segment *segment = &loads[i];
        size_t bound = file_backed ? (size_t)segment->filesz : (size_t)segment->memsz;
        if ((segment->flags & flags) == flags && address >= segment->va &&
            extent(address - segment->va, bytes, bound)) return 1;
    }
    return 0;
}
#endif
static int copy_string(char *destination, uintptr_t source)
{
    if (!source) return -NOS_EFAULT;
    for (unsigned i = 0; i < NOS_EXEC_STRLEN; i++) {
        if (source > UINTPTR_MAX - i || user_copy_in(&destination[i], source + i, 1)) return -NOS_EFAULT;
        if (!destination[i]) return 0;
    }
    return -NOS_ENAMETOOLONG;
}
#ifdef NOS_LINUX_COMPAT
static const char *argument(const struct nos_exec_args *args, unsigned i, unsigned environment)
{ return args->strings + args->offsets[(environment ? args->argc : 0) + i]; }
static int append_argument(struct nos_exec_args *args, uintptr_t source, const char *kernel_string, unsigned environment)
{
    unsigned slot = args->argc + args->envc;
    size_t overhead = (slot + 3u) * sizeof(uintptr_t);
    if (slot >= NOS_EXEC_ARGC || overhead >= NOS_EXEC_BYTES || args->used >= NOS_EXEC_BYTES - overhead)
        return -NOS_E2BIG;
    unsigned start = args->used;
    while (args->used < NOS_EXEC_BYTES - overhead) {
        unsigned offset = args->used - start;
        size_t bytes = 1;
        if (kernel_string) args->strings[args->used] = kernel_string[offset];
        else {
            if (!source || source > UINTPTR_MAX - offset) return -NOS_EFAULT;
            uintptr_t address = source + offset;
            bytes = NOS_PAGE_SIZE - (address & (NOS_PAGE_SIZE - 1));
            size_t remaining = NOS_EXEC_BYTES - overhead - args->used;
            if (bytes > remaining) bytes = remaining;
            if (bytes > 64) bytes = 64;
            /* Stay within one source page, including strings terminated just
             * before a guard page.  A narrow software range may end earlier;
             * fall back to one byte so a preceding NUL remains valid. */
            if (user_copy_in(args->strings + args->used, address, bytes)) {
                bytes = 1;
                if (user_copy_in(args->strings + args->used, address, bytes)) return -NOS_EFAULT;
            }
        }
        for (size_t i = 0; i < bytes; i++) {
            if (!args->strings[args->used + i]) {
                args->used += (unsigned)i + 1;
                args->offsets[slot] = start;
                if (environment) args->envc++; else args->argc++;
                return 0;
            }
        }
        args->used += (unsigned)bytes;
    }
    return -NOS_E2BIG;
}
static int copy_vector(const char *const *vector, struct nos_exec_args *args, unsigned environment)
{
    if (!vector) return 0;
    for (size_t i = 0; i <= NOS_EXEC_ARGC; i++) {
        uintptr_t pointer;
        if ((uintptr_t)vector > UINTPTR_MAX - i * sizeof(pointer) ||
            user_copy_in(&pointer, (uintptr_t)vector + i * sizeof(pointer), sizeof(pointer))) return -NOS_EFAULT;
        if (!pointer) return 0;
        int rc = append_argument(args, pointer, 0, environment);
        if (rc) return rc;
    }
    return -NOS_E2BIG;
}
#else
static const char *argument(const struct nos_exec_args *args, unsigned i, unsigned environment)
{ return environment ? args->envp[i] : args->argv[i]; }
static int copy_vector(const char *const *vector, char strings[][NOS_EXEC_STRLEN], unsigned *count)
{
    *count = 0;
    if (!vector) return 0;
    for (unsigned i = 0; i <= NOS_EXEC_ARGC; i++) {
        uintptr_t pointer;
        if ((uintptr_t)vector > UINTPTR_MAX - i * sizeof(pointer) ||
            user_copy_in(&pointer, (uintptr_t)vector + i * sizeof(pointer), sizeof(pointer))) return -NOS_EFAULT;
        if (!pointer) return 0;
        if (i == NOS_EXEC_ARGC) return -NOS_EINVAL;
        int rc = copy_string(strings[i], pointer);
        if (rc) return rc;
        (*count)++;
    }
    return -NOS_EINVAL;
}
#endif
int nos_exec_copy_args(const struct nos_exec_request *request, struct nos_exec_args *args)
{
    k_memset(args, 0, sizeof(*args));
    int rc = copy_string(args->path, (uintptr_t)request->path);
    if (rc) return rc;
    if (args->path[0] != '/') return -NOS_EINVAL;
#ifdef NOS_LINUX_COMPAT
    rc = copy_vector(request->argv, args, 0);
    if (rc) return rc;
    if (!args->argc) { rc = append_argument(args, 0, args->path, 0); if (rc) return rc; }
    return copy_vector(request->envp, args, 1);
#else
    rc = copy_vector(request->argv, args->argv, &args->argc);
    if (rc) return rc;
    rc = copy_vector(request->envp, args->envp, &args->envc);
    if (rc) return rc;
    if (!args->argc) { k_memcpy(args->argv[0], args->path, NOS_EXEC_STRLEN); args->argc = 1; }
    return 0;
#endif
}
static size_t string_bytes(const char *s)
{ size_t n = 0; while (s[n]) n++; return n + 1; }
static int valid_arguments(const struct nos_exec_args *args)
{
    if (!args || args->argc > NOS_EXEC_ARGC || args->envc > NOS_EXEC_ARGC) return 0;
#ifdef NOS_LINUX_COMPAT
    if (args->argc > NOS_EXEC_ARGC - args->envc || args->used > NOS_EXEC_BYTES) return 0;
#endif
    return 1;
}
static int stack_bytes(struct task *task, uintptr_t *stack, uintptr_t bottom, const void *data, size_t size)
{
    if (*stack < bottom || size > *stack - bottom) return -NOS_ENOMEM;
    *stack -= size;
    return task_copy_out(task, *stack, data, size);
}
/* Stream the pointer tables into the new address space: ARG_MAX can be much
 * larger than a kernel stack.  Strings stay above the completed table. */
static int stack_arguments(struct task *task, const struct nos_exec_args *args,
                          uintptr_t top, uintptr_t bottom, const uintptr_t *aux,
                          size_t aux_words, struct nos_elf_result *result)
{
    size_t strings = 0;
    for (unsigned environment = 0; environment < 2; environment++) {
        unsigned count = environment ? args->envc : args->argc;
        for (unsigned i = 0; i < count; i++) {
            size_t bytes = string_bytes(argument(args, i, environment));
            if (bytes > SIZE_MAX - strings) return -NOS_E2BIG;
            strings += bytes;
        }
    }
    size_t words = (size_t)args->argc + args->envc + 3 + aux_words;
    if (top < bottom || words > (top - bottom) / sizeof(uintptr_t)) return -NOS_ENOMEM;
    size_t table_bytes = words * sizeof(uintptr_t);
    if (strings > top - bottom - table_bytes) return -NOS_ENOMEM;
    uintptr_t stack = (top - strings - table_bytes) & ~(uintptr_t)15;
    if (stack < bottom) return -NOS_ENOMEM;
    uintptr_t cursor = stack, word = args->argc;
    if (task_copy_out(task, cursor, &word, sizeof(word))) return -NOS_EFAULT;
    cursor += sizeof(word);
    for (unsigned environment = 0; environment < 2; environment++) {
        unsigned count = environment ? args->envc : args->argc;
        for (unsigned i = 0; i < count; i++) {
            const char *string = argument(args, i, environment);
            if (stack_bytes(task, &top, bottom, string, string_bytes(string))) return -NOS_EFAULT;
            if (task_copy_out(task, cursor, &top, sizeof(top))) return -NOS_EFAULT;
            cursor += sizeof(word);
        }
        word = 0;
        if (task_copy_out(task, cursor, &word, sizeof(word))) return -NOS_EFAULT;
        cursor += sizeof(word);
    }
    if (task_copy_out(task, cursor, aux, aux_words * sizeof(uintptr_t))) return -NOS_EFAULT;
    result->stack = stack; result->argv = stack + sizeof(uintptr_t); result->argc = args->argc;
    return 0;
}
static int make_stack(struct task *task, const struct nos_exec_args *args,
                      const unsigned char *image, const struct elf_header *header,
                      const struct elf_segment *tls, uintptr_t phdr, uintptr_t entry,
                      struct nos_elf_result *result)
{
#ifdef NOS_MMU
    uintptr_t top = USER_DATA + USER_BYTES, bottom = USER_DATA + HEAP_END + NOS_PAGE_SIZE;
#else
    uintptr_t top = task->memory + USER_BYTES, bottom = task->memory + NOS_PAGE_SIZE;
#endif
    uintptr_t stack = top;
    struct nos_tls_image tls_info = {0};
    if (tls->memsz) {
        size_t alignment = tls->align > 16 ? (size_t)tls->align : 16;
        if (tls->memsz > NOS_PAGE_SIZE || stack < tls->memsz) return -NOS_ENOMEM;
        stack = (stack - (uintptr_t)tls->memsz) & ~(uintptr_t)(alignment - 1);
        if (stack < bottom) return -NOS_ENOMEM;
        unsigned char zero[64] = {0};
        for (size_t offset = 0; offset < tls->memsz; offset += sizeof(zero)) {
            size_t n = tls->memsz - offset < sizeof(zero) ? tls->memsz - offset : sizeof(zero);
            if (task_copy_out(task, stack + offset, zero, n)) return -NOS_EFAULT;
        }
        if (task_copy_out(task, stack, image + tls->offset, (size_t)tls->filesz)) return -NOS_EFAULT;
        tls_info = (struct nos_tls_image){stack, (size_t)tls->filesz, (size_t)tls->memsz, (size_t)tls->align};
    }
    if (stack_bytes(task, &stack, bottom, &tls_info, sizeof(tls_info))) return -NOS_ENOMEM;
    uintptr_t tls_address = stack;
    const uintptr_t aux[] = {6, NOS_PAGE_SIZE, 9, entry, 3, phdr,
        4, sizeof(struct elf_segment), 5, header->phnum, NOS_AUX_TLS, tls_address, 0, 0};
    *result = (struct nos_elf_result){.entry = entry, .tls = tls_address, .abi = NOS_ABI_NATIVE};
    return stack_arguments(task, args, stack, bottom, aux, sizeof(aux) / sizeof(aux[0]), result);
}
#ifndef NOS_MMU
static int relocate_nommu(struct task *task, uintptr_t base, size_t image_size,
                         const struct elf_segment *dynamic, const struct elf_segment *loads, unsigned count)
{
#ifdef NOS_RISCV32
    uint32_t rel = 0, rel_size = 0, rel_entry = 12;
#else
    uint32_t rel = 0, rel_size = 0, rel_entry = 8;
#endif
    if (!dynamic->memsz) return 0;
    if (!dynamic->filesz || dynamic->filesz % 8 || (dynamic->va & 3) ||
        !segment_range(loads, count, dynamic->va, (size_t)dynamic->filesz, 4, 1)) return -NOS_EINVAL;
    unsigned terminated = 0, seen = 0;
    for (size_t off = 0; off < dynamic->filesz; off += 8) {
        uint32_t pair[2];
        if (task_copy_in(task, pair, base + dynamic->va + off, sizeof(pair))) return -NOS_EFAULT;
        if (!pair[0]) { terminated = 1; break; }
        if (pair[0] == 1 || pair[0] == 22 || pair[0] == 23 ||
            (pair[0] == 30 && (pair[1] & 4))) return -NOS_ENOTSUP;
#ifdef NOS_RISCV32
        unsigned field = pair[0] == 7 ? 1 : pair[0] == 8 ? 2 : pair[0] == 9 ? 4 : 0;
        if (field & seen) return -NOS_EINVAL;
        seen |= field;
        if (field == 1) rel = pair[1];
        if (field == 2) rel_size = pair[1];
        if (field == 4) rel_entry = pair[1];
        if (pair[0] == 17 || pair[0] == 18 || pair[0] == 23) return -NOS_ENOTSUP;
#else
        unsigned field = pair[0] == 17 ? 1 : pair[0] == 18 ? 2 : pair[0] == 19 ? 4 : 0;
        if (field & seen) return -NOS_EINVAL;
        seen |= field;
        if (field == 1) rel = pair[1];
        if (field == 2) rel_size = pair[1];
        if (field == 4) rel_entry = pair[1];
        if (pair[0] == 7 || pair[0] == 8 || pair[0] == 23) return -NOS_ENOTSUP;
#endif
    }
    if (!terminated || (rel_size && ((seen & 3) != 3 || (rel & 3) ||
        !segment_range(loads, count, rel, rel_size, 4, 1)))) return -NOS_EINVAL;
#ifdef NOS_RISCV32
    if (rel_entry != 12 || rel_size % 12 || !extent(rel, rel_size, image_size)) return -NOS_EINVAL;
#else
    if (rel_entry != 8 || rel_size % 8 || !extent(rel, rel_size, image_size)) return -NOS_EINVAL;
#endif
    for (size_t off = 0; off < rel_size; off += rel_entry) {
#ifdef NOS_RISCV32
        uint32_t pair[3], value;
#else
        uint32_t pair[2], value;
#endif
        if (task_copy_in(task, pair, base + rel + off, sizeof(pair))) return -NOS_EFAULT;
#ifdef NOS_RISCV32
        if (pair[1] != 3 || (pair[0] & 3) || !segment_range(loads, count, pair[0], 4, 2, 0)) return -NOS_ENOTSUP;
        value = pair[2];
#else
        if (pair[1] != 23 || (pair[0] & 3) || !segment_range(loads, count, pair[0], 4, 2, 0)) return -NOS_ENOTSUP;
        if (task_copy_in(task, &value, base + pair[0], sizeof(value))) return -NOS_EFAULT;
#endif
        if (value > image_size || value > UINT32_MAX - base) return -NOS_EINVAL;
        value += (uint32_t)base;
        if (task_copy_load(task, base + pair[0], &value, sizeof(value))) return -NOS_EFAULT;
    }
    return 0;
}
#endif
int nos_elf_load(struct task *task, const void *data, size_t bytes,
                 const struct nos_exec_args *args, struct nos_elf_result *result)
{
    struct elf_header header;
    if (!data || !valid_arguments(args) || !result ||
        bytes < sizeof(header)) return -NOS_EINVAL;
    k_memcpy(&header, data, sizeof(header));
    if (header.ident[0] != 0x7f || header.ident[1] != 'E' || header.ident[2] != 'L' || header.ident[3] != 'F' ||
        header.ident[4] != ELF_CLASS || header.ident[5] != 1 || header.ident[6] != 1 ||
        header.machine != ELF_MACHINE || header.version != 1 || header.ehsize != sizeof(header) ||
        header.phentsize != sizeof(struct elf_segment) || !header.phnum || header.phnum > ELF_MAX_PHDR ||
        !extent((uintptr_t)header.phoff, header.phnum * sizeof(struct elf_segment), bytes)) return -NOS_EINVAL;
#ifdef NOS_MMU
    if (header.type != 2) return -NOS_ENOTSUP;
#else
    if (header.type != 3) return -NOS_ENOTSUP;
#endif
    const unsigned char *image = data;
    struct elf_segment loads[ELF_MAX_PHDR], tls = {0};
#ifndef NOS_MMU
    struct elf_segment dynamic = {0};
    unsigned dynamic_seen = 0;
#endif
    unsigned count = 0, executable_entry = 0;
    uintptr_t phdr = 0;
#ifndef NOS_MMU
    uintptr_t maximum = 0;
#endif
    uintptr_t entry = (uintptr_t)header.entry;
#ifdef NOS_ARM_M
    if (!(entry & 1)) return -NOS_EINVAL;
#endif
    for (unsigned i = 0; i < header.phnum; i++) {
        struct elf_segment segment;
        k_memcpy(&segment, image + header.phoff + i * sizeof(segment), sizeof(segment));
        if (!segment.type) continue; /* PT_NULL fields are unspecified. */
        if (!extent((uintptr_t)segment.offset, (size_t)segment.filesz, bytes)) return -NOS_EINVAL;
        if (segment.type == 3) return -NOS_ENOTSUP; /* No runtime interpreter. */
        if (segment.type == 0x6474e551u && (segment.flags & 1)) return -NOS_EACCES;
#ifdef NOS_MMU
        if (segment.type == 2) return -NOS_ENOTSUP;
#endif
        /* Architecture attributes and note headers need not occupy memory. */
        if (segment.type != 1 && segment.type != 7 && segment.type != 2) continue;
        if (!extent((uintptr_t)segment.offset, (size_t)segment.filesz, bytes) || segment.filesz > segment.memsz ||
            segment.va > UINTPTR_MAX - segment.memsz || (segment.align && ((segment.align & (segment.align - 1)) ||
            segment.align > 1024u * 1024u || ((segment.va - segment.offset) & (segment.align - 1))))) return -NOS_EINVAL;
        if (segment.type == 7) {
            if (tls.memsz || segment.memsz > NOS_PAGE_SIZE || segment.align > NOS_PAGE_SIZE) return -NOS_EINVAL;
            tls = segment; continue;
        }
#ifndef NOS_MMU
        if (segment.type == 2) {
            if (dynamic_seen++) return -NOS_EINVAL;
            dynamic = segment; continue;
        }
#endif
        if (segment.type != 1 || !segment.memsz) continue;
        if (!(segment.flags & 4) || segment.flags & ~7u || (segment.flags & 3) == 3 || segment.memsz > ELF_MAX_IMAGE)
            return -NOS_EACCES;
        uintptr_t start = (uintptr_t)segment.va & ~(uintptr_t)(NOS_PAGE_SIZE - 1);
        uintptr_t end = ROUND_PAGE((uintptr_t)segment.va + (uintptr_t)segment.memsz);
        if (!end || end < start) return -NOS_EINVAL;
#ifdef NOS_MMU
        if (!(segment.flags & 2)) {
            if (start < 0x00400000u || end > 0x00500000u) return -NOS_EACCES;
        } else if (start < USER_DATA || end > USER_DATA + HEAP_OFFSET) return -NOS_EACCES;
#else
        if (end > ELF_MAX_IMAGE) return -NOS_ENOMEM;
        if (end > maximum) maximum = end;
#endif
        for (unsigned j = 0; j < count; j++) {
            uintptr_t other_start = (uintptr_t)loads[j].va & ~(uintptr_t)(NOS_PAGE_SIZE - 1);
            uintptr_t other_end = ROUND_PAGE((uintptr_t)loads[j].va + (uintptr_t)loads[j].memsz);
            if (start < other_end && other_start < end) return -NOS_EINVAL;
        }
        loads[count++] = segment;
        uintptr_t pc = entry & ~(uintptr_t)1;
        size_t instruction =
#if defined(NOS_ARM64) || defined(NOS_ARM)
            4;
#else
            2;
#endif
        if ((segment.flags & 1) && pc >= segment.va &&
            extent(pc - segment.va, instruction, (size_t)segment.filesz)) executable_entry = 1;
        if (header.phoff >= segment.offset && extent((uintptr_t)(header.phoff - segment.offset),
            header.phnum * sizeof(struct elf_segment), (size_t)segment.filesz))
            phdr = (uintptr_t)segment.va + (uintptr_t)(header.phoff - segment.offset);
    }
    if (!count || !executable_entry || !phdr) return -NOS_EINVAL;
#if defined(NOS_ARM64) || defined(NOS_ARM)
    if (entry & 3u) return -NOS_EINVAL;
#elif defined(NOS_RISCV)
    if (entry & 1u) return -NOS_EINVAL;
#endif
    if (tls.memsz) {
        unsigned contained = 0;
        for (unsigned i = 0; i < count; i++) {
            struct elf_segment *segment = &loads[i];
            if (tls.va >= segment->va && extent((uintptr_t)(tls.va - segment->va),
                (size_t)tls.memsz, (size_t)segment->memsz) && tls.offset >= segment->offset &&
                extent((uintptr_t)(tls.offset - segment->offset), (size_t)tls.filesz, (size_t)segment->filesz) &&
                tls.va - segment->va == tls.offset - segment->offset) contained = 1;
        }
        if (!contained) return -NOS_EINVAL;
    }
#ifndef NOS_MMU
    if (dynamic.memsz) {
        unsigned contained = 0;
        for (unsigned i = 0; i < count; i++) {
            const struct elf_segment *segment = &loads[i];
            if (dynamic.va >= segment->va && dynamic.offset >= segment->offset &&
                dynamic.va - segment->va == dynamic.offset - segment->offset &&
                extent(dynamic.va - segment->va, (size_t)dynamic.memsz, (size_t)segment->memsz) &&
                extent(dynamic.offset - segment->offset, (size_t)dynamic.filesz, (size_t)segment->filesz)) contained = 1;
        }
        if (!contained) return -NOS_EINVAL;
    }
#endif
#ifndef NOS_MMU
    intptr_t allocation = vm_map(task, NOS_PROT_READ | NOS_PROT_EXEC, maximum);
    if (pointer_error(allocation)) return (int)allocation;
    uintptr_t bias = (uintptr_t)allocation;
    entry += bias; phdr += bias;
#else
    uintptr_t bias = 0;
#endif
    for (unsigned i = 0; i < count; i++) {
        struct elf_segment *segment = &loads[i];
        uintptr_t address = bias + (uintptr_t)segment->va;
#ifdef NOS_MMU
        if (!(segment->flags & 2)) {
            uintptr_t start = address & ~(uintptr_t)(NOS_PAGE_SIZE - 1);
            unsigned prot = NOS_PROT_READ | ((segment->flags & 1) ? NOS_PROT_EXEC : 0);
            intptr_t rc = vm_map_at(task, start, prot, ROUND_PAGE(address + (uintptr_t)segment->memsz) - start);
            if (pointer_error(rc)) return (int)rc;
        }
#endif
        if (task_copy_load(task, address, image + segment->offset, (size_t)segment->filesz)) return -NOS_EFAULT;
    }
#ifndef NOS_MMU
    int rc = relocate_nommu(task, bias, maximum, &dynamic, loads, count);
    if (rc) return rc;
    for (unsigned i = 0; i < count; i++) {
        struct elf_segment *segment = &loads[i];
        uintptr_t start = (uintptr_t)segment->va & ~(uintptr_t)(NOS_PAGE_SIZE - 1);
        uintptr_t end = ROUND_PAGE((uintptr_t)segment->va + (uintptr_t)segment->memsz);
        unsigned prot = NOS_PROT_READ | ((segment->flags & 1) ? NOS_PROT_EXEC : 0) |
                        ((segment->flags & 2) ? NOS_PROT_WRITE : 0);
        rc = (int)vm_load_protect(task, bias + start, prot, end - start);
        if (rc) return rc;
    }
#endif
    return make_stack(task, args, image, &header, &tls, phdr, entry, result);
}

/* Linux is an explicit execution personality.  Its interpreter performs all
 * dynamic relocation in user mode; the native static contract above is kept. */
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
#define LINUX_MAIN_BASE 0x06000000u
#define LINUX_INTERP_BASE 0x06800000u
#define LINUX_STACK_TOP 0x07800000u
#define LINUX_STACK_BYTES (256u * 1024u)
#define LINUX_PHDRS 32u
struct linux_image {
    struct elf_header header;
    struct elf_segment loads[LINUX_PHDRS];
    unsigned count;
    uintptr_t bias, entry, phdr;
    char interpreter[NOS_EXEC_STRLEN];
};
static int linux_parse_image(const void *data, size_t bytes, uintptr_t bias, unsigned role,
                              struct linux_image *parsed)
{
    if (!data || bytes < sizeof(struct elf_header)) return -NOS_EINVAL;
    k_memset(parsed, 0, sizeof(*parsed));
    struct elf_header *h = &parsed->header;
    k_memcpy(h, data, sizeof(*h));
    if (h->ident[0] != 127 || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F' ||
        h->ident[4] != ELF_CLASS || h->ident[5] != 1 || h->ident[6] != 1 || h->version != 1 ||
        h->machine != ELF_MACHINE || h->ehsize != sizeof(*h) || h->phentsize != sizeof(struct elf_segment) ||
        !h->phnum || h->phnum > LINUX_PHDRS ||
        !extent(h->phoff, h->phnum * sizeof(struct elf_segment), bytes)) return -NOS_EINVAL;
    if ((h->type != 2 && h->type != 3) || (role == 1 && h->type != 3) || (role == 2 && h->type != 2))
        return -NOS_ENOTSUP;
    parsed->bias = h->type == 3 ? bias : 0;
    if (h->entry > UINTPTR_MAX - parsed->bias) return -NOS_EINVAL;
    parsed->entry = parsed->bias + h->entry;
    const unsigned char *image = data;
    unsigned entry_ok = 0;
    for (unsigned i = 0; i < h->phnum; i++) {
        struct elf_segment segment;
        k_memcpy(&segment, image + h->phoff + i * sizeof(segment), sizeof(segment));
        if (!segment.type) continue;
        if (!extent(segment.offset, segment.filesz, bytes)) return -NOS_EINVAL;
        if (segment.type == 3) {
            if (role || parsed->interpreter[0] || segment.filesz < 2 || segment.filesz > NOS_EXEC_STRLEN ||
                image[segment.offset] != '/' || image[segment.offset + segment.filesz - 1]) return -NOS_EINVAL;
            for (size_t j = 0; j + 1 < segment.filesz; j++) if (!image[segment.offset + j]) return -NOS_EINVAL;
            k_memcpy(parsed->interpreter, image + segment.offset, (size_t)segment.filesz);
        }
        if (segment.type == 0x6474e551u && (segment.flags & 1)) return -NOS_EACCES;
        if (segment.type != 1) continue;
        if (!segment.memsz) { if (segment.filesz) return -NOS_EINVAL; continue; }
        if (!(segment.flags & 4) || (segment.flags & ~7u) || (segment.flags & 3) == 3 ||
            segment.filesz > segment.memsz || segment.memsz > 16u * 1024u * 1024u ||
            segment.va > UINTPTR_MAX - segment.memsz ||
            (segment.align && ((segment.align & (segment.align - 1)) || segment.align > 2u * 1024u * 1024u ||
                ((segment.va - segment.offset) & (segment.align - 1))))) return -NOS_EINVAL;
        if (segment.va > UINTPTR_MAX - parsed->bias ||
            segment.memsz > UINTPTR_MAX - (segment.va + parsed->bias)) return -NOS_EINVAL;
        uintptr_t address = parsed->bias + segment.va;
        uintptr_t end = address + segment.memsz;
        if (end > UINTPTR_MAX - (NOS_PAGE_SIZE - 1)) return -NOS_EINVAL;
        if (role == 2) {
            if ((segment.flags & 2) || address < NOS_LINUX_GATEWAY_BASE || end > NOS_LINUX_GATEWAY_END)
                return -NOS_EACCES;
        } else if (address < 0x00400000u || end > 0x10000000u) return -NOS_EACCES;
        for (unsigned j = 0; j < parsed->count; j++) {
            const struct elf_segment *other = &parsed->loads[j];
            if (segment.va < other->va + other->memsz && other->va < segment.va + segment.memsz)
                return -NOS_EINVAL;
        }
        parsed->loads[parsed->count++] = segment;
        uintptr_t pc = h->entry;
#if defined(NOS_ARM)
        pc &= ~(uintptr_t)1;
        size_t instruction = (h->entry & 1) ? 2 : 4;
#elif defined(NOS_ARM64)
        size_t instruction = 4;
#else
        size_t instruction = 2;
#endif
        if ((segment.flags & 1) && pc >= segment.va && extent(pc - segment.va, instruction, segment.filesz)) entry_ok = 1;
        if (h->phoff >= segment.offset && extent(h->phoff - segment.offset,
            h->phnum * sizeof(struct elf_segment), segment.filesz))
            parsed->phdr = address + h->phoff - segment.offset;
    }
#if defined(NOS_ARM64)
    if (h->entry & 3) return -NOS_EINVAL;
#elif defined(NOS_ARM)
    if (!(h->entry & 1) && (h->entry & 3)) return -NOS_EINVAL;
#else
    if (h->entry & 1) return -NOS_EINVAL;
#endif
    return parsed->count && entry_ok && (parsed->phdr || role == 2) ? 0 : -NOS_EINVAL;
}
static int linux_map_image(struct task *task, const void *data, const struct linux_image *parsed, unsigned gateway)
{
    uintptr_t boundaries[LINUX_PHDRS * 2]; unsigned n = 0;
    for (unsigned i = 0; i < parsed->count; i++) {
        const struct elf_segment *s = &parsed->loads[i];
        uintptr_t start = (parsed->bias + s->va) & ~(uintptr_t)(NOS_PAGE_SIZE - 1);
        uintptr_t end = ROUND_PAGE(parsed->bias + s->va + s->memsz);
        boundaries[n++] = start; boundaries[n++] = end;
    }
    for (unsigned i = 1; i < n; i++) {
        uintptr_t value = boundaries[i]; unsigned j = i;
        while (j && boundaries[j - 1] > value) { boundaries[j] = boundaries[j - 1]; j--; }
        boundaries[j] = value;
    }
    for (unsigned i = 0; i + 1 < n; i++) {
        uintptr_t start = boundaries[i], end = boundaries[i + 1]; unsigned prot = 0;
        if (start == end) continue;
        for (unsigned j = 0; j < parsed->count; j++) {
            const struct elf_segment *s = &parsed->loads[j];
            uintptr_t low = (parsed->bias + s->va) & ~(uintptr_t)(NOS_PAGE_SIZE - 1);
            uintptr_t high = ROUND_PAGE(parsed->bias + s->va + s->memsz);
            if (start >= low && end <= high) prot |= NOS_PROT_READ |
                ((s->flags & 2) ? NOS_PROT_WRITE : 0) | ((s->flags & 1) ? NOS_PROT_EXEC : 0);
        }
        if (!prot) continue;
        if ((prot & 6) == 6) return -NOS_EACCES;
        int rc = gateway ? vm_gateway_map(task, start, end - start, prot) : vm_image_map(task, start, end - start, prot);
        if (rc) return rc;
    }
    for (unsigned i = 0; i < parsed->count; i++) {
        const struct elf_segment *s = &parsed->loads[i];
        int rc = task_copy_load(task, parsed->bias + s->va, (const unsigned char *)data + s->offset, s->filesz);
        if (rc) return rc;
    }
    return 0;
}
static int linux_stack(struct task *task, const struct nos_exec_args *args, uintptr_t entry, uintptr_t phdr,
                        unsigned phnum, uintptr_t interpreter, const struct nos_credentials *credentials,
                        struct nos_elf_result *result)
{
    uintptr_t bottom = LINUX_STACK_TOP - LINUX_STACK_BYTES, stack = LINUX_STACK_TOP;
    int rc = vm_image_map(task, bottom, LINUX_STACK_BYTES, NOS_PROT_READ | NOS_PROT_WRITE);
    if (rc) return rc;
    if (stack_bytes(task, &stack, bottom, args->random, sizeof(args->random))) return -NOS_EFAULT;
    uintptr_t random_address = stack;
    if (stack_bytes(task, &stack, bottom, args->path, string_bytes(args->path))) return -NOS_ENOMEM;
    uintptr_t execfn = stack;
    const uintptr_t aux[] = {3, phdr, 4, sizeof(struct elf_segment), 5, phnum,
        6, NOS_PAGE_SIZE, 7, interpreter, 8, 0, 9, entry,
        11, credentials->ruid, 12, credentials->euid, 13, credentials->rgid, 14, credentials->egid,
        17, 100, 23, credentials->ruid != credentials->euid || credentials->rgid != credentials->egid,
        25, random_address, 31, execfn, 0, 0};
    return stack_arguments(task, args, stack, bottom, aux, sizeof(aux) / sizeof(aux[0]), result);
}
int nos_elf_load_linux(struct task *task, const void *image, size_t bytes,
                      const struct nos_exec_args *args, const struct nos_credentials *credentials,
                      struct nos_elf_result *result)
{
    if (!valid_arguments(args) || !result || !credentials || !args->random_valid)
        return -NOS_EAGAIN;
    struct linux_image parsed;
    int rc = linux_parse_image(image, bytes, LINUX_MAIN_BASE, 0, &parsed);
    if (rc) return rc;
    rc = linux_map_image(task, image, &parsed, 0); if (rc) return rc;
    uintptr_t main_entry = parsed.entry, phdr = parsed.phdr;
    unsigned phnum = parsed.header.phnum;
    char interpreter[NOS_EXEC_STRLEN]; k_memcpy(interpreter, parsed.interpreter, sizeof(interpreter));
    k_memset(result, 0, sizeof(*result)); result->entry = main_entry; result->abi = NOS_ABI_LINUX;
    uintptr_t interpreter_base = 0;
    if (interpreter[0]) {
        size_t interpreter_bytes; const void *loader = nos_initramfs_find(interpreter, &interpreter_bytes);
        if (!loader) return -NOS_ENOENT;
        rc = linux_parse_image(loader, interpreter_bytes, LINUX_INTERP_BASE, 1, &parsed); if (rc) return rc;
        rc = linux_map_image(task, loader, &parsed, 0); if (rc) return rc;
        result->entry = parsed.entry; interpreter_base = parsed.bias;
    }
    size_t gateway_bytes; const void *gateway = nos_initramfs_find("/lib/nos-linux-gateway.elf", &gateway_bytes);
    if (!gateway) return -NOS_ENOENT;
    rc = linux_parse_image(gateway, gateway_bytes, 0, 2, &parsed); if (rc) return rc;
    rc = linux_map_image(task, gateway, &parsed, 1); if (rc) return rc;
    result->gateway_entry = parsed.entry;
    return linux_stack(task, args, main_entry, phdr, phnum, interpreter_base, credentials, result);
}
#else
int nos_elf_load_linux(struct task *task, const void *image, size_t bytes,
                      const struct nos_exec_args *args, const struct nos_credentials *credentials,
                      struct nos_elf_result *result)
{ (void)task; (void)image; (void)bytes; (void)args; (void)credentials; (void)result; return -NOS_ENOTSUP; }
#endif
