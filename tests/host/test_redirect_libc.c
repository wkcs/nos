/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * C 库标准流 VFS 对齐与重定向单元测试
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include <fs/vfs.h>
#include <kernel/task.h>
#include <kernel/console.h>
#include "../../fs/vfs_fd.h"

_ssize_t _write(int fd, const void *buf, size_t cnt);
_ssize_t _read(int fd, void *buf, size_t cnt);
int _close(int fd);
_off_t _lseek(int fd, _off_t offset, int whence);
int _isatty(int fd);
int _fstat(int fd, struct stat *st);

bool kernel_running = true;
uint64_t sys_heartbeat_time = 0;

static struct task_struct *s_current_task = NULL;

struct task_struct *task_current(void)
{
    return s_current_task;
}

int nos_close(int fd)
{
    return vfs_fd_close(fd);
}

int nos_open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    return -ENOSYS;
}

int nos_dup(int oldfd)
{
    return vfs_fd_dup(oldfd);
}

int nos_dup2(int oldfd, int newfd)
{
    return vfs_fd_dup2(oldfd, newfd);
}

/* VFS 超级块存根 */
int vfs_get_mount_count(void) { return 0; }
int vfs_get_mount_entry(int index, struct vfs_mount_entry *entry) { (void)index; (void)entry; return -ENOENT; }

/* 控制台模拟存根 */
static char s_console_buf[256];
static size_t s_console_len = 0;
static char s_console_input[256];
static size_t s_console_input_pos = 0;
static size_t s_console_input_len = 0;

int console_write(const char *buf, int len)
{
    if (len <= 0) return 0;
    if (s_console_len + (size_t)len < sizeof(s_console_buf)) {
        memcpy(s_console_buf + s_console_len, buf, (size_t)len);
        s_console_len += (size_t)len;
        s_console_buf[s_console_len] = '\0';
    }
    return len;
}

int console_getc(void)
{
    if (s_console_input_pos < s_console_input_len) {
        return (unsigned char)s_console_input[s_console_input_pos++];
    }
    return -1;
}

/* 内存虚拟文件支持 */
struct mock_file_data {
    char buffer[512];
    size_t size;
};

static ssize_t mock_write(struct file *file, const char *buf, size_t count, loff_t *pos)
{
    struct mock_file_data *data = (struct mock_file_data *)file->private_data;
    if ((size_t)*pos + count > sizeof(data->buffer)) {
        count = sizeof(data->buffer) - (size_t)*pos;
    }
    memcpy(data->buffer + *pos, buf, count);
    *pos += count;
    if ((size_t)*pos > data->size) {
        data->size = (size_t)*pos;
    }
    file->f_inode->i_size = (loff_t)data->size;
    return (ssize_t)count;
}

static ssize_t mock_read(struct file *file, char *buf, size_t count, loff_t *pos)
{
    struct mock_file_data *data = (struct mock_file_data *)file->private_data;
    if ((size_t)*pos >= data->size) return 0;
    if ((size_t)*pos + count > data->size) {
        count = data->size - (size_t)*pos;
    }
    memcpy(buf, data->buffer + *pos, count);
    *pos += count;
    return (ssize_t)count;
}

static int mock_release(struct inode *inode, struct file *file)
{
    (void)inode;
    (void)file;
    return 0;
}

static const struct file_operations s_mock_fops = {
    .write = mock_write,
    .read = mock_read,
    .release = mock_release,
};

static struct file *create_mock_file(struct mock_file_data *data, mode_t mode)
{
    struct file *f = calloc(1, sizeof(*f));
    struct inode *ino = calloc(1, sizeof(*ino));
    ino->i_mode = mode;
    ino->i_size = (loff_t)data->size;
    f->f_inode = ino;
    f->f_op = &s_mock_fops;
    f->private_data = data;
    f->f_flags = O_RDWR;
    f->f_refs = 1;
    mutex_init(&f->f_pos_lock);
    return f;
}

static void test_stdout_redirection(void)
{
    struct task_struct task;
    vfs_task_files_init(&task, NULL);
    s_current_task = &task;

    struct mock_file_data dummy;
    memset(&dummy, 0, sizeof(dummy));
    /* 预设 0, 1, 2 */
    vfs_fd_publish(0, create_mock_file(&dummy, S_IFCHR | 0666));
    vfs_fd_publish(1, create_mock_file(&dummy, S_IFCHR | 0666));
    vfs_fd_publish(2, create_mock_file(&dummy, S_IFCHR | 0666));

    struct mock_file_data data_out;
    memset(&data_out, 0, sizeof(data_out));
    struct file *f = create_mock_file(&data_out, S_IFREG | 0644);

    int file_fd = vfs_fd_reserve();
    assert(file_fd == 3);
    vfs_fd_publish(file_fd, f);

    /* 重定向 FD 1 到该文件 */
    assert(nos_dup2(file_fd, 1) == 1);
    assert(_isatty(1) == 0);

    /* 调用 libc _write(1, ...) */
    const char msg[] = "Test Message\n";
    _ssize_t written = _write(1, msg, sizeof(msg) - 1);
    assert(written == (sizeof(msg) - 1));

    /* 验证文件已包含写入的数据 */
    assert(data_out.size == sizeof(msg) - 1);
    assert(memcmp(data_out.buffer, msg, sizeof(msg) - 1) == 0);

    /* 还原/关闭 */
    assert(_close(1) == 0);
    assert(nos_close(file_fd) == 0);
    assert(_close(0) == 0);
    assert(_close(2) == 0);
    vfs_task_files_cleanup(&task);
}

static void test_stdout_fallback(void)
{
    struct task_struct task;
    vfs_task_files_init(&task, NULL);
    s_current_task = &task;

    /* FD 1 未打开，nos_write 返回 -EBADF */
    assert(vfs_fd_get(1) == NULL);

    s_console_len = 0;
    const char fallback_msg[] = "Fallback Output\n";
    _ssize_t written = _write(1, fallback_msg, sizeof(fallback_msg) - 1);
    assert(written == (sizeof(fallback_msg) - 1));

    /* 验证回退输出到了 console_write */
    assert(s_console_len == sizeof(fallback_msg) - 1);
    assert(strcmp(s_console_buf, fallback_msg) == 0);

    vfs_task_files_cleanup(&task);
}

static void test_stdin_redirection(void)
{
    struct task_struct task;
    vfs_task_files_init(&task, NULL);
    s_current_task = &task;

    struct mock_file_data dummy;
    memset(&dummy, 0, sizeof(dummy));
    /* 预设 0, 1, 2 */
    vfs_fd_publish(0, create_mock_file(&dummy, S_IFCHR | 0666));
    vfs_fd_publish(1, create_mock_file(&dummy, S_IFCHR | 0666));
    vfs_fd_publish(2, create_mock_file(&dummy, S_IFCHR | 0666));

    struct mock_file_data data_in;
    memset(&data_in, 0, sizeof(data_in));
    const char in_msg[] = "Input Data\n";
    memcpy(data_in.buffer, in_msg, sizeof(in_msg) - 1);
    data_in.size = sizeof(in_msg) - 1;

    struct file *f = create_mock_file(&data_in, S_IFREG | 0644);
    int file_fd = vfs_fd_reserve();
    assert(file_fd == 3);
    vfs_fd_publish(file_fd, f);

    /* 重定向 FD 0 到该文件 */
    assert(nos_dup2(file_fd, 0) == 0);
    assert(_isatty(0) == 0);

    char read_buf[32] = {0};
    _ssize_t rd = _read(0, read_buf, sizeof(read_buf));
    assert(rd == (sizeof(in_msg) - 1));
    assert(memcmp(read_buf, in_msg, sizeof(in_msg) - 1) == 0);

    assert(_close(0) == 0);
    assert(nos_close(file_fd) == 0);
    assert(_close(1) == 0);
    assert(_close(2) == 0);
    vfs_task_files_cleanup(&task);
}

static void test_stdin_fallback(void)
{
    struct task_struct task;
    vfs_task_files_init(&task, NULL);
    s_current_task = &task;

    /* FD 0 未打开 */
    assert(vfs_fd_get(0) == NULL);

    s_console_input[0] = 'Q';
    s_console_input_pos = 0;
    s_console_input_len = 1;

    char ch = 0;
    _ssize_t rd = _read(0, &ch, 1);
    assert(rd == 1);
    assert(ch == 'Q');

    vfs_task_files_cleanup(&task);
}

static void test_isatty_and_fstat(void)
{
    struct task_struct task;
    vfs_task_files_init(&task, NULL);
    s_current_task = &task;

    /* 字符设备 FD */
    struct mock_file_data chr_data;
    memset(&chr_data, 0, sizeof(chr_data));
    struct file *chr_file = create_mock_file(&chr_data, S_IFCHR | 0666);
    int fd = vfs_fd_reserve();
    vfs_fd_publish(fd, chr_file);
    assert(nos_dup2(fd, 1) == 1);

    assert(_isatty(1) == 1);
    struct stat st;
    assert(_fstat(1, &st) == 0);
    assert(S_ISCHR(st.st_mode));

    _close(1);
    nos_close(fd);

    /* 未打开的 FD 1 回退检测 */
    assert(_isatty(1) == 1);
    assert(_fstat(1, &st) == 0);
    assert(S_ISCHR(st.st_mode));

    /* 未打开的非标准流 FD 必须返回 EBADF */
    errno = 0;
    assert(_fstat(5, &st) == -1);
    assert(errno == EBADF);
    errno = 0;
    assert(_isatty(5) == 0);
    assert(errno == EBADF);

    vfs_task_files_cleanup(&task);
}

static void test_lseek(void)
{
    struct task_struct task;
    vfs_task_files_init(&task, NULL);
    s_current_task = &task;

    struct mock_file_data data;
    memset(&data, 0, sizeof(data));
    data.size = 20;
    struct file *f = create_mock_file(&data, S_IFREG | 0644);
    int fd = vfs_fd_reserve();
    vfs_fd_publish(fd, f);

    assert(_lseek(fd, 5, SEEK_SET) == 5);
    assert(_lseek(fd, 3, SEEK_CUR) == 8);
    assert(_lseek(fd, 0, SEEK_SET) == 0);

    _close(fd);
    vfs_task_files_cleanup(&task);
}

int main(void)
{
    test_stdout_redirection();
    test_stdout_fallback();
    test_stdin_redirection();
    test_stdin_fallback();
    test_isatty_and_fstat();
    test_lseek();
    printf("All libc redirection tests passed!\n");
    return 0;
}
