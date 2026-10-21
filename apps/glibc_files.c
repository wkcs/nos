/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static int failed(const char *operation)
{ fprintf(stderr, "FAIL glibc_files (%s, errno=%d)\n", operation, errno); return 92; }
static int positioned_bulk(void)
{
    unsigned char source[1537], output[1601];
    for (size_t i = 0; i < sizeof(source); i++) source[i] = (unsigned char)(i * 37u + i / 251u);
    int fd = open("/glibc-bulk-pread", O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || write(fd, source, sizeof(source)) != sizeof(source) || lseek(fd, 37, SEEK_SET) != 37)
        return failed("bulk_pread_setup");
    int alias = dup(fd);
    memset(output, 0xa5, sizeof(output));
    if (alias < 0 || pread(alias, output + 31, sizeof(source), 0) != sizeof(source) ||
        memcmp(output + 31, source, sizeof(source)) || lseek(fd, 0, SEEK_CUR) != 37)
        return failed("bulk_pread_unaligned_contents_offset");
    for (size_t i = 0; i < sizeof(output); i++)
        if ((i < 31 || i >= 31 + sizeof(source)) && output[i] != 0xa5) return failed("bulk_pread_canary");
    memset(output, 0xa5, sizeof(output));
    if (pread(fd, output, 1024, 1000) != 537 || memcmp(output, source + 1000, 537) ||
        pread(fd, output + 1024, 512, sizeof(source)) || lseek(alias, 0, SEEK_CUR) != 37)
        return failed("bulk_pread_eof_offset");
    for (size_t i = 537; i < sizeof(output); i++) if (output[i] != 0xa5) return failed("bulk_pread_eof_canary");
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return failed("bulk_pread_page_size");
    unsigned char *mapped = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) return failed("bulk_pread_map");
    unsigned char *crossing = mapped + page - 257;
    memset(mapped, 0x5a, (size_t)page * 2);
    if (pread(fd, crossing, 769, 123) != 769 || memcmp(crossing, source + 123, 769) ||
        crossing[-1] != 0x5a || crossing[769] != 0x5a) return failed("bulk_pread_cross_page");
    for (unsigned test = 0; test < 2; test++) {
        memset(mapped, 0x5a, (size_t)page * 2);
        if (mprotect(mapped + page, (size_t)page, test ? PROT_NONE : PROT_READ))
            return failed("bulk_pread_protect");
        errno = 0;
        if (pread(fd, crossing, 769, 123) != -1 || errno != EFAULT || lseek(fd, 0, SEEK_CUR) != 37)
            return failed("bulk_pread_guard_rejected");
        for (size_t i = 0; i < 257; i++) if (crossing[i] != 0x5a) return failed("bulk_pread_guard_prefix");
        if (mprotect(mapped + page, (size_t)page, PROT_READ | PROT_WRITE)) return failed("bulk_pread_unprotect");
        for (size_t i = 257; i < 769; i++) if (crossing[i] != 0x5a) return failed("bulk_pread_guard_tail");
    }
    if (munmap(mapped, (size_t)page * 2)) return failed("bulk_pread_unmap");
    int writing = open("/glibc-bulk-pread", O_WRONLY), directory = open("/", O_RDONLY | O_DIRECTORY);
    if (writing < 0 || directory < 0) return failed("bulk_pread_bad_mode_setup");
    errno = 0;
    if (pread(writing, output, 512, 0) != -1 || errno != EBADF) return failed("bulk_pread_write_only");
    errno = 0;
    if (pread(directory, output, 512, 0) != -1 || errno != EISDIR) return failed("bulk_pread_directory");
    if (close(writing) || close(directory) || close(alias) || close(fd) || unlink("/glibc-bulk-pread"))
        return failed("bulk_pread_cleanup");
    puts("PASS glibc_pread_bulk_contents_guard_offset");
    /* Compare the new archive path against small inline reads of the same bytes. */
    fd = open("/lib/libc.so.6", O_RDONLY);
    if (fd < 0 || lseek(fd, 19, SEEK_SET) != 19) return failed("bulk_pread_archive_open");
    for (size_t done = 0; done < 769;) {
        size_t wanted = 769 - done < 64 ? 769 - done : 64;
        if (pread(fd, source + done, wanted, 129 + (off_t)done) != (ssize_t)wanted)
            return failed("bulk_pread_archive_reference");
        done += wanted;
    }
    memset(output, 0xa5, sizeof(output));
    if (pread(fd, output + 7, 769, 129) != 769 || memcmp(output + 7, source, 769) ||
        output[6] != 0xa5 || output[776] != 0xa5 || lseek(fd, 0, SEEK_CUR) != 19 || close(fd))
        return failed("bulk_pread_archive_contents_offset");
    puts("PASS glibc_pread_bulk_archive_inline_equivalence");
    return 0;
}
static int long_paths(void)
{
    char parent[256] = "/path", first[256], second[256], symbol[256], data[256];
    size_t levels[5] = {5};
    if (mkdir(parent, 0700)) return failed("long_base");
    for (unsigned i = 0; i < 4; i++) {
        size_t length = strlen(parent);
        parent[length++] = '/'; memset(parent + length, 'a' + (int)i, 50);
        parent[length + 50] = 0; levels[i + 1] = length + 50;
        if (mkdir(parent, 0700)) return failed("long_directory");
    }
    size_t prefix = strlen(parent);
    memcpy(first, parent, prefix); first[prefix] = '/';
    memset(first + prefix + 1, 'f', 45); first[255] = 0;
    memcpy(second, first, sizeof(first)); memset(second + prefix + 1, 'g', 45);
    memcpy(symbol, first, sizeof(first)); memset(symbol + prefix + 1, 'h', 45);
    int fd = open(first, O_CREAT | O_EXCL | O_RDWR, 0600);
    struct stat a, b;
    if (fd < 0 || write(fd, "long", 4) != 4 || link(first, second) ||
        stat(first, &a) || stat(second, &b) || a.st_ino != b.st_ino || a.st_nlink != 2 ||
        symlink(first, symbol) || readlink(symbol, data, sizeof(data)) != 255 || memcmp(data, first, 255))
        return failed("long_double_path");
    if (chdir(parent) || !getcwd(data, sizeof(data)) || strcmp(data, parent)) return failed("long_cwd");
    errno = 0;
    if (getcwd(data, 64) || errno != ERANGE || chdir("/")) return failed("long_cwd_small");
    if (unlink(second) || rename(first, second)) return failed("long_rename");
    errno = 0;
    if (stat(symbol, &b) != -1 || errno != ENOENT || unlink(symbol) || unlink(second) ||
        pread(fd, data, 4, 0) != 4 || memcmp(data, "long", 4) || close(fd)) return failed("long_lifetime");
    char too_long[258]; memset(too_long, 'x', sizeof(too_long)); too_long[0] = '/'; too_long[256] = 0;
    errno = 0;
    if (open(too_long, O_RDONLY) != -1 || errno != ENOENT) return failed("name255_missing");
    too_long[256] = 'x'; too_long[257] = 0;
    errno = 0;
    if (open(too_long, O_RDONLY) != -1 || errno != ENAMETOOLONG) return failed("path_limit");
    puts("PASS glibc_path_255_double_path_cwd");
    char name[201], full[256]; memset(name, 'z', 200); name[200] = 0;
    memcpy(full, "/path/", 6); memcpy(full + 6, name, sizeof(name));
    fd = open(full, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0 || close(fd)) return failed("long_name_create");
    DIR *directory = opendir("/path");
    if (!directory) return failed("long_name_opendir");
    unsigned found = 0; struct dirent *entry;
    errno = 0;
    while ((entry = readdir(directory))) if (!strcmp(entry->d_name, name)) {
        if (entry->d_reclen < 224) return failed("long_name_record");
        found++;
    }
    if (errno || found != 1 || closedir(directory) || unlink(full)) return failed("long_name_iteration");
    for (int i = 4; i >= 0; i--) { parent[levels[i]] = 0; if (rmdir(parent)) return failed("long_cleanup"); }
    puts("PASS glibc_directory_long_fragmented_record");
    return 0;
}
int main(void)
{
    int bulk = positioned_bulk();
    if (bulk) return bulk;
    mode_t previous = umask(0077);
    if (mkdir("/glibc-files", 0777)) return failed("mkdir");
    int directory = open("/glibc-files", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat info;
    if (directory < 0 || fstat(directory, &info) || !S_ISDIR(info.st_mode) ||
        (info.st_mode & 0777) != 0700) return failed("directory_mode");
    int fd = openat(directory, "data", O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0 || fstat(fd, &info) || !S_ISREG(info.st_mode) ||
        (info.st_mode & 0777) != 0600 || info.st_size || info.st_nlink != 1) return failed("file_mode");
    if (write(fd, "abcdef", 6) != 6 || lseek(fd, -2, SEEK_END) != 4) return failed("signed_seek");
    char data[32] = {0};
    if (read(fd, data, 2) != 2 || memcmp(data, "ef", 2) || pwrite(fd, "XY", 2, 2) != 2 ||
        lseek(fd, 0, SEEK_CUR) != 6 || pread(fd, data, 6, 0) != 6 || memcmp(data, "abXYef", 6))
        return failed("positioned_io");
    if (ftruncate(fd, sizeof(data)) || pread(fd, data, sizeof(data), 0) != sizeof(data) ||
        fstat(fd, &info) || info.st_size != sizeof(data)) return failed("grow");
    for (unsigned i = 6; i < sizeof(data); i++) if (data[i]) return failed("zero_extension");
    if (ftruncate(fd, 3) || lseek(fd, 0, SEEK_CUR) != 6 || fchmod(fd, 0640) ||
        fstat(fd, &info) || info.st_size != 3 || (info.st_mode & 0777) != 0640) return failed("shrink_mode");
    puts("PASS glibc_file_modes_positioned_io_truncate");
    ino_t inode = info.st_ino;
    if (linkat(directory, "data", directory, "alias", 0) ||
        fstatat(directory, "alias", &info, 0) || info.st_ino != inode || info.st_nlink != 2 ||
        symlinkat("alias", directory, "symbol") ||
        fstatat(directory, "symbol", &info, AT_SYMLINK_NOFOLLOW) || !S_ISLNK(info.st_mode) ||
        info.st_size != 5 || readlinkat(directory, "symbol", data, sizeof(data)) != 5 ||
        memcmp(data, "alias", 5) || fstatat(directory, "symbol", &info, 0) || info.st_ino != inode)
        return failed("link_identity");
    if (renameat(directory, "alias", directory, "kept") || unlinkat(directory, "data", 0))
        return failed("rename_unlinkat");
    errno = 0;
    if (fstatat(directory, "symbol", &info, 0) != -1 || errno != ENOENT ||
        unlinkat(directory, "symbol", 0) || unlinkat(directory, "kept", 0) ||
        fstat(fd, &info) || info.st_nlink || pread(fd, data, 3, 0) != 3 || memcmp(data, "abX", 3))
        return failed("unlinked_description");
    if (close(fd)) return failed("close_file");
    DIR *stream = fdopendir(directory);
    if (!stream || !(fcntl(dirfd(stream), F_GETFD) & FD_CLOEXEC)) return failed("fdopendir");
    struct dirent *entry;
    unsigned dots = 0;
    errno = 0;
    while ((entry = readdir(stream))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) dots++;
        else return failed("empty_directory");
    }
    if (errno || dots != 2 || closedir(stream) || rmdir("/glibc-files")) return failed("directory_cleanup");
    umask(previous);
    puts("PASS glibc_file_links_dirfd_lifetime");
    return long_paths();
}
