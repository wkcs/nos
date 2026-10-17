/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096u
static const char sem_name[] = "/nos-shared-sem";
static const char shm_name[] = "/nos-shared-memory";
static int failed(const char *name)
{ fprintf(stderr, "FAIL glibc_shared_sync %s errno=%d\n", name, errno); return 93; }
static int child_status(pid_t child)
{
    int status;
    if (waitpid(child, &status, 0) != child) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
static int timed_wait(sem_t *semaphore)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline)) return -1;
    deadline.tv_sec += 3;
    return sem_timedwait(semaphore, &deadline);
}
static int file_coherence(void)
{
    int fd = open("/shared-file", O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, PAGE)) return failed("file_create");
    unsigned char *write_map = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int read_fd = open("/shared-file", O_RDONLY);
    if (write_map == MAP_FAILED || read_fd < 0) return failed("file_map_rw");
    unsigned char *read_map = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, read_fd, 0);
    if (read_map == MAP_FAILED || read_map == write_map) return failed("file_map_ro");
    if (pwrite(fd, "file", 4, 64) != 4 || memcmp(write_map + 64, "file", 4) || memcmp(read_map + 64, "file", 4))
        return failed("pwrite_mapping_coherence");
    memcpy(write_map + 128, "maps", 4);
    char data[4];
    if (pread(read_fd, data, 4, 128) != 4 || memcmp(data, "maps", 4) || memcmp(read_map + 128, "maps", 4))
        return failed("mapping_pread_coherence");
    if (msync(write_map, PAGE, MS_SYNC)) return failed("file_msync");
    errno = 0;
    if (mprotect(read_map, PAGE, PROT_READ | PROT_WRITE) != -1 || errno != EACCES)
        return failed("readonly_mprotect");
    errno = 0;
    void *forbidden = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, read_fd, 0);
    if (forbidden != MAP_FAILED || errno != EACCES) return failed("readonly_map_write");
    if (close(read_fd) || close(fd) || unlink("/shared-file") || memcmp(read_map + 128, "maps", 4))
        return failed("file_unlink_mapping_lifetime");
    fd = open("/shared-file", O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, PAGE)) return failed("file_recreate");
    unsigned char *fresh = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fresh == MAP_FAILED || fresh[128] || memcmp(read_map + 128, "maps", 4)) return failed("file_recreate_identity");
    if (munmap(fresh, PAGE) || munmap(read_map, PAGE) || munmap(write_map, PAGE) || close(fd) || unlink("/shared-file"))
        return failed("file_cleanup");
    puts("PASS glibc_file_shared_coherence_permissions");
    return 0;
}
struct shared_page { sem_t start, reply; unsigned value; };
static int shared_memory(void)
{
    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, PAGE)) return failed("shm_create");
    struct shared_page *page = mmap((void *)0x30000000u, PAGE, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    if (page == MAP_FAILED || (uintptr_t)page != 0x30000000u || sem_init(&page->start, 1, 0) || sem_init(&page->reply, 1, 0))
        return failed("shared_sem_init");
    int ready[2];
    if (pipe(ready)) return failed("shared_sem_pipe");
    pid_t child = fork();
    if (child < 0) return failed("shared_sem_fork");
    if (!child) {
        close(ready[0]);
        if (munmap(page, PAGE)) _exit(1);
        struct shared_page *other = mmap((void *)0x31000000u, PAGE, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (other == MAP_FAILED || (uintptr_t)other != 0x31000000u || close(fd)) _exit(2);
        if (write(ready[1], "r", 1) != 1 || timed_wait(&other->start) || other->value != 7) _exit(3);
        other->value = 42;
        if (sem_post(&other->reply) || munmap(other, PAGE)) _exit(4);
        close(ready[1]); _exit(0);
    }
    close(ready[1]);
    char byte;
    if (read(ready[0], &byte, 1) != 1 || byte != 'r') return failed("shared_sem_child_ready");
    page->value = 7;
    if (sem_post(&page->start) || timed_wait(&page->reply) || page->value != 42 || child_status(child))
        return failed("shared_sem_different_va_wait_wake");
    close(ready[0]);
    if (shm_unlink(shm_name) || close(fd)) return failed("shm_unlink_live");
    fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, PAGE)) return failed("shm_recreate");
    struct shared_page *fresh = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fresh == MAP_FAILED || fresh->value || page->value != 42) return failed("shm_unlink_new_identity");
    fresh->value = 99;
    if (page->value != 42 || sem_destroy(&page->start) || sem_destroy(&page->reply) ||
        munmap(page, PAGE) || munmap(fresh, PAGE) || close(fd) || shm_unlink(shm_name)) return failed("shm_cleanup");
    puts("PASS glibc_shm_process_shared_sem_different_va");
    return 0;
}
static int named_child(int argc, char **argv)
{
    if (argc != 4) return failed("named_child_arguments");
    uintptr_t old_address = (uintptr_t)strtoull(argv[2], NULL, 16);
    int pipe_fd = atoi(argv[3]);
    void *reservation = mmap((void *)(old_address & ~(uintptr_t)(PAGE - 1)), PAGE, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (reservation == MAP_FAILED && errno != EEXIST) return failed("named_child_reserve");
    sem_t *semaphore = sem_open(sem_name, 0);
    uintptr_t address = (uintptr_t)semaphore;
    if (semaphore == SEM_FAILED || address == old_address || write(pipe_fd, &address, sizeof(address)) != sizeof(address))
        return failed("named_child_open_different_va");
    if (timed_wait(semaphore) || write(pipe_fd, "s", 1) != 1 || sem_close(semaphore)) return failed("named_child_wait");
    if (reservation != MAP_FAILED) munmap(reservation, PAGE);
    close(pipe_fd); return 0;
}
static int named_semaphore(const char *program)
{
    sem_t *old = sem_open(sem_name, O_CREAT | O_EXCL, 0600, 0);
    if (old == SEM_FAILED) return failed("named_sem_create");
    int ready[2];
    if (pipe(ready)) return failed("named_sem_pipe");
    char address[32], fd_number[16];
    snprintf(address, sizeof(address), "%lx", (unsigned long)(uintptr_t)old);
    snprintf(fd_number, sizeof(fd_number), "%d", ready[1]);
    pid_t child = fork();
    if (child < 0) return failed("named_sem_fork");
    if (!child) {
        close(ready[0]);
        execl(program, program, "sem-child", address, fd_number, (char *)NULL);
        _exit(5);
    }
    close(ready[1]);
    uintptr_t child_address;
    if (read(ready[0], &child_address, sizeof(child_address)) != sizeof(child_address) || child_address == (uintptr_t)old)
        return failed("named_sem_exec_address");
    if (sem_post(old)) return failed("named_sem_post");
    char byte;
    if (read(ready[0], &byte, 1) != 1 || byte != 's' || child_status(child) || close(ready[0]))
        return failed("named_sem_exec_wait");
    if (sem_unlink(sem_name) || sem_post(old) || sem_wait(old)) return failed("named_unlink_live");
    sem_t *fresh = sem_open(sem_name, O_CREAT | O_EXCL, 0600, 1);
    if (fresh == SEM_FAILED || fresh == old || sem_trywait(fresh) || sem_post(old)) return failed("named_recreate_identity");
    int old_value = -1, fresh_value = -1;
    if (sem_getvalue(old, &old_value) || old_value != 1 || sem_getvalue(fresh, &fresh_value) || fresh_value)
        return failed("named_recreate_independent_count");
    if (sem_wait(old) || sem_close(old) || sem_close(fresh) || sem_unlink(sem_name)) return failed("named_cleanup");
    puts("PASS glibc_named_sem_exec_unlink_recreate");
    return 0;
}
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "sem-child")) return named_child(argc, argv);
    int result = file_coherence();
    if (!result) result = shared_memory();
    if (!result) result = named_semaphore(argv[0]);
    return result;
}
