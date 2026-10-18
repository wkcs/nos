# 本轮验证结果

<!-- Copyright (C) 2026 NOS contributors -->

下文的 **50/50 个命令组通过、66 次真实 QEMU 启动** 是阶段 0–5 的历史完整软件验收，runner 退出 0。汇总为 `out/microkernel/acceptance/1792024151409000896/results.json`。后续 glibc/POSIX 开发修改了公共核心，不能以这份旧报告代替新快照的完整回归。

## 进行中的 POSIX.1-2017 / glibc 验证

目标为 ARM、ARM64、RV64 MMU 同步推进。已安装并审计真实 Debian glibc 2.41 与原始 ld.so，新增严格的 `qemu_glibc.py`、`qemu_posix_vfs.py`、`qemu_posix_suite.py` 和共享文件 runner；这些报告均明确 `full_posix_conformance=false`。动态库导出符号、程序可链接或少量独立用例通过不构成完整 POSIX 结论。

开发阶段的真实 QEMU 证据包括：ARM SMP2 glibc 报告 `1792082876583639855`、RV64 SMP2 `1792082906063751110`、ARM64 SMP4 `1792082939367445911`，均退出 0，包含 NPTL、信号、poll 和进程程序。原生目录与文件元数据的三架构报告为 `1792083055916836455`。这些是对应源码快照的局部开发验证，新增共享页、exec 参数预算和 RTC 端口必须另行验收。

首轮未修改的上游 OpenPOSIX 27 项在 ARM64 上 17 项通过；针对其余 10 项修复后的复验为 5 项通过、5 项未解决。未解决项暴露了命名信号量、MAP_SHARED 和 RAM 文件容量等真实缺口，保留原始记录。新增共享页对象、授权租约、文件后端与测试正在验证，不能把尚未执行的新用例计入通过数。后续完整快照报告将在集成完成后记录。

共享页实现后的扩展库存为 51 项，三架构各 **51/51 通过、QEMU 退出 0**：ARM `1792110806108988517`、ARM64 `1792110806221947150`、RV64 `1792110806116959945`。没有将未解决、不支持或超时计为通过。正式共享文件三架构报告为 `1792110722208431794`；文件 API 的三架构 focused 报告为 `1792110535215496399`。exec 的实际 128 KiB 预算、2050 个短参数及 guard 边界复制在三架构报告 `1792110930137089037` 中全部通过。这些均为 PATH_MAX256/bulk IPC 开发前的证据，不能替代后续新快照的最终冻结验收。

新增路径与安全 IPC 集成后的首轮完整执行为 `out/microkernel/acceptance/1792131234783825953/results.json`：**68/68 组完成，66 组通过、2 组失败，source_changed_during_run=false**。失败均为 ARM glibc 的单核/四核进程组信号程序，其余三架构 OpenPOSIX 扩展用例、原有基线、持久化、SMP、STM32 MPU、RV32 及应用组均通过。GDB 确认 ARM glibc 的 kill 汇编在 SVC 时保存合法的 4 字节对齐 SP，内核 sigreturn 原错误要求 8 字节对齐，导致 SIGSEGV。修复限于 ARM 恢复点的字对齐检查，线程函数入口仍要求 AAPCS 对齐，PC/权限/状态检查保留。后续修复验证及新冻结快照必须使用新的报告，不能覆盖本轮失败。

修复后的完整冻结验收为 `out/microkernel/acceptance/1792158919951824361/results.json`：**68/68 组通过，runner 退出 0，`source_changed_during_run=false`，开始与结束源码身份完全一致**。执行命令为 `python3 tests/acceptance.py --include-posix --require-frozen-source`。三种 MMU 架构均通过真实 glibc 的单核/四核完整程序组；51 项未修改的上游 OpenPOSIX 选定用例在各架构全部通过，共 153 项。原有八组合、持久化、12 组 SMP、STM32 MPU、RV32 MMU/非 MMU、64 位时间边界、精确异常注入、宿主 VFS sanitizers 和 Lua/LVGL 同时通过。

该快照包含共享文件/命名信号量、128 KiB exec 参数预算、256 字节路径与受 token 约束的 bulk IPC。验收时基线 HEAD 为 `4d3a98a1a3f09e289c7f1fbe3ea868a0e0b72eee`，工作区 tracked diff SHA256 为 `4ebe9851a5ec8e395f84450b58a89e49536f627ec7e8c56a8da764e0e4272567`；提交前已逐项确认源码仍与报告一致，随后仅更新本报告及执行计划。文件记录锁、真实凭据/权限、完整资源限额、TTY/作业控制、网络等仍待实现，不能由这 68 组或 153 项选定用例推断完整 POSIX.1-2017 已完成。

## 记录锁后续批次：开发证据与待统一验收

在已提交的 `a9c32bf59ea327355cd40fdab3202844e7bfb992` 基础上，新增 Linux profile 进程记录锁、ARM old/wide flock 转换、SA_RESTART、EDEADLK 和取消/退出清理，以及完整 generation 的组存活查询和排队 fork FD 引用保留。默认 native 配置明确 ENOTSUP，不扩大为两种原生 libc 的完整锁支持。

记录锁主程序的三 MMU SMP2 开发报告为 ARM `1792284522878828806`、ARM64 `1792284426033659694`、RV64 `1792284522874694409`，均退出 0；独立生命周期程序七项在 ARM64/RV64 报告 `1792284395777839377` 和 ARM 修复后 `1792284482165272694` 全部通过。ARM 首轮 signed/unsigned 编译错误保留，修复没有降低 Werror。组存活查询与槽复用回归报告 `1792284731630769733` 三架构分别通过 120/119/121 项、退出 0；该轮中途新增 fork-lease 路径，因此仅 RV64 镜像包含那条新增路径，不能将此报告冒称同一最终源码快照。

宿主真实 VFS/header 的记录锁、坏回复事务、取消、满锁表回滚、丢失退出通知、fork pin、FD 表容量及最后 fork ACK 后 pipe EOF 共 **15 项 ASan/UBSan 通过**，报告 `out/microkernel/host-record-locks/1792284780621381021/results.json`。其 IPC 为模拟传输，不能替代内核/QEMU 证明。默认 A64 musl 用户模板仍为 24504/24576 字节，ARM 兼容 profile 的最新完整链接通过。

这些是开发过程中的证据。最终资源基线与真实单核 fork 引用压力集成后，必须重新执行 `python3 tests/acceptance.py --include-posix --require-frozen-source`，冻结全部源码、文档和构建输入，并另存完整报告；新批次未完成该流程前不能使用先前 68/68 报告代替验收。

新增记录锁批次的第一次完整冻结执行 `1792285136162409783` 已完成全部 68 组，**65 组通过，3 组失败，source_changed_during_run=false**。失败均为三 MMU 的 glibc 完整四核组达到 180 秒宿主上限；单核、上游 153 项、原生平台及其余检查通过。这份结果保留为失败，不与较早已提交快照的 68/68 混淆。

对同一失败 A64 ELF 校验 SHA 后，以 420 秒诊断窗口连续运行，逐行时间戳保存在 `out/microkernel/glibc420-diagnostics/record-locks-serial/arm64_virt/`；**333.10 秒自然退出 1**，真实 stdio/进程/主记录锁/NPTL 等继续推进，但独立生命周期程序的 exec 握手失败。这仅证明原 180 秒没有覆盖整个新工作量，不能据此宣称延长窗口后通过。新的批量组查询优化减少每次 VFS 回收的陷入次数，保留回收频率与全部安全规则；应用正在拆分错误点，未放宽原有语义断言与等待条件。该优化的最终统一验收需另存新报告。

批量快照实装后的 A64 四核机制/主记录锁 smoke `1792304823096638051` 通过 122 项；生命周期专项 `1792304957670286712` 通过 116 项、退出 0、七项标记完整，仍使用原 10 秒握手等待，运行期间源码未变化。宿主真实服务的批量错误保留测试加入后，16 项 ASan/UBSan 在 `1792304788189631468` 全部通过；默认 A64 musl 模板仍为 24504/24576 字节。

保留原 180 秒预算测量的优化后 A64 完整组 `1792305051583998020` 仍按时限失败，155 项检查已完成、没有局部 FAIL：文件组在 163.40 秒、路径组在 173.30 秒通过，180.00 秒宿主终止。它不能计为完整通过。相对优化前的同镜像阶段记录，文件组从 235.57 秒降至 163.40 秒，路径组从 249.07 秒降至 173.30 秒；这是本环境的软件模拟耗时，不能泛化为硬件性能结论。

完整 glibc 组包含新增辅助进程、记录锁程序和额外 exec 子场景，因此将该组默认宿主总预算设为 420 秒；focused 应用和原生专项仍为 180 秒，显式正数 `--timeout` 优先。报告记录 `timeout_seconds` 与 `timeout_policy`。全部必需标记、实际退出码、RTC、CPU mask、SMP 时钟以及应用内原有 10 秒等待继续检查；新的完整冻结结果必须另行产生，不把任何旧超时或生命周期失败改写为成功。

第二次完整冻结记录锁验收 `1792305342920623360` 执行全部 68 组，**67 组通过、1 组失败，source_changed_during_run=false，开始与结束身份一致**。A64 四核组报告 `1792306373408971900` 在 420 秒内自然退出 1：具体失败为 `exec_preserving_replace_ready errno=110`，即生命周期程序内部 10 秒 exec 握手期限。ARM/RV64 的完整单核/四核和三架构 153 项选定上游用例均通过。这份真实失败保留，不能算成完整验收通过；旧工具会话失效，没有另行补造外层 shell 退出收据。

文件映射加载原每次只通过 64 字节 pread RPC 取内容。最小修复将 Linux profile 的大定位读改为现有 512 字节 bulk 中转，不扩大内核上限：整范围预检、OFD 偏移不变、token STORE、成功 RPC 后发布当前块的规则保留；默认配置和 pwrite 不变。新增真实 glibc 的跨页、只读/PROT_NONE guard、EOF/canary、共享 OFD 偏移及归档/inline 内容等价检查。A64 四核文件专项 `1792332436126709452` **115 项通过**；紧接完整四核组 `1792332465350698763` **190 项通过、退出 0**，持久化收据 `out/microkernel/pread512-validation/full-exit.json` 记录实际命令退出 0、源码未变化、总用时 73.68 秒。原应用内 10 秒等待未改。此专项证据尚不能替代后续新快照的完整 68 组。

退出回收审查同时补齐一条防御路径：VFS 的组查询现在按 IPC 保存的完整进程 generation 清除该组所有 pending I/O，而非仅记录锁等待；排队 fork 仍只保留用户 FD 表，存活组内其他线程不触发全组清理。当前内核另有按槽保留最新 token 的取消通知，任务退出会通知已经交付的请求；因此不能将宿主注入同时遗漏通知的情形声称为已证明现内核必现的漏 pin。宿主真实服务的 **23 项 ASan/UBSan** 在 `1792332575283168274` 全部通过，新增 lost-reader/EPIPE、lost-writer/保留数据与 EOF、同槽不同 generation 和存活线程隔离；它仍是 mock IPC，真实内核生命周期回归及最终冻结验收另行记录。

新增单核真实生命周期回归 `pipe_pending_owner_exit_slot_reuse`：优先级 0 的写进程填满管道后发布 armed，再直接阻塞写；优先级 2 的监督者能继续执行，证明写进程及其捐赠优先级的 VFS 已阻塞。随后终止写进程、无 VFS 调用地复用相同槽 13 代，再让读进程验证原数据、取消字节未出现及 EOF，并严格恢复任务、端点和页数基线。该顺序证明只在单核要求；SMP 不计此标记。它验证现内核取消通知与 VFS 回收的组合，不将宿主遗漏双通知的注入等同真实必现故障。真实执行结果将由以下新冻结报告补记。

新冻结队列的三架构单核文件专项均严格通过：A64 `1792333719778569822`（113 项）、ARM `1792333741209308794`（112 项）、RV64 `1792333760474717176`（114 项），退出 0、源码身份稳定，均包含确定性管道生命周期标记。随后完整验收 `1792333776038247785` 在 ARM 四核组自然达到 420 秒截止后停止：**只完成 9/68 组，8 组通过、1 组失败、source_changed_during_run=false**。不能算作完整 68 组。其 glibc 报告 `1792333996967226783` 保留超时及缺少 `ipc_bulk_cancel_revocation` 的原始结果；之后有序终止验收，持久收据记录验收子进程实际退出 -15、外层队列退出 1。

只读审查找到原 bulk 回归等待 clear_tid 的真实丢唤醒竞态：原子读取非零后，再普通读取一次作为 FUTEX_WAIT expected；若线程恰在两次读取间清零并唤醒，第二次取到 0 会永久等待已错过的唤醒。失败 ARM ELF 反汇编确有两次独立加载，未被编译器合并；本次没有在途 GDB 任务状态证据，因此不将其冒称已直接观察到 TASK_FUTEX/expected0。最小修复仅保存一次原子读取的非零值并以该值等待；内核等待零值的合法 futex 语义不变。其它 clear_tid/join 循环审查未发现同类双读。修复后需新的 ARM 四核机制验证及完整冻结验收，不修改旧超时记录或放宽任何期限。

## 记录锁批次最终冻结验收

最终报告 `out/microkernel/acceptance/1792334677968073996/results.json`：**68/68 组通过，complete=true，验收进程及外层队列均实际退出 0，source_changed_during_run=false，开始/结束源码身份一致**。命令为 `python3 tests/acceptance.py --include-posix --require-frozen-source`；持久化退出收据位于 `out/microkernel/bulk-futex-fix-queue/1792334650513119460/full68-exit.json`。在修改最终文档前，已独立确认当前 git_identity 完全等于报告 source_after，保存全部输入哈希；随后仅更新本报告和执行计划，代码与测试输入保持一致。

三 MMU 架构各 51/51 上游选定用例通过，共 153 项；六组真实 glibc 单核/四核均严格通过、退出 0，保留全部生命周期标记及内部 10 秒期限。六组命令含构建用时依次为 ARM 33.84/59.50 秒、A64 36.29/68.48 秒、RV64 28.51/46.44 秒，不将其当作纯来宾性能。原有八组合、持久化、原生 SMP、stock QEMU `netduinoplus2` 的 STM32/MPU、RV32 MMU/非 MMU、64 位时间、精确异常注入和 Lua/LVGL 同时通过。宿主 VFS 包含 100000 次有界输入及 23 项实际服务 ASan/UBSan 边界检查；mock IPC 的范围仍单独说明。

该冻结快照以 `a9c32bf59ea327355cd40fdab3202844e7bfb992` 为基线，tracked diff SHA256 为 `d209de1dc87edea6f807c7345e278a6ec1dd833d0911325d06df21eea2ed2072`，新增文件 SHA256 与各 ELF 摘要保存在报告。ARM 四核最小修复专项 `1792334650648758540` 先通过 114 项、退出 0；实际新 ELF 反汇编确认同一次非零读取值用于比较与等待，旧独立第二次加载已消除。所有旧超时、生命周期失败和部分中断报告保留，未改写为通过。

本批完成进程记录锁、generation/fork 引用回收和安全 512 字节定位读，**不代表完整 POSIX.1-2017 已完成**。真实凭据/权限、资源容量和限额、路径/pipe 与 libc 查询一致性、TTY/作业控制、网络及文件映射边界仍按支持范围表继续开发；非 MMU 限制、真实硬件和远端 CI 的未执行边界不变。

## 阶段 0–5 历史验收

以下报告要求 completed 等于 total，结果均取自该轮结束后的记录。

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
- 本历史验收未覆盖 glibc、动态链接和 Linux personality。当前新增实现与剩余完整 POSIX、网络、TTY、按需分页及交换缺口以 [支持范围表](POSIX_SUPPORT.md) 和新的兼容性报告为准。

本报告完成软件主线验收，原计划的真实板与远端 CI 外部条件仍未满足。阶段清单见 [EXECUTION_PLAN.md](EXECUTION_PLAN.md)，重跑方式见 [QEMU_VALIDATION.md](QEMU_VALIDATION.md)。
