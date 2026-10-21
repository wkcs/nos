/* Copyright (C) 2026 NOS contributors. Real glibc pathname boundaries. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PATH_BYTES 4096u
#define NAME_BYTES 255u
#define DEPTH 15u
#define BASE "/p4096"
#define MOVED_BASE "/q4096"
#define CHECK(condition, step) do { if (!(condition)) return failed(step); } while (0)
#define ERROR(operation, code, step) do { errno = 0; CHECK((operation) == -1 && errno == (code), step); } while (0)

/* The application has its own ELF mappings; these buffers do not enlarge the
 * native/VFS template or consume a gateway/server stack frame. */
static char directory[PATH_BYTES], first[PATH_BYTES], second[PATH_BYTES];
static char scratch[PATH_BYTES], target[PATH_BYTES + 1];
static size_t directory_lengths[DEPTH + 1];
static unsigned char *guard;
static int root_fd = -1, directory_fd = -1;
static ino_t file_inode;

static int failed(const char *step)
{
    fprintf(stderr, "FAIL glibc_path4096 %s errno=%d\n", step, errno);
    return 96;
}

static int joined(char output[PATH_BYTES], const char *parent, const char *name)
{
    size_t a = strlen(parent), b = strlen(name);
    if (a + 1 + b >= PATH_BYTES) return failed("fixture_join_bounds");
    memcpy(output, parent, a); output[a] = '/'; memcpy(output + a + 1, name, b + 1);
    return 0;
}

static int missing(const char *path)
{
    struct stat value;
    errno = 0;
    return stat(path, &value) == -1 && errno == ENOENT;
}

static int original_file(void)
{
    struct stat value;
    char bytes[6];
    int fd = open(first, O_RDONLY);
    if (fd < 0) return 0;
    int ok = !fstat(fd, &value) && value.st_ino == file_inode && value.st_size == 6 &&
        read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes) && !memcmp(bytes, "Source", sizeof(bytes));
    return !close(fd) && ok;
}

static int create_tree(void)
{
    CHECK(!mkdir(BASE, 0755), "create_base");
    strcpy(directory, BASE); directory_lengths[0] = strlen(directory);
    for (unsigned level = 0; level < DEPTH; level++) {
        size_t length = strlen(directory);
        CHECK(length + 1 + NAME_BYTES < PATH_BYTES, "fixture_directory_bound");
        directory[length++] = '/';
        memset(directory + length, (int)('a' + level), NAME_BYTES);
        directory[length + NAME_BYTES] = 0;
        CHECK(!mkdir(directory, 0755), "create_maximum_component_directory");
        directory_lengths[level + 1] = strlen(directory);
    }
    CHECK(strlen(directory) == 3846, "fixture_deep_directory_length");
    char name[249]; memset(name, 'f', sizeof(name) - 1); name[sizeof(name) - 1] = 0;
    CHECK(!joined(first, directory, name), "fixture_first_path");
    memset(name, 'g', sizeof(name) - 1);
    CHECK(!joined(second, directory, name) && strlen(first) == PATH_BYTES - 1 &&
          strlen(second) == PATH_BYTES - 1, "fixture_exact_path_lengths");
    int fd = open(first, O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK(fd >= 0 && write(fd, "Source", 6) == 6, "create_4095_byte_path");
    struct stat value;
    CHECK(!fstat(fd, &value) && !close(fd), "record_long_file_identity");
    file_inode = value.st_ino;
    root_fd = open("/", O_RDONLY | O_DIRECTORY);
    directory_fd = open(directory, O_RDONLY | O_DIRECTORY);
    CHECK(root_fd >= 0 && directory_fd >= 0, "pin_root_and_long_directory");
    return 0;
}

static int limits_and_names(void)
{
    CHECK(pathconf("/", _PC_PATH_MAX) == PATH_BYTES && fpathconf(root_fd, _PC_PATH_MAX) == PATH_BYTES,
          "real_libc_path_limit_matches_ram_namespace");
    CHECK(pathconf("/", _PC_NAME_MAX) == NAME_BYTES && fpathconf(root_fd, _PC_NAME_MAX) == NAME_BYTES,
          "real_libc_name_limit_matches_ram_namespace");
    char name[NAME_BYTES + 2];
    memset(name, 'z', NAME_BYTES); name[NAME_BYTES] = 0;
    CHECK(!joined(scratch, BASE, name), "maximum_name_fixture");
    int fd = open(scratch, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && !close(fd), "component_255_succeeds");
    DIR *stream = opendir(BASE);
    CHECK(stream, "maximum_component_directory_stream");
    unsigned found = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(stream))) if (!strcmp(entry->d_name, name)) found++;
    CHECK(!errno && found == 1 && !closedir(stream) && !unlink(scratch),
          "real_readdir_preserves_all_255_name_bytes");
    name[NAME_BYTES] = 'z'; name[NAME_BYTES + 1] = 0;
    CHECK(!joined(scratch, BASE, name), "oversized_name_fixture");
    ERROR(open(scratch, O_CREAT | O_EXCL | O_RDWR, 0600), ENAMETOOLONG, "component_256_file_rejected");
    ERROR(mkdir(scratch, 0700), ENAMETOOLONG, "component_256_directory_rejected");
    CHECK(original_file(), "name_failures_leave_existing_long_file");
    return 0;
}

static int guard_boundary(void)
{
    CHECK(sysconf(_SC_PAGESIZE) == PATH_BYTES, "page_size_for_guard_fixture");
    guard = mmap(NULL, PATH_BYTES * 2, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(guard != MAP_FAILED && !mprotect(guard + PATH_BYTES, PATH_BYTES, PROT_NONE), "guard_mapping");
    memcpy(guard, first, PATH_BYTES);
    int fd = open((char *)guard, O_RDONLY);
    CHECK(fd >= 0 && !close(fd), "nul_at_byte_4095_before_guard_succeeds");
    memset(guard, 'x', PATH_BYTES); guard[0] = '/';
    ERROR(open((char *)guard, O_RDONLY), ENAMETOOLONG, "4096_non_nul_bytes_do_not_read_guard");
    ERROR(open((char *)guard + 1, O_RDONLY), EFAULT, "short_readable_span_faults_before_path_limit");
    CHECK(original_file(), "copy_failures_leave_long_file");
    return 0;
}

static int two_paths_atomic(void)
{
    CHECK(!rename(first, second) && missing(first), "rename_two_4095_byte_paths");
    struct stat value, alias;
    CHECK(!stat(second, &value) && value.st_ino == file_inode && !rename(second, first),
          "rename_preserves_file_identity");
    CHECK(!link(first, second) && !stat(first, &value) && !stat(second, &alias) &&
          value.st_ino == alias.st_ino && value.st_ino == file_inode && value.st_nlink == 2 &&
          !unlink(second), "link_two_4095_byte_paths");
    char *bad = (char *)guard + PATH_BYTES - 17;
    memset(bad, 'b', 17);
    ERROR(rename(first, bad), EFAULT, "rename_bad_second_copy");
    CHECK(original_file() && missing(second), "rename_second_copy_no_namespace_change");
    ERROR(rename(bad, first), EFAULT, "rename_bad_first_copy");
    CHECK(original_file() && missing(second), "rename_first_copy_preserves_existing_target");
    ERROR(link(first, bad), EFAULT, "link_bad_second_copy");
    CHECK(!stat(first, &value) && value.st_ino == file_inode && value.st_nlink == 1 &&
          original_file() && missing(second), "link_copy_failure_keeps_link_count");
    ERROR(symlink(first, bad), EFAULT, "symlink_bad_second_copy");
    CHECK(original_file() && missing(second), "symlink_copy_failure_preserves_target");
    return 0;
}

static int symlink_readlink(void)
{
    CHECK(!symlink(first, second), "symlink_two_4095_byte_inputs");
    struct stat value;
    CHECK(!lstat(second, &value) && S_ISLNK(value.st_mode) && value.st_size == PATH_BYTES - 1,
          "long_symlink_size");
    memset(target, 0x5a, sizeof(target));
    CHECK(readlink(second, target, sizeof(target)) == PATH_BYTES - 1 &&
          !memcmp(target, first, PATH_BYTES - 1) && target[PATH_BYTES - 1] == 0x5a &&
          target[PATH_BYTES] == 0x5a, "readlink_exact_contents_without_nul_or_overwrite");
    unsigned char short_target[18]; memset(short_target, 0x6b, sizeof(short_target));
    CHECK(readlink(second, (char *)short_target, sizeof(short_target) - 1) == (ssize_t)sizeof(short_target) - 1 &&
          !memcmp(short_target, first, sizeof(short_target) - 1) && short_target[17] == 0x6b,
          "readlink_truncates_without_terminating");
    CHECK(!stat(second, &value) && value.st_ino == file_inode, "absolute_long_symlink_resolves");
    ERROR(open(second, O_RDONLY | O_NOFOLLOW), ELOOP, "long_final_symlink_nofollow");
    CHECK(!unlink(second), "remove_absolute_symlink");
    const char *name = strrchr(second, '/') + 1;
    CHECK(!symlink(name, second), "deep_relative_symlink_loop");
    ERROR(open(second, O_RDONLY), ELOOP, "deep_relative_symlink_loop_is_bounded");
    CHECK(!unlink(second) && original_file(), "symlink_cleanup_preserves_regular_inode");
    return 0;
}

static int cwd_rename_identity(void)
{
    CHECK(!fchdir(directory_fd) && !rename(BASE, MOVED_BASE), "rename_ancestor_of_pinned_cwd");
    directory[1] = 'q'; first[1] = 'q'; second[1] = 'q';
    CHECK(getcwd(scratch, sizeof(scratch)) == scratch && !strcmp(scratch, directory),
          "cwd_reconstructs_renamed_ancestor");
    int fd = openat(directory_fd, strrchr(first, '/') + 1, O_RDONLY);
    struct stat value;
    CHECK(fd >= 0 && !fstat(fd, &value) && value.st_ino == file_inode && !close(fd),
          "dirfd_identity_survives_ancestor_rename");
    CHECK(!fchdir(root_fd) && !fchdir(directory_fd) && original_file(), "fchdir_renamed_directory");
    errno = 0;
    CHECK(!getcwd(scratch, strlen(directory)) && errno == ERANGE, "long_getcwd_short_buffer");
    CHECK(!rename(MOVED_BASE, BASE), "restore_ancestor_name");
    directory[1] = 'p'; first[1] = 'p'; second[1] = 'p';
    CHECK(getcwd(scratch, sizeof(scratch)) == scratch && !strcmp(scratch, directory),
          "cwd_tracks_second_ancestor_rename");
    return 0;
}

static int unlinked_directory_identity(void)
{
    CHECK(!joined(scratch, directory, "held") && !mkdir(scratch, 0755), "create_held_directory");
    int old = open(scratch, O_RDONLY | O_DIRECTORY);
    struct stat before, after, replacement;
    CHECK(old >= 0 && !fstat(old, &before) && !fchdir(old) && !rmdir(scratch), "unlink_pinned_directory");
    CHECK(!mkdir(scratch, 0755) && !stat(scratch, &replacement) && replacement.st_ino != before.st_ino &&
          !fstat(old, &after) && after.st_ino == before.st_ino, "same_name_replacement_has_distinct_inode");
    errno = 0;
    CHECK(!getcwd(target, sizeof(target)) && errno == ENOENT, "unlinked_cwd_does_not_report_replacement_path");
    CHECK(!fchdir(root_fd) && !fchdir(old), "fchdir_unlinked_old_directory");
    ERROR(openat(old, "child", O_CREAT | O_EXCL | O_RDWR, 0600), ENOENT,
          "unlinked_dirfd_does_not_create_under_replacement");
    CHECK(!fchdir(directory_fd) && !close(old), "release_old_unlinked_directory");
    CHECK(!joined(target, scratch, "child"), "replacement_child_fixture");
    int fd = open(target, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && !close(fd) && !unlink(target) && !rmdir(scratch), "replacement_namespace_remains_independent");
    return 0;
}

static int relative_search_permissions(void)
{
    CHECK(!joined(scratch, directory, "relative"), "relative_file_fixture");
    int fd = open(scratch, O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK(fd >= 0 && write(fd, "R", 1) == 1 && !close(fd), "create_relative_file");
    CHECK(!joined(target, directory, "denied") && !mkdir(target, 0000), "create_denied_component");
    int kept = open(first, O_RDONLY);
    CHECK(kept >= 0 && !chmod(first, 0000) && !chmod(BASE, 0000) && !seteuid((uid_t)70001),
          "drop_effective_identity_after_pin_and_chmod");
    fd = openat(directory_fd, "relative", O_RDONLY);
    char byte = 0;
    CHECK(fd >= 0 && read(fd, &byte, 1) == 1 && byte == 'R' && !close(fd),
          "relative_dirfd_does_not_recheck_denied_ancestors");
    ERROR(open(scratch, O_RDONLY), EACCES, "absolute_path_checks_denied_ancestor");
    ERROR(openat(directory_fd, strrchr(first, '/') + 1, O_RDONLY), EACCES,
          "relative_lookup_checks_final_file_mode");
    CHECK(read(kept, &byte, 1) == 1 && byte == 'S', "existing_file_fd_retains_read_authorization");
    CHECK(!seteuid(0) && !chmod(BASE, 0755) && !seteuid((uid_t)70001), "restore_ancestor_but_keep_internal_denial");
    ERROR(openat(directory_fd, "denied/../relative", O_RDONLY), EACCES,
          "dotdot_cannot_lexically_remove_denied_search");
    CHECK(!fchdir(directory_fd), "fchdir_uses_pinned_start_directory_permissions");
    fd = open("relative", O_RDONLY);
    CHECK(fd >= 0 && !close(fd), "relative_cwd_does_not_recheck_ancestors");
    CHECK(!seteuid(0) && !chmod(first, 0644) && !close(kept) && !unlink(scratch) && !rmdir(target),
          "restore_identity_and_permission_fixture");
    return 0;
}

static int raw_directory_record(void)
{
    unsigned char bytes[560];
    int fd = open(directory, O_RDONLY | O_DIRECTORY), found = 0;
    CHECK(fd >= 0, "open_deep_directory_for_raw_records");
    const char *name = strrchr(first, '/') + 1;
    for (;;) {
        long count = syscall(SYS_getdents64, fd, bytes, sizeof(bytes));
        CHECK(count >= 0, "raw_deep_directory_read");
        if (!count) break;
        for (size_t offset = 0; offset < (size_t)count;) {
            uint16_t length;
            CHECK((size_t)count - offset >= 24, "directory_record_header_bound");
            memcpy(&length, bytes + offset + 16, sizeof(length));
            CHECK(length >= 24 && length <= (size_t)count - offset &&
                  memchr(bytes + offset + 19, 0, length - 19), "directory_record_length_and_nul");
            if (!strcmp((char *)bytes + offset + 19, name)) found += length == 272;
            offset += length;
        }
    }
    CHECK(!close(fd) && found == 1, "raw_record_preserves_248_byte_component");
    DIR *stream = opendir(directory);
    CHECK(stream, "real_libc_open_deep_directory");
    found = 0; errno = 0;
    struct dirent *entry;
    while ((entry = readdir(stream))) if (!strcmp(entry->d_name, name)) found++;
    CHECK(!errno && found == 1 && !closedir(stream), "real_libc_readdir_preserves_long_component");
    return 0;
}

static int cleanup(void)
{
    CHECK(!fchdir(root_fd) && !close(directory_fd) && !unlink(first), "release_long_file_and_directory_reference");
    for (unsigned i = DEPTH + 1; i-- > 0;) {
        directory[directory_lengths[i]] = 0;
        CHECK(!rmdir(directory), "remove_deep_directory_tree");
    }
    CHECK(missing(BASE) && missing(MOVED_BASE), "removed_tree_has_no_old_or_renamed_namespace");
    CHECK(!close(root_fd) && !munmap(guard, PATH_BYTES * 2), "release_root_and_guard_mapping");
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "qemu")), "arguments");
    CHECK(!create_tree() && !limits_and_names(), "name_and_limit_checks");
    puts("PASS glibc_path4096_limits_name255");
    CHECK(!guard_boundary(), "guard_checks");
    puts("PASS glibc_path4096_guard_boundary");
    CHECK(!two_paths_atomic(), "two_path_checks");
    puts("PASS glibc_path4096_two_paths_atomic");
    CHECK(!symlink_readlink(), "symlink_checks");
    puts("PASS glibc_path4096_symlink_readlink");
    CHECK(!cwd_rename_identity(), "rename_cwd_checks");
    puts("PASS glibc_path4096_cwd_rename_identity");
    CHECK(!unlinked_directory_identity(), "unlinked_directory_checks");
    puts("PASS glibc_path4096_unlinked_directory_identity");
    CHECK(!relative_search_permissions(), "relative_permission_checks");
    puts("PASS glibc_path4096_relative_search_permissions");
    CHECK(!raw_directory_record(), "directory_record_checks");
    puts("PASS glibc_path4096_directory_records");
    CHECK(!cleanup(), "cleanup_checks");
    puts("PASS glibc_path4096_cleanup");
    puts("PASS glibc_posix_path4096");
    return 0;
}
