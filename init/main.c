/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/console.h>
#include <kernel/cpu.h>
#include <kernel/init.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <kernel/sch.h>
#include <kernel/sleep.h>

void nos_print_kernel_info(void) {
  pr_info("Welcome to NOS:\r\n");
  pr_info("    arch[%s]\r\n", CONFIG_ARCH);
  pr_info("    board[%s]\r\n", CONFIG_BOARD);
  pr_info("    build-time[%s]\r\n", CONFIG_BUILD_INFO);
  pr_info("    cpu-type[%s]\r\n", CONFIG_CPU_TYPE);
  pr_info("    version[%u.%u.%u]\r\n", (CONFIG_VERSION_CODE >> 16) & 0xff,
          (CONFIG_VERSION_CODE >> 8) & 0xff, CONFIG_VERSION_CODE & 0xff);
  pr_info("    page-size[%d]\r\n", CONFIG_PAGE_SIZE);
}

#include <fs/vfs.h>

int nos_start(void) {
  cpu_init();
  arch_init();
  board_init();
  console_init();
  kernel_log_init();
  nos_print_kernel_info();

  mm_node_early_init();
  mm_init();
  pid_init();
  sch_init();

  if (vfs_init()) {
    pr_err("VFS: init failed\r\n");
  } else {
    /* Mount Root FS (RamFS for now) */
    struct dentry *root = vfs_mount("ramfs", 0, NULL, NULL);
    if (root) {
      pr_info("VFS: RootFS mounted successfully\r\n");
      // Optional: Run verification test here instead of manual call
      // extern void vfs_test(void);
      // vfs_test();
    } else {
      pr_err("VFS: Failed to mount RootFS\r\n");
    }
  }

  sch_start();

  return 0;
}
