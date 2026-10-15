/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "init.h"
static unsigned initialized = 0x51a7;
static unsigned zeroed;
static const char text[] = "NOS ELF APP PASS argc/argv env/auxv data/bss syscall\n";
static const char *volatile relocated_message = text;
#ifdef NOS_MMU
static const uint32_t ro_return =
#ifdef NOS_ARM64
    0xd65f03c0u;
#elif defined(NOS_RISCV)
    0x00008067u;
#else
    0xe12fff1eu;
#endif
#endif
int main(int argc, char **argv)
{
    if (argc < 1 || !argv || !argv[0] || !argv[0][0] || argv[argc] || initialized != 0x51a7 || zeroed)
        return 99;
    zeroed = 1;
#ifdef NOS_MMU
    if (argc > 1 && argv[1][0] == 'n' && argv[1][1] == 'x') {
        ((void (*)(void))(uintptr_t)&ro_return)();
        return 95; /* A mapped executable data page would reach here. */
    }
    if (nos_syscall(NOS_SYS_USER_CHECK, (uintptr_t)text, sizeof(text), NOS_PROT_READ) ||
        nos_syscall(NOS_SYS_USER_CHECK, (uintptr_t)text, sizeof(text), NOS_PROT_WRITE) != -NOS_EFAULT) return 94;
#endif
    char **environment = argv + argc + 1;
    if (!*environment) return 97;
    while (*environment) environment++;
    uintptr_t *aux = (uintptr_t *)(environment + 1);
    unsigned found = 0;
    for (unsigned i = 0; i < 16 && aux[0]; i++, aux += 2) {
        if (aux[0] == 6 && aux[1] == NOS_PAGE_SIZE) found |= 1;
        if (aux[0] == 3 && aux[1]) found |= 2;
        if (aux[0] == 4 && aux[1] == (sizeof(uintptr_t) == 8 ? 56u : 32u)) found |= 4;
        if (aux[0] == 5 && aux[1]) found |= 8;
        if (aux[0] == 9 && aux[1]) found |= 16;
        if (aux[0] == NOS_AUX_TLS && aux[1]) found |= 32;
    }
    if (found != 63) return 96;
    if (nos_syscall(NOS_SYS_WRITE, (uintptr_t)relocated_message, sizeof(text) - 1, 0) != (intptr_t)(sizeof(text) - 1))
        return 98;
    return 23;
}
