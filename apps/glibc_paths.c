/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#define PATH_BYTES 256u
#define PAGE_BYTES 4096u
static int failed(const char *step)
{ fprintf(stderr, "FAIL glibc_paths %s errno=%d\n", step, errno); return 94; }
static void make_path(char path[PATH_BYTES], char letter)
{ path[0] = '/'; memset(path + 1, letter, PATH_BYTES - 2); path[PATH_BYTES - 1] = 0; }
static int raw_long_entry(const char *name)
{
    int fd = open("/", O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    unsigned char bytes[280];
    int found = 0;
    for (;;) {
        long count = syscall(SYS_getdents64, fd, bytes, sizeof(bytes));
        if (count < 0) { close(fd); return -1; }
        if (!count) break;
        for (size_t offset = 0; offset < (size_t)count;) {
            uint16_t length;
            if ((size_t)count - offset < 24) { close(fd); return -1; }
            memcpy(&length, bytes + offset + 16, 2);
            if (length < 24 || length > (size_t)count - offset) { close(fd); return -1; }
            if (!strcmp((char *)bytes + offset + 19, name)) found += length == 280;
            offset += length;
        }
    }
    return close(fd) ? -1 : found;
}
int main(void)
{
    char first[PATH_BYTES], second[PATH_BYTES], alias[PATH_BYTES], directory[PATH_BYTES];
    make_path(first, 'a'); make_path(second, 'b'); make_path(alias, 'c'); make_path(directory, 'd');
    int fd = open(first, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || write(fd, "x", 1) != 1 || close(fd)) return failed("long_open");
    char *pages = mmap(NULL, 2 * PAGE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED || mprotect(pages + PAGE_BYTES, PAGE_BYTES, PROT_NONE)) return failed("guard_setup");
    char *boundary = pages + PAGE_BYTES - PATH_BYTES;
    memcpy(boundary, first, PATH_BYTES);
    fd = open(boundary, O_RDONLY);
    if (fd < 0 || close(fd)) return failed("nul_at_guard_boundary");
    memset(pages, 'q', PAGE_BYTES); errno = 0;
    if (open(pages, O_RDONLY) != -1 || errno != ENAMETOOLONG) return failed("unterminated_path_bound");
    errno = 0;
    if (open(boundary, O_RDONLY) != -1 || errno != EFAULT) return failed("short_unterminated_path_guard");
    errno = 0;
    if (open(boundary + 1, O_RDONLY) != -1 || errno != EFAULT) return failed("path_guard_fault");
    if (rename(first, second) || link(second, alias)) return failed("two_maximum_paths");
    struct stat source, linked;
    if (stat(second, &source) || stat(alias, &linked) || source.st_ino != linked.st_ino || source.st_nlink != 2)
        return failed("long_stat_link_identity");
    if (symlink(second, "/path-link")) return failed("long_link_target");
    char target[PATH_BYTES];
    if (readlink("/path-link", target, sizeof(target)) != PATH_BYTES - 1 || memcmp(target, second, PATH_BYTES - 1))
        return failed("long_readlink");
    memset(boundary, 'q', PATH_BYTES);
    if (readlink("/path-link", boundary, 1024 * 1024) != PATH_BYTES - 1 ||
        memcmp(boundary, second, PATH_BYTES - 1) || boundary[PATH_BYTES - 1] != 'q')
        return failed("readlink_actual_output_before_guard");
    memset(boundary, 'q', PATH_BYTES); errno = 0;
    if (readlink("/path-link", boundary + 2, 1024 * 1024) != -1 || errno != EFAULT)
        return failed("readlink_actual_output_crosses_guard");
    for (unsigned i = 0; i < PATH_BYTES; i++)
        if (boundary[i] != 'q') return failed("readlink_guard_output_preflight");
    puts("PASS glibc_path_bulk_guard_and_pairs");
    DIR *stream = opendir("/");
    if (!stream) return failed("long_directory_open");
    unsigned found = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(stream))) if (!strcmp(entry->d_name, alias + 1)) found++;
    if (errno || found != 1 || closedir(stream) || raw_long_entry(alias + 1) != 1)
        return failed("long_directory_record_280");
    if (mkdir(directory, 0700) || chdir(directory)) return failed("long_chdir");
    memset(boundary, 'q', PATH_BYTES); errno = 0;
    if (getcwd(boundary + 1, 1024 * 1024) || errno != EFAULT)
        return failed("getcwd_actual_output_crosses_guard");
    for (unsigned i = 0; i < PATH_BYTES; i++)
        if (boundary[i] != 'q') return failed("getcwd_guard_output_preflight");
    if (getcwd(boundary, 1024 * 1024) != boundary || strcmp(boundary, directory))
        return failed("getcwd_bounded_output_span");
    errno = 0;
    if (getcwd(boundary, PATH_BYTES - 1) || errno != ERANGE) return failed("getcwd_short_buffer");
    char *allocated = getcwd(NULL, 0);
    if (!allocated || strcmp(allocated, directory)) return failed("getcwd_allocated");
    free(allocated);
    char root[2];
    if (chdir("..") || getcwd(root, sizeof(root)) != root || strcmp(root, "/")) return failed("maximum_cwd_parent");
    if (rmdir(directory) || unlink("/path-link") || unlink(alias) || unlink(second) || munmap(pages, 2 * PAGE_BYTES))
        return failed("path_cleanup");
    puts("PASS glibc_path_getcwd_getdents_280");
    return 0;
}
