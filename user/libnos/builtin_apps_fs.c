/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 将独立编译的 EL0 用户态 ELF 应用在启动阶段写入 RamFS (/bin/ai_agent.elf)
 */

#include <fs/fs.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <stddef.h>
#include <stdint.h>

extern const uint8_t g_builtin_ai_agent_elf[];
extern const size_t g_builtin_ai_agent_elf_size;
extern const uint8_t g_builtin_tool_srv_elf[];
extern const size_t g_builtin_tool_srv_elf_size;

static int install_elf_file(const char *path, const uint8_t *buf, size_t size)
{
    size_t written = 0U;
    int fd = nos_open(path, O_CREAT | O_RDWR | O_TRUNC);

    if (fd < 0) {
        pr_err("builtin_apps_fs: open %s failed (%d)\n", path, fd);
        return fd;
    }

    while (written < size) {
        ssize_t n = nos_write(fd, buf + written, size - written);
        if (n <= 0) {
            (void)nos_close(fd);
            pr_err("builtin_apps_fs: write %s failed\n", path);
            return -EIO;
        }
        written += (size_t)n;
    }

    (void)nos_close(fd);
    pr_info("Installed EL0 C app %s (%u bytes)\n", path, (unsigned int)size);
    return 0;
}

static int builtin_user_apps_fs_init(void)
{
    int rc;

    rc = nos_mkdir("/bin", 0755);
    if (rc != 0 && rc != -EEXIST) {
        pr_err("builtin_apps_fs: mkdir /bin failed (%d)\n", rc);
        return rc;
    }

    rc = install_elf_file("/bin/ai_agent.elf", g_builtin_ai_agent_elf, g_builtin_ai_agent_elf_size);
    if (rc != 0) {
        return rc;
    }

    return install_elf_file("/bin/tool_srv.elf", g_builtin_tool_srv_elf, g_builtin_tool_srv_elf_size);
}

SYS_INIT(builtin_user_apps_fs_init, INIT_LEVEL_APPLICATION);
