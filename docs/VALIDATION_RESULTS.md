# 本轮验证结果

<!-- Copyright (C) 2026 NOS contributors -->

## ESP32 可选磨损均衡：2026-10-26

干净冻结0399da6的最终raw/WL四组专项及两个WL普通生态启动通过，共42次成功启动；四份WL回归各128次提交、221个物理数据/备用块参与4736次擦除。首轮S3 raw TLS/清理断言、C3 raw生态软件定时器失败保留为失败；相同源码/ELF/期限串行复验通过，根因未定，不计一次并行全绿。未重跑公共89组；[范围、原始证据与固件](ESP32_WEAR_VALIDATION.md)。

## ESP32 原生Flash持久化：2026-10-25

干净冻结提交 `7577e74` 通过C3/S3共22次真实QEMU启动；各七阶段持久化/损坏回退、运行时29/生态31标记、四份普通固件各32项UART交互均通过。公共驱动框架和NOR实际适配器故障模型的ASan/UBSan/LSan通过，五个runner退出0、源码身份一致。本批未改公共核心，未重跑89组，不扩大实板/Flash耐久/POSIX结论。[完整范围、固件与日志](ESP32_STORAGE_VALIDATION.md)。

## ESP32 Newlib 文件/标准流适配：2026-10-25

干净冻结提交 `106338b` 通过C3/S3各四个实际镜像的构建/审计与真实QEMU；四份普通固件各32项UART交互、运行时28/生态30标记齐全，最终runner退出0。Newlib真实stdio、SDK FD并存、任务所有权/退出回收及全部任务槽容量通过。此批未改公共核心，未重跑89组；旧完整基线单独保留。详细范围、首次辅助检查失败与复验、固件和日志见 [ESP32_NEWLIB_VALIDATION.md](ESP32_NEWLIB_VALIDATION.md)。

## ESP32 原生VFS与公共完整验收：2026-10-25

干净冻结提交 `bfd67ab` 通过公共完整软件验收 **89/89组**，complete=true、全部returncode=0、源码未变化；报告 `out/microkernel/acceptance/1792907005224629555/results.json`。C3/S3各四个实际镜像启动通过，四个正常固件各32项UART交互、运行时23/生态25个必需标记齐全，五个runner实际退出0。原始日志、正常固件和S3内存修复见 [ESP32_VFS_VALIDATION.md](ESP32_VFS_VALIDATION.md)。这不是完整POSIX认证或新增实板结论。

## ESP32 启动接管：2026-10-25

干净提交 `76a8ec8` 通过 C3/S3 各三种镜像的实际构建、入口/符号审计和真实 QEMU；正常启动各15项 UART nshell 交互、运行时19/生态21个标记齐全，全部退出0。仓库依赖离线完整性9组通过。此处仅为ESP专项，未新增完整POSIX或实板结论。原始报告、日志和固件见 [ESP32_BOOT_VALIDATION.md](ESP32_BOOT_VALIDATION.md)。

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

## 管道 4096 / IOV1024 批次：开发证据

以已推送 `f1211025a7dc7d297f1566ec665a03e06f5829c0` 为基线，Linux profile 实装 PIPE_BUF/pipe/BULK4096、IOV1024，以及原始大请求的非阻塞前缀、pipe 最终读 span 与部分写后 SIGPIPE 通知；默认 native 容量不扩大。原记录锁、batch generation/fork 查询、PREAD512 和 clear_tid 单快照修复保留。

combined host `out/microkernel/host-vfs/1792365648160113008/results.json` 实际退出 0：100000 次有界输入、原 23 项记录锁/定位读/退出回收及新增 13 项 pipe ASan/UBSan 检查通过，后两项报告为 `1792365650351075573`、`1792365653142410098`。IPC、权限和回复仍为 host 模拟，不能冒充真实内核验证。

新独立 glibc 应用在 GNU/Linux host 与 UBSan 共 11 项通过，ARM/A64/RV64 实际 glibc ELF 以 Werror 编译链接通过，尚未作为 NOS guest 成功证据。服务和 runtime/gateway 的三架构对象编译通过；实测编译栈链保守上限分别为 4928/5184/5248 字节，低于 8192，readv 为 792/992/1056。报告及源码哈希保存在 `out/pipe-atomic-objects/`。默认 A64 musl 完整链接仍为 24504/24576 字节模板。

内核新增 4 项实际来宾断言，覆盖 4096 字节未对齐跨页、非相邻物理 backing、FETCH/STORE canary、RO/PROT_NONE/hole、CALL 后权限变更/撤权、4097/溢出/零边界及精确资源回归；它们当前只有编译证据。三架构单核/四核 focused 与新完整冻结 68 组必须另行执行，未执行前不得使用前批报告代替。

## 管道 4096 / IOV1024 批次最终冻结验收

最终报告 `out/microkernel/acceptance/1792365892261153665/results.json`：**68/68 组通过、complete=true、source_changed_during_run=false，开始与结束身份一致**。完整验收进程、六组 focused 与外层队列收据均实际退出 0，位于 `out/microkernel/pipe4096-iov1024-queue/1792365748254822931/`；命令仍为 `python3 tests/acceptance.py --include-posix --require-frozen-source`，没有放宽已有断言或期限。

ARM/A64/RV64 单核及四核新管道 focused 全部通过；随后六组完整 glibc 分别通过 ARM 204/207、A64 204/207、RV64 205/208 项检查，缺失标记为空、退出 0。各组同时验证全部 11 项 pipe 程序标记（含 scalar/writev 部分写后 SIGPIPE、原子性、NONBLOCK 前缀、IOV1024/guard）及 4 项新增跨页 IPC 检查。三架构各 51/51 上游选定用例、原有八平台、持久化、SMP、stock QEMU STM32/MPU、RV32、时钟、异常与应用组全部通过。宿主组合保持 100000 次输入、23 项记录锁/回收及 13 项 pipe ASan/UBSan 检查。

冻结基线 HEAD 为 `f1211025a7dc7d297f1566ec665a03e06f5829c0`，tracked diff SHA256 为 `e65530ca4c3db869c8a03ec0bc8722f6ee03a7bd846244a788089fcd03b55b1b`，新增源文件和各镜像摘要保存在报告。收尾前已独立确认当前 git_identity 完全等于 source_after，并保存全部输入哈希至 `out/microkernel/final-pipe4096-checkpoint/`；之后仅更新支持范围、接口表、执行计划和本报告。代码与测试输入仍为已验证快照。

本批完成 Linux profile 的 PIPE_BUF4096/IOV1024 一致性与上述管道边界，不将默认 native 容量、未实现的真实凭据/权限、资源最小容量、TTY/网络、映射边界或所有 POSIX 接口算作完成。下一批继续稳定进程凭据、可信 IPC subject 与文件权限；本报告不声称完整 POSIX.1-2017 符合性。

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


## 在开发：凭据、权限与信号入睡竞争

本批尚未完成最终冻结验收。首轮 focused `1792385832377068361` 保留新测试的 argv 约定和 RECV 授权竞态失败；修正测试后，ARM64 单核 `1792385966911225539` 全部 130 项通过、实际退出 0。四核 `1792385990626316289` 和带阶段诊断的 `1792386169813760194` 则自然退出 1：真实 NPTL setxid 子场景触发原有 10 秒期限，所有 7 项 native 凭据/subject 检查及凭据程序此前阶段通过；不把这些报告算作完整或四核通过。

旧 ELF 的逐次 futex 断点与仅 deadline 硬断点两次诊断均自然通过而未命中，不据此否认竞争。非停止式 QMP 抽样 `out/microkernel/signal-wait-diagnostic/1792416765000623096/` 首次确认相隔 151 毫秒的同一线程同时处于无超时 TASK_FUTEX、signal_pending=1<<32、mask=0、signal_depth=0。此次 GDB 因 EL0 地址空间不能读取内核地址而退出 1，来宾仍自然失败 1；完整原始结果保留。

随后 `1792416853295541826` 再次实时命中，并用 QEMU 物理内存读取模式成功抓取 GDB 状态（调试器退出 0、来宾自然退出 1）：进程 567 的主线程等待真实 xid_command（syscall149/setresgid、cntr=1、error=0）；线程 29 在 barrier 的 FUTEX_WAIT 无限等待，实际值与 expected 均为 18，同时挂起未屏蔽 signal33、深度 0。队列中真实 si_code=-6、si_pid=567、si_uid=70001，排除伪造 sender 或 NPTL 校验不通过。这些是旧镜像机制诊断，不是新源码验收。

修复以通用阻塞入队屏障覆盖该窗口，并新增真实单发线程信号与 futex 竞争回归，保留 NPTL 场景 10 秒期限。新完整矩阵与最终结果待后续记录；完整 POSIX.1-2017 仍未完成。

信号修复后的开发队列 `1792417006892298880` 中，ARM64 凭据四核/单核、信号四核、权限单核/四核依次通过 133/130/129/132/135 项，均实际退出 0；宿主 100000 次状态检查与 12+23+13 项 sanitizer 同时通过。随后 ARM 四核 `1792417196846351887` 在 180 秒自然截止，已通过 93 项，停于新 native expire 测试，尚未进入 glibc；队列依策略停止，不能算作全部专项通过。测试将以 `(endpoint << 2) | mode` 传递参数，端点基值为 0x40000000，ARM32 左移丢失高位，子任务调用错误端点而父任务等待 RECV；修正为独立入口传递完整句柄，不改变失效断言或超时期限。


完整句柄修正后，队列 `1792417547951277121` 的 ARM 凭据四核/单核、信号四核、权限单核/四核均通过，检查数为 133/130/128/131/133，实际退出均为 0。随后 RV64 四核 `1792417728610211862` 自然退出 1：7 项 native 身份检查和初始 auxv 通过，最大有效 UID/GID 边界失败。真实应用反汇编显示从 getres* 结果以 lw 取得规范 32 位值后直接比较 getuid 返回寄存器；未修改 glibc 的 getter 为 ecall/ret，而 NOS 原返回零扩展。这是 RV64 psABI 的 32 位返回形式适配问题，不能靠移除高位 ID 断言解决；本批为四个无 errno getter 明确规定规范返回，保留内存中的 u32 身份及全部测试。


修正 RV64 getter 后，冻结开发队列 `1792418107545737192` 的宿主检查和三架构 15 组凭据/信号/权限专项均实际退出 0，身份稳定。随后完整验收 `1792418605907030763` **只完成 5/68 组，其中 4 组通过、1 组失败**：ARM 长路径报告 `1792418697526040142` 实际退出 1，缺少 `vfs_long_path_pair_cwd_resolve`。有序停止记录验收子进程 -15、外层退出 1；下一组中断不计完成。这不是完整矩阵通过。

失败原因是普通路径解析被加入末项执行检查。现已将通用解析与 NOS_RESOLVE_EXEC 明确区分，并保留两者的祖先搜索检查；新宿主权限报告 `1792418981232267365` 的 13 项 ASan/UBSan 检查全部通过，包含长路径、符号链接、硬链接和执行标志。另行审查修正普通 exec 的保存身份：成功时 SUID/SGID=EUID/EGID，失败保持全部旧凭据。真实 glibc 测试增加 leader/非 leader、有效 root/非 root、丢弃旧保存身份后不能恢复的检查。两处修复后须重新冻结并执行专项及完整 68 组；前述旧快照通过不能替代它们。


## 凭据与 VFS 权限：最终冻结验收通过

协调队列 `out/microkernel/credentials-final-queue/1792419321026678809/results.json` 完整结束，三架构长路径、宿主检查、15 组真实 glibc 专项及完整验收依次通过，各层退出收据均为 0。新专项队列为 `1792419366878287212`；宿主 `1792419366999168207` 包含 100000 次有界输入、23 项记录锁、13 项管道和 13 项权限 ASan/UBSan 检查。宿主 RPC 模拟不计作真实 IPC 验证。

完整验收 `out/microkernel/acceptance/1792419840208812446/results.json` **完成 68/68 组，全部通过，实际退出 0，source_changed_during_run=false**。六个完整 glibc 单核/四核组检查数分别为 ARM 241/243、ARM64 240/243、RV64 241/244，缺失标记均为空；包括保存 ID 更新与失败回滚、NPTL setxid、可信 IPC subject 生命周期、RAM DAC 及单发信号/futex 入睡竞争。原有八平台、非 MMU、stock QEMU netduinoplus2 STM32/MPU、持久化、选定上游用例等均在本轮矩阵内。

测试时 HEAD 为 `ccd5660f04db8a5443b894465f636761174b2bed`，tracked diff SHA256 为 `176e635795ec96f8911900ba6a54ccb22e81d48770d16b2e861eef78a60b32d5`；新文件逐一 SHA256 与各层报告一致。独立复核的证据为 `out/microkernel/credentials-final-evidence.json`，提交前完整文件哈希为 `out/microkernel/credentials-tested-inputs.json`。复核后仅更新结果文档，非文档输入保持测试快照不变。

本批完成所列凭据/权限子集及实际发现的竞争修复，**完整 POSIX.1-2017 仍未完成**。资源限额执行和最小容量、4096 字节路径、TTY/作业控制、网络、映射边界等继续开发；set-ID ELF、fsuid/fsgid、完整持久化所有权、纳秒时间接口及真实硬件/远端 CI 仍不能由本次软件验收推断支持。


## 进程共享 NOFILE：最终冻结验收通过

实现进程共享、fork 复制和 exec 保留的 NOFILE 限额，补齐真实 ARM ugetrlimit191 查询。VFS 通过 IPC_LIMITS70 查询活动请求的实时可信限额，实际限制 open/dup/pipe 的新 FD 编号；旧高 FD、失败 dup2 的目标与记录锁、pipe 双 FD 原子失败均有回归。普通 dup 与 F_DUPFD 的 soft=0 错误区分，以及负 dup2/dup3 目标的桥接歧义已修复。

冻结队列 `out/microkernel/resources-final-queue/1792546443792975770/results.json` 全部完成，宿主检查、六个专项和完整验收各层实际退出均为 0。宿主 `1792546444013824914` 包括 100000 次有界输入及原有 23 项记录锁、13 项管道、13 项权限和新增 6 项资源分配 ASan/UBSan 检查；模拟 IPC/限额不冒充内核验证。六个真实 glibc 资源专项的单核/四核检查数为 ARM 134/137、ARM64 134/137、RV64 135/137，包含 12 项应用标记、ARM 额外191标记和4项 IPC 限额查询授权/输出预检/生命周期/清理标记，缺失均为空。

完整验收 `out/microkernel/acceptance/1792546638466611109/results.json` **完成 68/68 组，全部通过，实际退出 0，source_changed_during_run=false**。六组完整 glibc 单核/四核分别通过 ARM 259/262、ARM64 257/260、RV64 258/261 项检查，实际退出0、无缺失标记。原有八平台、真实未修改 glibc、选定上游用例、SMP、持久化、stock netduinoplus2 STM32/MPU、RV32 非 MMU、时钟/异常及应用组均在矩阵内。

测试时 HEAD 为 `98a0775c5e4a92885eb1e8ca603151d9b0ebc257`，tracked diff SHA256 为 `1d5ae760e6f026b2d7699b58d5aa2c73a6c17e0ddb80db2017895eeba5d4d262`。独立复核 `out/microkernel/resources-final-evidence.json` 核对每层真实退出、全部必需标记与开始/结束源码身份，并保存全部输入文件哈希；复核后仅更新结果文档，非文档输入保持测试快照不变。

该批只完成 NOFILE 的声明子集。结构 FD 上限仍为32，STACK 仅查询/相同值；完整 AS/NPROC/CPU/FSIZE 等限制、POSIX 最小线程/进程容量、4096 字节路径、TTY/作业控制、网络及映射边界仍未完成。prlimit 坏旧输出先预检再提交是已测试的 NOS 事务承诺，不宣称其错误路径与上游 Linux 顺序完全相同。**完整 POSIX.1-2017 仍未完成**；真实硬件与远端 CI 的未执行边界不变。


## 开发中：PATH4096

VFS对象/镜像开发构建 `1792567542752686627` 的三架构profile与default6行均退出0，源哈希稳定。IPC内核12对象、native vector6对象、运行库6行以及新应用3架构对象均严格编译通过；单frame、嵌套路径调用、模板与独立映射预算分别报告，不把编译作为实际运行。宿主组合 `1792548897166435367` 全部通过：100000有界输入、23记录锁、13管道、13权限、6资源和新增8项PATH检查；加入实际峰值输出后新PATH宿主 `1792567629679485038` 再次通过，ASan/UBSan退出0、源稳定，scratch峰值36864/65536、总路径映射196608字节。真实三架构QEMU和新完整冻结验收仍待完成，不以旧NOFILE的68组通过替代。


PATH4096首轮冻结队列 `1792567770636266679` 的宿主组合通过，随后 ARM 旧长路径报告 `1792567793925475274` 自然退出2，失败为新 `ipc_vector_authority_lifetime` 与 `ipc_vector_cleanup`，原VFS路径与其它vector标记通过。队列未进入新应用或完整68组，不能算作该矩阵通过。测试错将端点关闭的既有ESRCH期望写成EACCES，并在RECV服务退出已自动回收端点后要求再次关闭成功；现已按既有契约修正，仍要求旧token失效和任务/页/端点精确基线回收，须另存新快照重跑。


第二轮PATH4096队列 `1792567897528841008` 的宿主、三架构旧路径和六个新应用专项均实际退出0。随后完整验收 `1792568130496912259` **只完成8/68组，7通过、ARM完整glibc单核1组失败**，并有序停止（保留实际退出收据）。报告 `1792568357740916704` 发现旧文件/路径应用仍以256字节为上限；同时旧getcwd大capacity、短实际输出紧邻guard的成功断言暴露真实运行库缺陷：将最大capacity声明为直接WRITE span会预检过多不可访问字节。

现将getcwd/readlink先接收到私有有界输出区，再完整预检并复制实际返回字节；保留合法guard边界成功断言，新增实际结果跨guard时EFAULT且输出不部分改变的检查。旧文件应用改为NAME255合法缺失/NAME256拒绝，未终止路径改用完整4096字节边界，短可读前缀则仍应EFAULT。上述修复须另存新冻结快照重跑，旧专项通过不代替新运行库验收。


输出修复后，ARM文件/路径专项分别 `1792568620430607800`、`1792568644758184248` 全部132/128项通过、实际退出0，包含保留及新增的guard断言。三架构profile/default运行库/网关6行严格重编译 `1792568713646503870` 全部退出0；getcwd/readlink frame与保守路径调用链重新核对，仍低于2KiB。须继续新完整冻结队列，不将这些专项计为68组通过。


## RAM PATH4096：最终冻结验收通过

最终队列 `out/microkernel/path4096-final-queue/1792568771233057308/results.json` 完整结束，各层真实退出收据均为0。宿主 `1792568771470843371` 包含100000次有界输入、23项记录锁、13项管道、13项权限、6项资源及8项新增长路径ASan/UBSan检查；临时区峰值36864/65536字节，独立命名空间映射196608字节，scope退出归零。模拟IPC/资源的宿主结果不替代真实内核验证。

三架构旧长路径组全部通过；新glibc应用六个单核/四核专项分别通过 ARM136/139、ARM64 137/140、RV64 138/141 项检查，包含10项长路径应用和5项native vector必需标记、实际退出0、无缺失。

完整验收 `out/microkernel/acceptance/1792569009279952329/results.json` **完成68/68组，全部通过，实际退出0，source_changed_during_run=false**。六个完整glibc单核/四核组分别通过 ARM275/278、ARM64 273/276、RV64 274/277 项，全部退出0、无缺失。此前失败的文件和路径应用、guard输入/实际输出、原有八平台、共享页、选定上游用例、存储/SMP、stock netduinoplus2 STM32/MPU、RV32非MMU、时钟/异常及应用组都在本轮矩阵内。首次native测试失败和第二次8/68部分验收的真实记录保留，不改写为通过。

测试HEAD为 `de3ed1aaaf713abad37aee48e09e4a42a04cc4d5`，tracked diff SHA256为 `0a15a4061c99c8c71a0775e5c8e725dc334400a0be9c378f2ea936443e05dde5`。独立复核 `out/microkernel/path4096-final-evidence.json` 核对每层退出、全部必需标记、开始/结束源身份和所有输入文件哈希；核对后仅更新结果文档，非文档输入保持与验收快照相同。

本批支持有界RAM PATH4096/NAME255、两个独立READ span和稳定cwd引用，未将过渡全路径表示冒充完整inode/dentry命名空间。持久后端64、archive256、已删除目录的完整相对解析、超出有界绝对表示的寻址、完整资源容量/其他限额、TTY/作业控制、网络和映射边界仍未完成。**完整POSIX.1-2017仍未完成**；真实硬件与远端CI的未执行边界不变。


## 地址空间限额与退出生命周期：最终统一验收

完整冻结验收 `out/microkernel/acceptance/1792591938965616587/results.json` 完成 **68/68 全通过**，各组实际退出 0、开始/结束源码身份一致。外层串行队列 `out/microkernel/as-limits-final-queue/1792591555901993538/results.json` 的 14 阶段和外层回执均退出 0：宿主 sanitizers、六个确定性生命周期专项、六个 AS 专项，以及新完整矩阵。独立汇总与文件哈希为 `out/microkernel/as-limits-final-evidence.json`；提交前只更新结果文档并核对所有非文档输入未变化。

| 平台 | 生命周期专项 单核/四核 | AS 专项 单核/四核 | 完整 glibc 单核/四核 |
|---|---|---|---|
| ARM32 | 143 / 146 | 138 / 140 | 291 / 294 |
| ARM64 | 143 / 146 | 139 / 142 | 289 / 292 |
| RV64 | 144 / 147 | 140 / 143 | 290 / 293 |

表中为实际检查数，全部退出 0、必需标记齐全。AS 回归覆盖真实 glibc 查询/权限/指针回滚、页舍入、PROT_NONE、MAP_FIXED 净增长、共享映射、brk、线程共享、fork 独立和 exec 保留/失败；内核自检另验证所有凭据先安装的地址空间初始化入口统一继承并执行限额。生命周期回归先收到 SIGCHLD 再 wait，检查退出码 37 与致命 SIGTERM，并保留额外存活线程以验证整组终止；孤儿回归由中间父进程退出的实际 wait 结果同步，验证存活线程身份、fork/wait 与最终回收。

宿主 100000 次边界检查、23 项记录锁、13 项管道、13 项权限、6 项 NOFILE 和 8 项路径检查均在 ASan/UBSan 下退出 0。原有八组合、每架构 51 个上游选定用例、原版 STM32 netduinoplus2/MPU、RV32 MMU/非 MMU、时钟/异常、持久化和 Lua/LVGL 都包含在新 68 组内。

保留开发失败和中断：首轮工具沙箱使 LeakSanitizer 因 ptrace 限制退出，普通执行环境保持全部检测后通过；`1792588582197485123` 完成 5/68 后因审查发现原生 spawn 漏接限额而主动中断。随后 `1792589121306842428` 完成 36/68，35 组通过、RV64 单核 glibc 组失败：来宾虽退出 0，仍因 `leader_exit_worker_child errno=10` 的 FAIL 被严格拒绝，揭示活父进程的子僵尸提前回收及旧 leader 状态掩盖 worker 失败。新增线程回调的 Werror 编译失败保留于队列 `1792590746379902085`，修正显式返回且未关闭 Werror。队列 `1792590819853562420` 的 12 个专项通过，完整运行 `1792591209853527184` 完成 3/68 后因同类孤儿身份审查主动中断。最终快照加入集中初始化、整组状态记录和孤儿可回收性检查后，才得到上面的新 68/68。

验收基线 HEAD 为 `a8339b0511ccaf7861a20a597946e00232d4346a`，tracked diff SHA256 为 `083c6c69a9140b143ac021d179a0ef781b9d62582b5e58e063f1b7f041fac6fb`；当时未跟踪的新 AS 应用 SHA256 为 `85eebba181a2a73d32eb3551604e5898af52c0b2117235b7915f95a759522c28`。本批不扩大任务/物理容量，不实现可变 STACK、NPROC、init/subreaper 归属、TTY 或网络，不能宣称完整 POSIX.1-2017。


## 线程容量扩展最终验收：1792629772377014664

完整命令 `python3 tests/acceptance.py --include-posix --require-frozen-source` 完成 **68/68 组**，各组实际退出 0；宿主 ASan/UBSan/LeakSanitizer、六个容量专项和完整验收共 8 个串行阶段均返回 0。源码与 Git 索引冻结，测试 HEAD `ba878939699ee7cd07dfda9acdd82a46d3b53e5c`，tracked diff SHA-256 `f5ed2a0637d5dfc083afaebc4e44bdc35c1d909ca4ac1ea9efb10abd5ad062cc`。队列 `out/microkernel/capacity-final-queue/1792629560717039839/results.json`；独立证据 `out/microkernel/capacity-final-evidence.json`。

每个专项包含两轮各 64 个默认属性 glibc worker 与主线程同时存活，默认栈 262144 字节；第二轮实际缓存复用。采样的最低空闲页数包含 monitor 的 8192 字节栈开销，起止基线均在 monitor 不存活时采集，基线已包含真实 virtio-rng 首次 DMA 初始化；最终 join/进程退出和有界 VFS GC 后要求精确相等。

| 平台 | CPU | 专项检查 | 全量 glibc 检查 | 空闲页：前/最少/后 | 任务峰值 | 二轮复用栈 |
|---|---:|---:|---:|---|---:|---:|
| arm_virt | 4 | 144 | 308 | 7757/2804/7757 | 72 | 64 |
| riscv_virt | 4 | 145 | 308 | 7757/2495/7757 | 72 | 64 |
| arm64_virt | 4 | 144 | 307 | 7757/2405/7757 | 72 | 64 |
| arm_virt | 1 | 141 | 306 | 7949/2996/7949 | 69 | 64 |
| riscv_virt | 1 | 143 | 305 | 7949/2737/7949 | 69 | 64 |
| arm64_virt | 1 | 142 | 304 | 7949/2639/7949 | 69 | 64 |

实测预算（最终四核专项 ELF；kernel 大小包含原有 32 MiB 页池）：

| 平台 | kernel 起点至 BSS 结束/字节 | 用户数据模板/字节 | 12 份页表/字节 | VM 事务最大单函数栈帧/字节 |
|---|---:|---:|---:|---:|
| arm_virt | 45301760 | 66472 | 3538944 | 128 |
| arm64_virt | 49123328 | 70032 | 6438912 | 224 |
| riscv_virt | 46100480 | 70000 | 6438912 | 144 |

预算、ELF SHA-256 和编译器 `-fstack-usage` 明细为 `out/microkernel/capacity-final-budgets/results.json`。用户模板均小于 131072 字节；单函数栈帧不是整个调用链的总和。

额外实际回归覆盖 IPC 槽位 31/32/63/64/79、完整 ID/token、私有端点通知跨 VFS 重初始化保留；独立地址空间池耗尽/引用释放；最高槽位 gateway；384 VMA 满表保护/替换失败回滚；64 VFS 挂起读/记录锁等待、63 FD 表 GC/fork 引用及 64 owner 图遍历。默认八组合、stock QEMU netduinoplus2 STM32/MPU、RV32 MMU/非 MMU、存储与库应用均包含在同轮完整矩阵。

开发阶段曾因旧映射耗尽测试上限、首次 RNG 三页 DMA 计入时机和 IPC 回归 fixture 的通知优先级顺序失败；均修正后重跑。早期冻结验收在审查发现 IPC 重初始化误清私有通知时主动停止，报告 `1792628520052524844` 保留为未完成；失败队列 `1792629431354189462` 亦不计入通过。最终结果仅使用上面的新源码身份和完整报告。

本批仍为有界 POSIX 子集：12 地址空间/进程对象、32 FD、32 MiB 页池；25 个子进程同时存活、NPROC/可变 STACK、init/subreaper、完整命名空间、TTY/作业控制和网络仍待开发。未宣称完整 POSIX.1-2017、物理板或远端 CI 已通过。


## 进程容量与 NPROC 最终验收：1792672703748903595

完整 `python3 tests/acceptance.py --include-posix --require-frozen-source` 完成 **68/68 组**，每组实际退出 0。宿主、六个 25 子进程专项、六个 NPROC 专项及完整验收共 14 个串行阶段全部返回 0。源码、Git 索引和四份编排脚本冻结；测试 HEAD `c875858aff441f581766aec7e55023582d8dc64d`，tracked diff SHA-256 `97e73edf2346c2cb3df5e225ada99eea77efee293e99851ef5aa72640859438c`。队列 `out/microkernel/process-final-queue/1792672359003114895/results.json`；独立证据 `out/microkernel/process-final-evidence.json`。

| 平台 | CPU | 进程专项/NPROC/全量 glibc 检查 | 采样空闲页：前/最少/后 | 任务峰值 | RO 升级/远端读取轮数 |
|---|---:|---|---|---:|---|
| arm_virt | 4 | 143/148/334 | 7757/2652/7757 | 33 | 16/16 |
| riscv_virt | 4 | 145/149/332 | 7757/2611/7757 | 33 | 16/16 |
| arm64_virt | 4 | 144/148/331 | 7757/2488/7757 | 33 | 16/16 |
| arm_virt | 1 | 141/145/331 | 7949/2844/7949 | 30 | 16/0 |
| riscv_virt | 1 | 143/147/330 | 7949/2803/7949 | 30 | 16/0 |
| arm64_virt | 1 | 142/146/329 | 7949/2680/7949 | 30 | 16/0 |

每个组合两轮各 25 子进程与父进程同时存活，WNOHANG 和独立原生 monitor 确认 held cohort；四核每波实际在至少两核执行。RO 升级阶段 16 轮真实默认属性 pthread 并发读取与父子页隔离、部分 unmap 检查；单核远端轮数为 0。采样最低空闲页包含 8 KiB monitor 栈，起止基线在 monitor 不存活时采集，并已包含真实 RNG 初始化；最终对象/任务/物理页精确相等。

| 平台 | kernel 起点至 BSS 结束/字节（含32MiB页池） | 用户数据模板/字节 | 40 份页表/字节 | VM 事务最大单函数栈帧/字节 |
|---|---:|---:|---:|---:|
| arm_virt | 54149120 | 108024 | 11796480 | 128 |
| arm64_virt | 64946176 | 112144 | 21463040 | 208 |
| riscv_virt | 61788160 | 112112 | 21463040 | 192 |

ELF SHA-256 和 `-fstack-usage` 明细 `out/microkernel/process-final-budgets/results.json`。模板仍小于 131072 字节；单函数栈帧不冒充整条调用链总量。

旧 eager fork 实测 ARM64 四核仅成功创建 10 子进程，下一次 ENOMEM；空闲页 7757→采样最低418→7757，原报告 `out/microkernel/glibc-reports/1792653352040069443/results.json` 保留为失败。优化后的新报告实际证明 25 子进程，无扩大物理池或修改 libc/tunable。可写私有页继续复制，仅 RO/已背页 PROT_NONE 私有页保留引用并在写权限升级前复制，未实现按写故障触发的完整 COW。

宿主消毒器包含 203 候选组四批查询、首/中/末批失败及非法末项不释放状态、完整 generation/待确认 fork 引用、6576 OFD 引用回收。内核故障注入覆盖 fork 引用、多持有者、PROT_NONE、特权复制拒绝、保护事务安装失败回滚、父子升级、MAP_FIXED/部分释放及最后持有者回收。NPROC 的 13 个严格标记覆盖真实 UID 的线程/僵尸计数、拒绝创建恢复、root 边界、指针/别名回滚、继承和 UID 转移后延迟 exec；单纯降低 soft 的 exec 保持允许。

同轮完整矩阵同时保留六个全量 glibc 中的原有 64 worker 数值验证、默认八组合、stock netduinoplus2 STM32/MPU、RV32 MMU/非 MMU、存储、时钟/故障及独立库应用。未宣称物理板或远端 CI 已通过。

剩余边界：80 任务槽、40 空间/进程对象、32 FD 和 32 MiB 页池仍有界；init/subreaper、完整 wait/会话、可变 STACK、完整命名空间、TTY/作业控制及网络尚未完成，完整 POSIX.1-2017 仍未完成。

## nshell、源码整理与 RISC-V IRQ 重使能

最终命令 `python3 tests/acceptance.py --include-posix --require-frozen-source`：**72/72 组通过，实际验收退出 0**。报告 `out/microkernel/acceptance/1792678382066433286/results.json`；外层退出回执 `out/microkernel/shell-final-exit.json`；独立汇总 `out/microkernel/shell-final-evidence.json`。

验收期间源码与 Git 索引保持不变，1435 个文件逐一核对 SHA256。测试基点 HEAD 为 `4c31189ffe24278449a12d7fd1dad11b03c98f09`，冻结差异 SHA256 为 `f1d0ed29bdd7a473d73980bb35da579e98db5abbc3c462b86b5e46c1bd7fcfca`。验收后仅补写本节与执行清单，非文档文件继续按冻结快照核对。

| glibc 配置 | 通过检查 | 来宾退出 |
|---|---|---|
| arm_virt_glibc_1_cores | 331 | 0 |
| arm_virt_glibc_4_cores | 334 | 0 |
| arm64_virt_glibc_1_cores | 329 | 0 |
| arm64_virt_glibc_4_cores | 332 | 0 |
| riscv_virt_glibc_1_cores | 330 | 0 |
| riscv_virt_glibc_4_cores | 332 | 0 |

三种 MMU 主架构各完成 35 条 UART shell 交互，覆盖引用与转义、相对路径、错误输入无副作用、编辑/历史/CRLF、独立 ELF、两次服务重启和资源基线。STM32 普通模式验证原版 `netduinoplus2` 启动；MPU 两库与 RV32 MMU/非 MMU、SMP、持久化、时钟边界、特权异常、Lua/LVGL 仍全部通过。nshell 宿主 ASan/UBSan/LSan 检查包含 10 万次解析、1 万次参数往返、10 万次编辑随机输入与边界用例。该结果覆盖有界 nshell，支持范围见 [NSHELL.md](NSHELL.md)。

RISC-V UART 的一次真实停顿由 GDB 确认：ring 为空、接收线程等待 IRQ、UART LSR=`0x61`/IER=`1`，PLIC pending 与 enable 均为 `0x400`，但 sip=`0`；未补发输入，仅写回阈值原值即恢复。证据 `out/microkernel/shell-uart-diagnostic/1792677863540668155/1/`。NOS 在解除屏蔽后保持阈值不变并写回，使原版 QEMU 10.0.13 重新评估 pending。QEMU 本身未修改。

新增受控回归在真实 NOS ACK 处暂停，注入静默触发字符及屏蔽期间输入，随后只恢复 NOS 代码；测试不写控制器寄存器。旧 RV64 镜像 SHA256 `e85e1e467c63ba5a9918f8f10df4635970d702c45ff57bd4da6c44b233caf301` 正确失败，GDB/窗口检查成功但 pending 命令未执行，报告 `out/microkernel/uart-rearm/1792678250156875125/riscv_virt/results.json`。修复后的 RV64、RV32 MMU、RV32 非 MMU 三项均通过，且在最终 72 组再次通过。

开发期间保留的失败记录：`out/microkernel/acceptance/1792675107193889589/results.json` 为远端服务取消后立即重启的旧测试竞态；`out/microkernel/acceptance/1792676448571282442/results.json` 为 UART 丢通知。增量检查首次遇到旧对象残留后中止的记录位于 `out/microkernel/shell-first-exit.json`。这些记录均未作为最终通过证据。当前源结构与清理边界见 [SOURCE_LAYOUT.md](SOURCE_LAYOUT.md)。

## nk60_v2 用户态键盘迁移：最终软件验证

本批在 `dev/nk60-v2` 上从 `62ae16cd9e3399fe18a5ba65b46816ff7e52902b` 开发。新增真实 STM32F401 键盘固件目标、用户态扫描/USB HID/配置/RGB 服务、能力检查的寄存器白名单与固定 SPI2 DMA，以及板级开发/烧录工具。功能与硬件边界见 [NK60 V2](NK60_V2.md)，主流项目依据见 [键盘调研](KEYBOARD_RESEARCH.md)。

**80 个唯一命令组均有同一冻结源码下的正式通过证据，但不是一次连续无失败的 80/80。** 汇总为 `out/microkernel/nk60-final/coverage-results.json`。冻结输入共 1477 个文件，基础 HEAD 如上；原 tracked diff SHA256 为 `c032a7129a03c14062e18ae69f2dcccfc0c9be1d1d833ae5d74c16b70018dbeb`，新增文件 SHA256 同时记录。测试后逐文件核对全部输入一致，随后仅更新本报告与执行计划。

执行与失败记录完整保留：

- 原始 `python3 tests/acceptance.py --include-posix --require-frozen-source` 在 27/80 组通过后会话进程中断，报告 `out/microkernel/acceptance/1792719902448257600/results.json`；完整 runner 的退出码不可得，不能算完整通过。
- 严格核对源码身份、全部文件哈希和命令前缀后恢复剩余 53 组。`out/microkernel/acceptance/1792741226753387273/results.json` 完成全部覆盖，**79/80，通过 79、失败 1，source_changed_during_run=false**，恢复进程实际退出 1。失败是 ARM64 glibc 四核组达到原 420 秒宿主总期限，缺少最终容量标记；未将超时算通过。
- 对比既有通过版本，失败 ELF 的内核/原生用户执行段与 26 个应用的全部可加载节相同；记录在 `out/microkernel/nk60-final/generic-binary-comparison.json`。同一失败镜像的带时间戳诊断于 352.93 秒自然退出 0，必需标记齐全，没有触发停滞 GDB 抓取。该次使用 900 秒诊断上限，明确不作为正式验收、不覆盖原失败，也不据此声称修复了某个死锁。
- 随后正式 runner 以**原来的 420 秒期限**重跑 ARM64 四核组，**331 项检查、退出 0**，64 线程与 25 子进程容量/回收指标、跨 CPU、RTC 和时钟检查均通过。报告 `out/microkernel/glibc-reports/1792743558289208836/results.json`，收据 `out/microkernel/nk60-final/a64-strict-retry-exit.json`。前后 ELF SHA256 均为 `c26f4ea884aaf543ffed6755678e7ad57cbd0045bdd4004f48bca217e028db98`。来宾断言和内部等待期限未变，未修改冻结源码；首次超时原因没有被冒称为已定位的软件缺陷。

原有八组合、三架构各 51 项上游选定 POSIX 用例（共 153）、glibc 单核/四核、存储/回退、SMP、STM32 MPU、RV32 MMU/非 MMU、时钟、精确异常及 Lua/LVGL 都有通过记录。完整 POSIX.1-2017 和远端 CI 仍不能由这些选定检查推断。

新增键盘软件检查在原完整覆盖的 `boards` section 中全部通过：

| 检查 | 实际范围 |
| --- | --- |
| `host_keyboard.py` | 真实动作引擎、10 万次状态扫描、双槽 CRC、1080 个逐字节断电点、存储故障和 generation 回绕 |
| `host_usb_keyboard.py` | 真实 HID/EP0 与 DWC2 寄存器模型、boot/NKRO/media、背压释放、idle/reset/suspend、1 万次异常请求 |
| `host_nk60_hardware.py` | 72→61 键映射、SPI NOR 分页/容量/超时/进度、GRB/DMA 错误断电 |
| `host_nk60_device.py` | 实际 MMIO 宽度/对齐/地址白名单、核心/DMA 寄存器拒绝、SRAM/长度/溢出边界 |
| `host_nk60_service.py` | 实际服务/动作/日志/HID 集成，物理解锁、UART 改键/宏、非阻塞输出、Flash 期间继续 USB/输入、挂起关灯及计时回绕 |
| `test_nk60_flash_tool.py` | 17 项 ARM ELF/入口/向量/载入地址与擦除边界校验 |
| `nk60_firmware.py` | MPU 关/开两种真实固件编译、64 KiB SRAM/128 KiB Flash、用户段与特权段边界 |
| `qemu_nk60_startup.py` | 实际 NK60 ELF 的 Cortex-M 用户入口、PSP/MSP、MPU、真实 SVC、设备权限、空闲调度；明确跳过 F401 时钟初始化 |

前五组启用 ASan、UBSan 与 LeakSanitizer。最终固件报告为 `out/microkernel/nk60-firmware/1792720080241941009/results.json`：MPU 关闭/开启的 bin 分别为 **47856 / 48844 字节**，静态 SRAM 最高地址均为 `0x20009e88`，低于 64 KiB 栈顶并保留至少 4 KiB MSP。精确测试产物已按哈希复制到 `out/microkernel/nk60_v2/{minimal,mpu}/nos.{elf,bin,hex,map}`，来源及 SHA256 在各目录 `firmware.json`。烧录器默认只生成已验证计划，没有连接探针或写入实物。

**本批没有可访问 nk60_v2 实板。** 原版 `netduinoplus2` 是 F405 模型；跳过时钟的启动专项只证明 CPU/内核 ABI，宿主模型只证明软件状态机。F401 晶振、实际扫描周期、USB 枚举与桌面兼容、LED/DMA 波形、Flash 实际掉电及电流仍待实板验收。扫描目标 4 ms，不宣称 1 kHz。RGB 配置前/挂起断电和软件限幅不等于全板满足 USB 挂起电流；低功耗尚未完成。VIA/Vial、组合键、无线和 USB 升级未实现，后续按调研优先级推进。


## ESP32-S3 / ESP-IDF

最新常驻nshell实现 `afb0818` 在冻结源码上通过C3/S3各三种镜像的官方QEMU专项：普通镜像各15项真实UART0交互、runtime19/生态21个必需标记全部通过，进程退出0，源码前后身份相同。报告为 `out/esp32c3/nshell-validation/reports/1792884401549386511/results.json` 和 `out/esp32s3/nshell-validation/reports/1792884403539264806/results.json`；三个相关shell/设备宿主runner同时通过。普通app_main常驻执行shell，系统控制台持有UART0和引脚；VFS/ELF/NLP仍未绑定。完整日志与固件哈希见 [最新验证](ESP32_C3_VALIDATION.md)。

C3适配提交 `eae2305` 的公共89组验收在41组通过后被中断，没有完整成功收据。以下保留更早快照，不把它们冒称为本次完整验收。

最新连接能力提交 `cc005897534b3265392b27d27d9af2dce1081890` 从干净工作区通过冻结专项：官方 S3 QEMU 的 normal/runtime/ecosystem 三次实际启动，runtime 19个、ecosystem 20个必需标记齐全、QEMU 均退出0；SDK 调度器替换、32个生态入口、全部后端及组件管理器审计通过。报告为 `out/esp32s3/connectivity-validation/reports/1792877237620117254/results.json`，开始/结束源码身份相同。

同一冻结源码的四个相关 runner（USB Host/OTG sanitizer、stock QEMU DWC2 键盘/鼠标/存储、设备框架和受管理服务）均退出0；S3 专项内另含实际 SDK USB/OTG/Wi-Fi 适配代码故障模型和可移植 GATT 状态检查。汇总为 `out/esp32s3/connectivity-evidence.json`。完整生态通过 SDK 支持的虚拟出厂校准 eFuse 启动，没有修改 SDK/模拟器。官方 S3 machine 缺少 RF/USB OTG 模型，实板无线、USB 枚举/供电仍未验收；本轮未修改公共内核/架构/运行库源码，未重跑完整89组或宣称完整POSIX。接口、固件哈希和证据范围见 [连接能力](ESP32_S3_CONNECTIVITY.md) 与 [冻结验证](ESP32_S3_VALIDATION.md)。

以下设备管理接入的历史完整验收：`6fa7a6e` 的冻结 `1792843580309736541` 为 **86/86 组通过，全部 runner 退出 0，源码未变化**。无线错误保护提交 `4db0927` 随后通过 S3 正常启动、16 个运行标记、32 SDK 入口与全部后端链接、Wi-Fi 实际适配代码故障模型和组件管理器检查，报告 `out/esp32s3/managed-validation/reports/1792846421120832933/results.json`。两提交间公共内核/架构/运行库/驱动框架完全相同；完整矩阵没有在后一个提交重复执行。

以下为早期移植记录：

公共实现 `e8ca7d0` 的冻结完整验收 `1792823254327022584` 为 **85/85 组通过、所有 runner 退出 0、源码未变化**。S3 工具环境修正后，`e85bbdd` 的专项 `1792826002233142349` 再次通过正常启动、14 个运行标记、32 个实际 SDK 入口链接审计和组件管理器配置检查。两提交间只变化 S3 环境脚本/依赖锁/测试及说明，已核对公共实现逐文件相同；不是冒称一次运行覆盖两个源码快照。ADC 全生态 profile 未在 QEMU 启动，实板外设、无线、双核和完整 POSIX 不由此推断。完整身份、固件哈希和限制见 [ESP32-S3 验证记录](ESP32_S3_VALIDATION.md)。
