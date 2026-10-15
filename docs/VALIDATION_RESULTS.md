# 本轮验证结果

<!-- Copyright (C) 2026 NOS contributors -->

最终完整软件验收：**50/50 个命令组通过，66 次真实 QEMU 启动**，runner 退出 0。汇总为 `out/microkernel/acceptance/1792024151409000896/results.json`，每组日志保留在同目录。报告要求 completed 等于 total；以下是结束后的结果，不是运行中状态。

## 基础平台与真实 C 库

| 平台 | 运行库 | 来宾检查数 | QEMU 退出 | 结果 |
|---|---|---:|---:|---|
| ARM Cortex-A15 / MMU | minimal | 92 | 0 | 通过 |
| ARM Cortex-A15 / MMU | newlib | 109 | 0 | 通过 |
| ARM64 Cortex-A53 / MMU | minimal | 95 | 0 | 通过 |
| ARM64 Cortex-A53 / MMU | musl | 114 | 0 | 通过 |
| RV64GC / Sv39 | minimal | 96 | 0 | 通过 |
| RV64GC / Sv39 | musl | 115 | 0 | 通过 |
| STM32F405 netduinoplus2 / 非 MMU | minimal | 84 | 0 | 通过 |
| STM32F405 netduinoplus2 / 非 MMU | newlib | 100 | 0 | 通过 |

基础矩阵合计 805 项来宾检查，其中包含不同架构/运行库重复验证，不能解读为同等数量的独立功能。基础报告 ID 为 `1792024151437049681`；永久日志与配置保留在 `out/microkernel/reports/1792024151437049681/`。非 MMU 验证共享内存和非特权契约，没有把未执行的 MMU 隔离测试计为通过。

## 完整验收

| 类别 | 命令组 | 结果与范围 |
|---|---:|---|
| 基础矩阵 | 1 | 八组合、1000 轮资源与端点生命周期、F01–F11 相关错误路径、线程/C 库/ELF/内存 |
| 构建与宿主 VFS | 2 | 无变化增量、参数重编、静态库重链接；真实 VFS 处理函数 ASAN/UBSAN 与 100000 次有界随机输入 |
| 正常启动 | 4 | 三主架构 UART 输入与连续两次 VFS/UART 重启后资源相同；STM32 为持续 init/ELF 启动，uart_input_tested=false |
| 持久化 | 13 | 三主架构 native/FAT × legacy/modern 的 12 项及 native 损坏回退，共 26 次启动；真实 virtio IRQ，无轮询替代计数 |
| SMP | 12 | 三主架构 × minimal/真实库 × 2/4 CPU；真实用户并行、锁、TLS、共享 stdio、TLB shootdown、宿主经过时间 |
| STM32 MPU | 2 | minimal/Newlib 实际内核访问、只读写入、NX 故障；不是完整 MMU 地址空间 |
| RV32 | 2 | Sv32 与非 MMU minimal，各自 ELF、宽度/指针/浮点上下文与错误路径 |
| 64 位时间 | 4 | ARM、STM32、RV32 两模式从 0x180000000 tick 启动；高位快照、旧 API 边界、睡眠进展 |
| 特权异常注入 | 6 | 六平台精确匹配 privileged_exception、指定 FAIL 与预期退出 1；这是预期失败验证通过，不是正常镜像失败 |
| 应用 | 4 | 两平台实际库构建 + ARM64/RV64 musl 独立 Lua/LVGL、ELF TLS；LVGL 为软件帧缓冲 |

musl 验收包含负值 futex 锁字、原子 REQUEUE/CMP_REQUEUE 的两等待者换队、三个真实条件变量等待者广播及 join。只读 futex 保持 READ 访问契约；clear_tid/DMA 保留 WRITE 检查。ELF 包含 file-backed 入口、RW 重定位目标、动态表/重定位表文件边界和终止检查，以及真实只读段执行故障。

## 修复与证据身份

首轮完整执行记录 `1792023843309662736` 为 49/50：唯一失败是生成器内容未变仍重写 initramfs C，造成不必要的 initramfs.o 重编。修复为内容相同不写入，保留原严格构建断言；单独 F07 和本轮完整验收均通过。旧失败记录保留，没有覆盖或改写为成功。

最终 23 份矩阵子报告的源码身份一致，全部 `source_changed_during_run=false`；镜像 SHA256、实际配置、C 库归档 SHA256、工具版本及逐项检查保留在各自不可覆盖的报告目录。HEAD 为 `e1b263086ff109690fb7abece8a414baa5cb7470`，本轮修改仍在工作区，报告另外记录 tracked diff 与所有未跟踪源码的哈希；不能只用该旧 HEAD 重现本轮源代码。

原版 QEMU 10.0.13；ARM GCC 14.2.1，ARM64/RV64 GCC 14.2.0；Newlib 4.5.0 与 musl 1.2.5 的实际源码端口。环境安装脚本已完整重跑退出 0，日志 `/workspace/.nos-env/logs/final-setup.log`；未绕过包签名、源码 SHA256 或 TLS。当前实例可开发/构建/运行，不代表新任务的快照恢复已经另行验证。

最终镜像的静态内核直接分支边界检查单列在验收目录 `*-kernel-boundaries.json`；这只检查可解析直接分支，不是间接调用的形式化证明。

## 未执行、条件交付与支持边界

- 真实 STM32：没有物理串口设备。验收目录 `hardware-stm32/results.json` 为 `executed=false`，未执行物理板测试；不能把 QEMU 通过计为硬件通过。
- 远端 CI：Git 仓库读取成功，但 Actions API 返回 Forbidden；GitHub/GitLab 配置已提供，尚无远端运行成功记录。云环境草稿已添加 api.github.com，需设置应用后重试。
- 板级 USB/显示/输入驱动仍在 legacy，尚未迁移为新设备服务；Lua/LVGL 应用和 FatFs 移植不能替代这些工作。
- owner-death 分配器失效注入、dtoa 回收前后精确堆计数未做专项验收。已验证正常分配/浮点格式化、线程退出及共享 stdio，不扩大此结论。
- 无 IOMMU；设备服务为可信 I/O 域。非 MMU/MPU、固定容量资源和粗粒度内核锁的边界见 [架构说明](MICROKERNEL.md)。
- glibc、动态链接、完整 POSIX/信号/fork、网络、文件映射、按需分页和交换尚不支持；它们是后续兼容目标。[支持范围表](POSIX_SUPPORT.md) 逐项列出接口限制。

本报告完成软件主线验收，原计划的真实板与远端 CI 外部条件仍未满足。阶段清单见 [EXECUTION_PLAN.md](EXECUTION_PLAN.md)，重跑方式见 [QEMU_VALIDATION.md](QEMU_VALIDATION.md)。
