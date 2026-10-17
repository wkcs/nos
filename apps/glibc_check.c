/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/auxv.h>
#include <stdint.h>
#include <sys/mman.h>
static _Thread_local unsigned initialized_tls = 0x7152;
static _Thread_local unsigned zero_tls;
#define EXEC_BUDGET 131072u
#define LONG_ENV_BYTES 16384u
static char long_argument[EXEC_BUDGET];
static char long_environment[13 + LONG_ENV_BYTES + 1];
static char *many_arguments[2051];
static int replace_with_guarded_argument(void)
{
    char *memory = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED || mprotect(memory + 4096, 4096, PROT_NONE)) return 89;
    char old_pid[40]; pid_t original = getpid();
    snprintf(old_pid, sizeof(old_pid), "NOS_OLD_PID=%ld", (long)original);
    char *environment[] = {"LD_LIBRARY_PATH=/lib", old_pid, NULL};
    char *arguments[] = {"/bin/glibc_check", "guard-exec", memory + 4093, NULL};
    memcpy(memory + 4093, "bad", 3);
    errno = 0;
    if (execve(arguments[0], arguments, environment) != -1 || errno != EFAULT ||
        getpid() != original || initialized_tls != 0x7152 || zero_tls) return 90;
    memcpy(memory + 4093, "ok", 3);
    if (fflush(NULL)) return 91;
    execve(arguments[0], arguments, environment);
    perror("glibc_check execve guarded argument"); return 92;
}
static int check_exec_arguments(int argc, char **argv)
{
    const char *environment = getenv("NOS_LONG_ENV"), *old_pid = getenv("NOS_OLD_PID");
    if (argc != 3 || !environment || !old_pid || strlen(environment) != LONG_ENV_BYTES ||
        getpid() != (pid_t)strtol(old_pid, NULL, 10) || strlen(argv[2]) < 2000) return 75;
    size_t total = 8 * sizeof(uintptr_t); /* argc=3, envc=3, plus their NULL pointers. */
    for (int i = 0; i < argc; i++) total += strlen(argv[i]) + 1;
    extern char **environ;
    unsigned count = 0;
    for (char **p = environ; *p; p++) { total += strlen(*p) + 1; count++; }
    if (count != 3 || total != EXEC_BUDGET || initialized_tls != 0x7152 || zero_tls) return 76;
    for (const char *p = argv[2]; *p; p++) if (*p != 'q') return 77;
    for (const char *p = environment; *p; p++) if (*p != 'e') return 78;
    puts("PASS glibc_exec_argument_budget");
    return 0;
}
static int replace_with_many_arguments(void)
{
    char old_pid[40];
    snprintf(old_pid, sizeof(old_pid), "NOS_OLD_PID=%ld", (long)getpid());
    char *environment[] = {"LD_LIBRARY_PATH=/lib", old_pid, NULL};
    many_arguments[0] = "/bin/glibc_check"; many_arguments[1] = "many-exec";
    for (unsigned i = 2; i < 2050; i++) many_arguments[i] = "x";
    if (fflush(NULL)) return 85;
    execve(many_arguments[0], many_arguments, environment);
    perror("glibc_check execve many arguments"); return 86;
}
static int replace_with_long_arguments(void)
{
    char *program = "/bin/glibc_check";
    char old_pid[40]; pid_t original = getpid();
    snprintf(old_pid, sizeof(old_pid), "NOS_OLD_PID=%ld", (long)original);
    memcpy(long_environment, "NOS_LONG_ENV=", 13);
    memset(long_environment + 13, 'e', LONG_ENV_BYTES); long_environment[13 + LONG_ENV_BYTES] = 0;
    char *environment[] = {"LD_LIBRARY_PATH=/lib", long_environment, old_pid, NULL};
    volatile uintptr_t invalid_vector = 1;
    errno = 0;
    if (execve(program, (char *const *)invalid_vector, environment) != -1 || errno != EFAULT ||
        getpid() != original || initialized_tls != 0x7153 || zero_tls != 1) return 79;
    size_t fixed = 8 * sizeof(uintptr_t) + strlen(program) + 1 + sizeof("long-exec");
    for (char **p = environment; *p; p++) fixed += strlen(*p) + 1;
    if (fixed >= sizeof(long_argument)) return 81;
    size_t length = sizeof(long_argument) - fixed - 1;
    memset(long_argument, 'q', length + 1); long_argument[length + 1] = 0;
    char *arguments[] = {program, "long-exec", long_argument, NULL};
    errno = 0;
    if (execve(program, arguments, environment) != -1 || errno != E2BIG ||
        getpid() != original || initialized_tls != 0x7153 || zero_tls != 1) return 80;
    long_argument[length] = 0;
    if (fflush(NULL)) return 82;
    execve(program, arguments, environment);
    perror("glibc_check execve long arguments");
    return 83;
}
int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "long-exec")) {
        int rc = check_exec_arguments(argc, argv);
        return rc ? rc : replace_with_many_arguments();
    }
    if (argc >= 2 && !strcmp(argv[1], "many-exec")) {
        const char *old_pid = getenv("NOS_OLD_PID");
        if (argc != 2050 || !old_pid || getpid() != (pid_t)strtol(old_pid, NULL, 10)) return 87;
        for (int i = 2; i < argc; i++) if (strcmp(argv[i], "x")) return 88;
        puts("PASS glibc_exec_many_short_arguments"); return replace_with_guarded_argument();
    }
    if (argc >= 2 && !strcmp(argv[1], "guard-exec")) {
        const char *old_pid = getenv("NOS_OLD_PID");
        if (argc != 3 || strcmp(argv[2], "ok") || !old_pid ||
            getpid() != (pid_t)strtol(old_pid, NULL, 10)) return 93;
        puts("PASS glibc_exec_guard_terminated_argument"); return 0;
    }
    if (sysconf(_SC_ARG_MAX) != EXEC_BUDGET) return 84;
    if (argc != 2 || strcmp(argv[1], "qemu") || !getenv("LD_LIBRARY_PATH") || !getauxval(AT_BASE) ||
        !getauxval(AT_PHDR) || !getauxval(AT_RANDOM)) return 61;
    if (initialized_tls != 0x7152 || zero_tls) return 62;
    initialized_tls++; zero_tls = 1;
    unsigned char *memory = malloc(16384);
    if (!memory) return 63;
    memset(memory, 0xa5, 16384);
    if (memory[16383] != 0xa5) return 64;
    free(memory);
    errno = 0;
    FILE *missing = fopen("/glibc-missing", "r");
    if (missing || errno != ENOENT) return 65;
    FILE *file = fopen("/glibc-file", "w+");
    if (!file) { perror("glibc_check fopen"); return 66; }
    if (fprintf(file, "glibc %d %.2f\n", argc, 3.25) <= 0) { perror("glibc_check fprintf"); return 72; }
    if (fflush(file)) { perror("glibc_check fflush"); return 73; }
    if (fseek(file, 0, SEEK_SET)) { perror("glibc_check fseek"); return 74; }
    char buffer[64];
    if (!fgets(buffer, sizeof(buffer), file) || strcmp(buffer, "glibc 2 3.25\n") || fclose(file)) return 67;
    if (unlink("/glibc-file")) { perror("glibc_check unlink"); return 94; }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000) return 68;
    if (clock_gettime(CLOCK_REALTIME, &now) || now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000) return 71;
    printf("NOS REALTIME %lld\n", (long long)now.tv_sec);
    void *library = dlopen("libnos_probe.so", RTLD_NOW);
    if (!library) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 69; }
    int (*add)(int) = (int (*)(int))dlsym(library, "nos_dynamic_add");
    int *initialized = (int *)dlsym(library, "nos_dynamic_initialized");
    if (!add || !initialized || *initialized != 1 || add(13) != 42 || dlclose(library)) return 70;
    puts("PASS glibc_real_stdio_malloc_errno_tls_dlopen");
    return replace_with_long_arguments();
}
