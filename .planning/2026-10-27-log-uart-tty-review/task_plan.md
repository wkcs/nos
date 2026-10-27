# NOS 日志、UART 与 TTY 设计审查

目标：基于源码评估 STM32 小内存和 ARM64 大内存配置，给出有证据的风险、分层方案、内存预算与演进步骤；本轮不改生产代码。

## 阶段
1. [complete] 梳理构建、配置、日志、串口、TTY、libc 和架构路径
2. [complete] 验证关键正确性问题并量化缓冲区与输出时延
3. [complete] 对照官方资料提出可裁剪的统一设计
4. [complete] 形成审查文档并汇总结论和验证边界

## 遇到的问题
- 初次并行目录检索时 .planning 目录尚未创建；笔记目录现已创建。
- 初始检索中的 board/qemu-virt-arm64 和 app/nshell 路径不存在；改为先用 rg --files 定位真实目录。

- user/posix/fd.c 不存在，libc 读取后 shell 未被 && 执行；改用文件清单定位用户态 FD 桥接。

- 随后猜测的 user/posix/posix_fd.c 同样不存在；文件清单确认桥接实际位于 user/libnos/posix_fd.c。
- fs/devfs 不存在，当前未发现 tty/termios 层。

- ARM64 配置成功，但当前 aarch64-elf 工具链编译在 string.h 缺失处失败；保留环境失败事实，不扩展为 ARM64 调试任务。

- include/kernel/notification.h 不存在；类型实际在 include/nos/ipc.h。

## 用户要求实施后的状态

- [complete] STM32 共用 UART、可配置日志队列与 raw/可选 canonical TTY。
- [complete] 四板标准构建、三板 DMA 编译、主机回归和 QEMU STM32 冒烟验证。
- [partial] ARM64 PL011 轮询接口及独立目标文件；完整固件受目标 C 库和平台启动/调度基础缺口阻断。
- [pending] 实体板 DMA、应用负载及中断栈高水位验证；完整 POSIX TTY 和日志留存/多后端。
