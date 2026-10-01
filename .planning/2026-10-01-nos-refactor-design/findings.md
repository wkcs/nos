# 事实与设计差距

- 基线 master@7ae39ba；已有 M1–M4 提交和 nk60 回迁。未跟踪旧文件保留。
- 原设计错误画成 kernel → drivers → board；目标需明确驱动可依赖内核服务，核心不依赖具体硬件。
- 启动汇编先清 BSS/复制 data 再 nos_start；旧设计顺序不符。
- EARLY 枚举存在但 nos_start 未执行；APPLICATION 在 sch_start 前同步运行；task_init 在 core 任务中仍活跃。
- init.h 传递包含 board/init.h；仅扫描源文件中 board/board.h 不足。
- 设备结构混合静态模型和 legacy ops；get_binding 仍指向动态链表。
- FatFs _FS_REENTRANT=0；option/syscall.c 是未接入的 Windows 示例。
- 当前 FatFs 无文件系统即格式化，mount 失败 fallback ramfs；smoke 不验证后端。
- ramdisk smoke 在 mount 后写 LBA 1；应移到专用测试设备或格式化前。
- qemu-stm32 依赖定制 nos_stm32 机型，链接脚本 RAM=256MiB，ramdisk=256KiB；不能推断真实 STM32 RAM 可承载。

- layer gate 退出 0；QEMU 10.2.0 含 nos_stm32，GCC 14.2.Rel1。
- 独立 out-dir 配置成功；编译退出 2，include 遮蔽来自未跟踪 board/qemu-stm32/inside-include/stm32f10x.h。未执行 QEMU 运行验收。
- 设计采用 POST_KERNEL/APPLICATION 在 core 任务执行、单卷 VFS 全局 mutex、显式 FatFs/ramdisk 后端、格式化策略与挂载错误码接口。
