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

static int builtin_user_apps_fs_init(void)
{
    size_t written = 0U;
    int rc;
    int fd;

    rc = nos_mkdir("/bin", 0755);
    if (rc != 0 && rc != -EEXIST) {
        pr_err("builtin_apps_fs: mkdir /bin failed (%d)\n", rc);
        return rc;
    }

    fd = nos_open("/bin/ai_agent.elf", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) {
        pr_err("builtin_apps_fs: open /bin/ai_agent.elf failed (%d)\n", fd);
        return fd;
    }

    while (written < g_builtin_ai_agent_elf_size) {
        ssize_t n = nos_write(fd,
                              g_builtin_ai_agent_elf + written,
                              g_builtin_ai_agent_elf_size - written);
        if (n <= 0) {
            (void)nos_close(fd);
            pr_err("builtin_apps_fs: write /bin/ai_agent.elf failed\n");
            return -EIO;
        }
        written += (size_t)n;
    }

    (void)nos_close(fd);
    pr_info("Installed EL0 C app /bin/ai_agent.elf (%u bytes)\n",
            (unsigned int)g_builtin_ai_agent_elf_size);
    return 0;
}

SYS_INIT(builtin_user_apps_fs_init, INIT_LEVEL_APPLICATION);
