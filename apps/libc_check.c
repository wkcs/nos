/* Copyright (C) 2026 NOS contributors */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#if defined(NOS_MUSL)
static _Thread_local unsigned initialized_tls = 0x7165;
static _Thread_local unsigned zero_tls;
#endif
int main(int argc, char **argv)
{
    if (argc != 2 || !argv[1] || strcmp(argv[1], "qemu")) return 61;
#if defined(NOS_MUSL)
    if (initialized_tls != 0x7165 || zero_tls) return 62;
    initialized_tls++; zero_tls = 1;
#endif
    unsigned char *allocation = malloc(8192);
    if (!allocation) return 63;
    memset(allocation, 0xa5, 8192);
    if (allocation[8191] != 0xa5) return 64;
    free(allocation);
    errno = 0;
    FILE *missing = fopen("/missing-libc-app", "r");
    if (missing || errno != ENOENT) return 65;
    FILE *file = fopen("/libc-app", "w+");
    if (!file || fprintf(file, "ELF libc %d\n", argc) <= 0 || fflush(file) || fseek(file, 0, SEEK_SET)) return 66;
    char buffer[64];
    if (!fgets(buffer, sizeof(buffer), file) || strcmp(buffer, "ELF libc 2\n") || fclose(file)) return 67;
    printf("NOS ELF LIBC APP PASS malloc stdio errno%s\n",
#if defined(NOS_MUSL)
           " PT_TLS _Thread_local"
#else
           " Newlib reentrancy"
#endif
    );
    return 0;
}
