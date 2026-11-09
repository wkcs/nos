/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * DevFS 单元测试：验证极简 DevFS 字符设备注册、/dev/ 挂载以及 null/zero/console 契约。
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fs/fs.h>
#include <fs/vfs.h>
#include <nos/tty.h>
#include "../../fs/vfs_fd.h"

uintptr_t kernel_running = 0;

struct task_struct *task_current(void)
{
    return NULL;
}

void vfs_dentry_put(struct dentry *dentry)
{
    (void)dentry;
}

int vfs_get_mount_count(void)
{
    return 0;
}

int vfs_get_mount_entry(int index, struct vfs_mount_entry *entry)
{
    (void)index;
    (void)entry;
    return -ENOENT;
}

int nos_close(int fd)
{
    return vfs_fd_close(fd);
}

/* Host 内存与 VFS 桩函数 */
void *kmalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return malloc(size);
}

void *kzalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return calloc(1, size);
}

void kfree(void *ptr)
{
    free(ptr);
}

struct inode *nos_vfs_alloc_inode(struct super_block *sb)
{
    struct inode *inode = calloc(1, sizeof(*inode));
    if (inode != NULL) {
        inode->i_sb = sb;
    }
    return inode;
}

void nos_vfs_destroy_inode(struct inode *inode)
{
    free(inode);
}

int register_filesystem(struct file_system_type *fs)
{
    (void)fs;
    return 0;
}

/* 模拟 TTY 底层收发缓冲 */
static unsigned char g_mock_rx_buf[64];
static unsigned int g_mock_rx_len;
static unsigned int g_mock_rx_pos;

static unsigned char g_mock_tx_buf[64];
static unsigned int g_mock_tx_len;

static int mock_uart_read(void *context, unsigned char *buf, unsigned int len, bool wait)
{
    (void)context;
    (void)wait;
    if (g_mock_rx_pos >= g_mock_rx_len) {
        return -EAGAIN;
    }
    unsigned int to_copy = len;
    if (g_mock_rx_pos + to_copy > g_mock_rx_len) {
        to_copy = g_mock_rx_len - g_mock_rx_pos;
    }
    memcpy(buf, &g_mock_rx_buf[g_mock_rx_pos], to_copy);
    g_mock_rx_pos += to_copy;
    return (int)to_copy;
}

static int mock_uart_write(void *context, const unsigned char *buf, unsigned int len)
{
    (void)context;
    if (g_mock_tx_len + len > sizeof(g_mock_tx_buf)) {
        len = sizeof(g_mock_tx_buf) - g_mock_tx_len;
    }
    memcpy(&g_mock_tx_buf[g_mock_tx_len], buf, len);
    g_mock_tx_len += len;
    return (int)len;
}

static const struct file_operations dummy_fops = { 0 };

static void test_register_validation(void)
{
    printf("1. 测试 devfs_register_chrdev 参数校验...\n");

    /* 空指针参数校验 */
    assert(devfs_register_chrdev(NULL, &dummy_fops, NULL) == -EINVAL);
    assert(devfs_register_chrdev("test", NULL, NULL) == -EINVAL);

    /* 空字符串或纯斜杠名称校验 */
    assert(devfs_register_chrdev("", &dummy_fops, NULL) == -EINVAL);
    assert(devfs_register_chrdev("/", &dummy_fops, NULL) == -EINVAL);
    assert(devfs_register_chrdev("/dev/", &dummy_fops, NULL) == -EINVAL);

    /* 正常注册 */
    int priv_val = 1234;
    assert(devfs_register_chrdev("test_dev1", &dummy_fops, &priv_val) == 0);

    /* 重复注册同一设备（幂等更新）应返回成功 */
    int priv_val2 = 5678;
    assert(devfs_register_chrdev("/dev/test_dev1", &dummy_fops, &priv_val2) == 0);

    printf("   -> devfs_register_chrdev 参数校验通过\n");
}

static void test_devfs_capacity(void)
{
    printf("5. 测试 DevFS 静态表容量边界 (最大 8 项)...\n");

    /* 当前已注册: null (1), zero (2), test_dev1 (3), console (4), ttyS0 (5) */
    int dummy_priv = 99;
    assert(devfs_register_chrdev("extra1", &dummy_fops, &dummy_priv) == 0); /* 6 */
    assert(devfs_register_chrdev("extra2", &dummy_fops, &dummy_priv) == 0); /* 7 */
    assert(devfs_register_chrdev("extra3", &dummy_fops, &dummy_priv) == 0); /* 8 (满) */

    /* 第 9 个设备应被拒绝并返回 -ENOMEM */
    assert(devfs_register_chrdev("overflow_dev", &dummy_fops, NULL) == -ENOMEM);

    /* 已存在的设备仍可幂等更新 */
    assert(devfs_register_chrdev("extra3", &dummy_fops, NULL) == 0);

    printf("   -> DevFS 8 项容量边界验证通过\n");
}

static void test_devfs_null(struct dentry *devfs_root)
{
    printf("2. 测试 /dev/null 读写行为...\n");

    struct dentry dentry = { 0 };
    dentry.d_name.name = (const unsigned char *)"null";
    dentry.d_name.len = 4;

    /* 查找 null 设备 */
    assert(devfs_root->d_inode->i_op->lookup(devfs_root->d_inode, &dentry) == NULL);
    assert(dentry.d_inode != NULL);
    assert(S_ISCHR(dentry.d_inode->i_mode));
    assert(dentry.d_inode->i_fop != NULL);

    struct file file = {
        .f_inode = dentry.d_inode,
        .f_op = dentry.d_inode->i_fop,
    };
    if (file.f_op->open != NULL) {
        assert(file.f_op->open(file.f_inode, &file) == 0);
    }

    /* 验证读操作：返回 0 (EOF)，且缓冲区不被污染 */
    char read_buf[16] = "unchanged";
    loff_t pos = 0;
    ssize_t read_bytes = file.f_op->read(&file, read_buf, sizeof(read_buf), &pos);
    assert(read_bytes == 0);
    assert(strcmp(read_buf, "unchanged") == 0);

    /* 验证写操作：返回写入长度，数据被丢弃 */
    const char *write_msg = "test_discard_data";
    ssize_t written = file.f_op->write(&file, write_msg, strlen(write_msg), &pos);
    assert(written == (ssize_t)strlen(write_msg));

    if (file.f_op->release != NULL) {
        assert(file.f_op->release(file.f_inode, &file) == 0);
    }
    nos_vfs_destroy_inode(dentry.d_inode);

    printf("   -> /dev/null (EOF/discard) 行为验证通过\n");
}

static void test_devfs_zero(struct dentry *devfs_root)
{
    printf("3. 测试 /dev/zero 读写行为...\n");

    struct dentry dentry = { 0 };
    dentry.d_name.name = (const unsigned char *)"zero";
    dentry.d_name.len = 4;

    /* 查找 zero 设备 */
    assert(devfs_root->d_inode->i_op->lookup(devfs_root->d_inode, &dentry) == NULL);
    assert(dentry.d_inode != NULL);
    assert(S_ISCHR(dentry.d_inode->i_mode));
    assert(dentry.d_inode->i_fop != NULL);

    struct file file = {
        .f_inode = dentry.d_inode,
        .f_op = dentry.d_inode->i_fop,
    };
    if (file.f_op->open != NULL) {
        assert(file.f_op->open(file.f_inode, &file) == 0);
    }

    /* 验证读操作：填满 0 并返回请求字节数 */
    char read_buf[32];
    memset(read_buf, 0xAA, sizeof(read_buf));
    loff_t pos = 0;
    ssize_t read_bytes = file.f_op->read(&file, read_buf, sizeof(read_buf), &pos);
    assert(read_bytes == (ssize_t)sizeof(read_buf));
    for (size_t i = 0; i < sizeof(read_buf); i++) {
        assert(read_buf[i] == 0);
    }

    /* 验证写操作：返回写入长度 */
    const char *write_msg = "zero_discard";
    ssize_t written = file.f_op->write(&file, write_msg, strlen(write_msg), &pos);
    assert(written == (ssize_t)strlen(write_msg));

    if (file.f_op->release != NULL) {
        assert(file.f_op->release(file.f_inode, &file) == 0);
    }
    nos_vfs_destroy_inode(dentry.d_inode);

    printf("   -> /dev/zero (全0填充/discard) 行为验证通过\n");
}

static void test_devfs_console(struct dentry *devfs_root)
{
    printf("4. 测试 /dev/console 与 TTY fops 绑定...\n");

    struct nos_tty mock_tty;
    const struct nos_tty_ops ops = {
        .read = mock_uart_read,
        .write = mock_uart_write,
    };
    assert(nos_tty_init(&mock_tty, &ops, NULL) == 0);

    /* 将 mock_tty 注册为 /dev/console 与 /dev/ttyS0 */
    assert(devfs_register_chrdev("console", &nos_tty_fops, &mock_tty) == 0);
    assert(devfs_register_chrdev("/dev/ttyS0", &nos_tty_fops, &mock_tty) == 0);

    /* 查找 /dev/console */
    struct dentry dentry = { 0 };
    dentry.d_name.name = (const unsigned char *)"console";
    dentry.d_name.len = 7;

    assert(devfs_root->d_inode->i_op->lookup(devfs_root->d_inode, &dentry) == NULL);
    assert(dentry.d_inode != NULL);
    assert(S_ISCHR(dentry.d_inode->i_mode));
    assert(dentry.d_inode->i_fop == &nos_tty_fops);
    assert(dentry.d_inode->i_private == &mock_tty);

    /* 模拟文件打开并发布到 VFS 描述符表 */
    struct file *file = kzalloc(sizeof(*file), 0);
    assert(file != NULL);
    file->f_inode = dentry.d_inode;
    file->f_op = dentry.d_inode->i_fop;
    file->f_flags = O_RDWR;
    assert(file->f_op->open(file->f_inode, file) == 0);
    assert(file->private_data == &mock_tty);

    int fd = vfs_fd_reserve();
    assert(fd >= 0);
    vfs_fd_publish(fd, file);

    /* 验证 nos_ioctl 通过 VFS FD 工作 */
    struct nos_termios tio;
    assert(nos_ioctl(fd, TCGETS, (unsigned long)(uintptr_t)&tio) == 0);
    tio.c_lflag |= ECHO;
    assert(nos_ioctl(fd, TCSETS, (unsigned long)(uintptr_t)&tio) == 0);
    assert(nos_ioctl(fd, 0x9999, 0) == -ENOSYS);
    assert(nos_ioctl(-1, TCGETS, 0) == -EBADF);
    assert(nos_ioctl(100, TCGETS, 0) == -EBADF);

    /* 写入验证 (通过 nos_write) */
    g_mock_tx_len = 0;
    const char *hello = "Hello Console!\n";
    ssize_t written = nos_write(fd, hello, strlen(hello));
    assert(written == (ssize_t)strlen(hello));
    assert(g_mock_tx_len == strlen(hello));
    assert(memcmp(g_mock_tx_buf, hello, strlen(hello)) == 0);

    /* 读取验证 (通过 nos_read) */
    g_mock_rx_buf[0] = 'X';
    g_mock_rx_len = 1;
    g_mock_rx_pos = 0;
    char in_buf[4] = { 0 };
    ssize_t nread = nos_read(fd, in_buf, sizeof(in_buf));
    assert(nread == 1);
    assert(in_buf[0] == 'X');

    /* 释放与关闭 (通过 nos_close 级联释放 file) */
    assert(nos_close(fd) == 0);
    nos_vfs_destroy_inode(dentry.d_inode);

    printf("   -> /dev/console 与 TTY fops 读写及 nos_ioctl 验证通过\n");
}

static int mock_filldir(void *dirent, const char *name, int len, loff_t offset, u64 ino, unsigned type)
{
    (void)dirent;
    (void)offset;
    (void)ino;
    (void)type;
    (void)name;
    (void)len;
    return 0;
}

static void test_devfs_readdir(struct dentry *devfs_root)
{
    printf("6. 测试 DevFS readdir 目录遍历...\n");

    struct file dir_file = {
        .f_inode = devfs_root->d_inode,
        .f_op = devfs_root->d_inode->i_fop,
        .f_pos = 0,
    };

    assert(dir_file.f_op != NULL && dir_file.f_op->readdir != NULL);
    assert(dir_file.f_op->readdir(&dir_file, NULL, mock_filldir) == 0);

    printf("   -> DevFS readdir 遍历验证通过\n");
}

int main(void)
{
    printf("========================================\n");
    printf("开始执行 DevFS 字符设备注册与 /dev/ 挂载测试\n");
    printf("========================================\n");

    /* 初始化 DevFS 并挂载 */
    assert(init_devfs() == 0);

    /* devfs 挂载得到根 dentry */
    extern struct dentry *vfs_mount(const char *fs_name, int flags, const char *dev_name, void *data);
    /* 采用 devfs 自身 mount 函数构建根节点测试 */
    extern struct file_system_type devfs_fs_type;
    struct dentry *root = devfs_fs_type.mount(&devfs_fs_type, 0, "devfs", NULL);
    assert(root != NULL);
    assert(root->d_inode != NULL);
    assert(S_ISDIR(root->d_inode->i_mode));

    test_register_validation();
    test_devfs_null(root);
    test_devfs_zero(root);
    test_devfs_console(root);
    test_devfs_capacity();
    test_devfs_readdir(root);

    printf("========================================\n");
    printf("所有 DevFS 单元测试全部通过 (ALL PASSED)!\n");
    printf("========================================\n");
    return 0;
}
