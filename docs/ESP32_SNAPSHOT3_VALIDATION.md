# ESP32 扩展持久快照冻结验收

<!-- Copyright (C) 2026 NOS contributors -->

被测实现为干净提交 `3ab51e7e11991bd0c20b14727a7059362db7198d`。最终公共完整验收89/89组通过，C3/S3 raw/WL的新旧格式八组共80次正式QEMU启动通过；所选最终runner实际退出0，源码身份保持一致；同提交S3 WL的首轮超时另列，原因仍未完全确定。本文在测试结束后补录，不计入被测提交。

## 实现与结果

可选snapshot3支持16条持久记录、单文件8192字节，复用与RAM文件共享的32768字节按需页预算。默认native2保持原512字节格式。新版只读导入v2，首次sync固定写后半区，完整落盘后写入拒绝旧固件的标记；错误重试必须先完成标记，再复用前半区。机制、编译刷写及容量限制见 [ESP32_SNAPSHOT3.md](ESP32_SNAPSHOT3.md)。

| 范围 | 结果 | 原始报告 |
| --- | --- | --- |
| 公共内核/VFS/运行库 | 完整89/89，冻结检查通过、实际退出0 | `out/microkernel/acceptance/1793066140638957259/results.json` |
| esp32c3-raw-native2 | 正常/回归/生态3次启动，持久化7次独立启动，通过 | `out/esp32c3/snapshot3-baseline-raw/reports/1793027030083675253/results.json` |
| esp32c3-raw-snapshot3 | 正常、v2种子、只读导入、迁移、降级拒绝、第二次提交、边界及3种损坏检查，共10次启动，通过 | `out/esp32c3/snapshot3-verified-raw/reports/1793027233344052976/results.json` |
| esp32c3-wl-native2 | 正常/回归/生态3次启动，持久化7次独立启动，通过 | `out/esp32c3/snapshot3-baseline-wl/reports/1793027030027076582/results.json` |
| esp32c3-wl-snapshot3 | 正常、v2种子、只读导入、迁移、降级拒绝、第二次提交、边界及3种损坏检查，共10次启动，通过 | `out/esp32c3/snapshot3-verified-wl/reports/1793027469972369766/results.json` |
| esp32s3-raw-native2 | 正常/回归/生态3次启动，持久化7次独立启动，通过 | `out/esp32s3/snapshot3-baseline-raw/reports/1793027288755251365/results.json` |
| esp32s3-raw-snapshot3 | 正常、v2种子、只读导入、迁移、降级拒绝、第二次提交、边界及3种损坏检查，共10次启动，通过 | `out/esp32s3/snapshot3-verified-raw/reports/1793027562093405125/results.json` |
| esp32s3-wl-native2 | 正常/回归/生态3次启动，持久化7次独立启动，通过 | `out/esp32s3/snapshot3-baseline-wl/reports/1793065750198349396/results.json` |
| esp32s3-wl-snapshot3 | 正常、v2种子、只读导入、迁移、降级拒绝、第二次提交、边界及3种损坏检查，共10次启动，通过 | `out/esp32s3/snapshot3-verified-wl/reports/1793066060852207035/results.json` |

八组共16个固件镜像，其中8个正常固件、8个原有回归/生态固件；每个新格式组复用匹配的默认格式正常固件生成旧数据和验证降级。原始串口日志和Flash/eFuse镜像按每次独立启动保存。八个正式runner与公共runner回执在 `out/esp32-snapshot3-final3/`；完整报告、模型输入和镜像证据索引为 `out/esp32-snapshot3-evidence.json`。

默认格式组保留原有RAM页配额、稀疏/截断/引用回收、stdio/TLS、任务及设备槽、UART待处理中断等回归；四份WL回归镜像继续完成128次提交的物理块轮换检查。每份默认正常固件通过32项UART/nshell交互。

新版格式实际验证4096/8192字节持久文件跨重启精确读取；首次导入与未sync修改逐字节核对storage不变。迁移确认后重新刷入旧正常固件，挂载返回EIO且不写storage；重新刷回新版后数据仍在。8192字节后继续append与第17条记录创建返回ENOSPC。raw分别破坏最新头和最后已用数据扇区，回退至上一份4KiB快照；两份头损坏拒绝挂载。WL分别验证一份状态副本损坏后修复、两份损坏拒绝、未知非空拒绝。

每个默认格式组另运行真实VFS的snapshot3宿主模型，包含C3/S3各224/220扇区四种配置，ASan/UBSan/LSan全部通过。模型覆盖每个迁移/后续提交写入及flush中断点、每个读取点故障、CRC/元数据/路径/父目录/类型/几何错误、16页各分配失败回滚、共享预算和代次溢出。块设备与IPC边界为模型，真实CPU/IPC另由QEMU覆盖。

额外冻结模型覆盖满16条记录、全部63字节长路径跨三个元数据扇区、不同文件的部分尾扇区、空文件及截断后稀疏扩展的两次提交/重载；C3/S3的224/220扇区四组ASan/UBSan/LSan通过。补充夹具和报告保留于 `out/esp32-snapshot3-records/1793026916745993143/results.json`，不修改被测源码。

## 保留的失败与修正

- 开发态第一轮QEMU脚本在已成功写入8192字节后，错误地预期权限为0666；实际umask后的结果为0644。仅修正预期，原报告和日志保留于 `out/esp32c3/snapshot3-dev/qemu/1792997237258407919/`。该运行不计入最终80次。
- 冻结候选8e2d0ef的C3 raw回归在文件系统之前失败：before=0、deferred=1、after=2，触发原有deferred至少2断言。原始IDF首个SysTick建立硬件计数基线，不能从启动后50ms推断中断已分派两次。测试先等待首个实际tick，再进入原有暂停区间；冻结、重放、链表和回收预算不变，未修改原始SDK或NOS调度逻辑，仅新增回归观测。机制见 [ESP32_TIMING.md](ESP32_TIMING.md)。
- 该候选矩阵为失败/部分运行，未按通过处理。终止记录为 `out/esp32-snapshot3-final/candidate-aborted.json`，原始报告与日志保留；当时BIN/ELF、清单和编译输入副本及SHA256在 `out/esp32-snapshot3-candidate8e2d0ef/`。
- 修正后的C3 raw额外执行两次完整预检，两次均为initial=0、ready=1，暂停时deferred=5，完整回归通过；报告 `out/esp32c3/snapshot3-firsttick-check/1793025623782366321/results.json`。这两次预检另计，正式八组80次均在新提交冻结后重新完成。
- acf8ab5候选的C3四组通过，S3 raw生态却在50ms后观察到deferred=0；配置确认S3也用SYSTIMER。最终回归等待实际5个tick，64轮定时器每轮等待实际3个tick，带独立3秒刺激看门狗及IRQ状态诊断，原冻结/重放/回收断言保留。原失败及部分矩阵在 `out/esp32-snapshot3-verified/`，BIN/ELF副本在 `out/esp32-snapshot3-candidateacf8ab5/`。修正后S3生态另做两次37标记完整预检均通过，报告 `out/esp32-snapshot3-stimulus/1793026866096090185/results.json`，正式80次在3ab51e7重新冻结后执行。
- 最终3ab51e7的S3 WL默认组首轮runtime在128次提交阶段超过180秒，无断言；失败镜像恢复至代次108。独立诊断采样代次18→35→51→65→80→96→113→130并到达128次提交末尾。原超时原因仍未确定，不能认定已修复或证明原进程没有瞬时阻塞。同一BIN/ELF、同一180秒预算单独复验后通过，只有后者10次启动计入正式80次。初始回执、原日志/Flash、诊断及哈希见 `out/esp32-snapshot3-evidence.json` 的retained_same_commit_wl_timeout；原失败报告 `out/esp32s3/snapshot3-baseline-wl/reports/1793027526689786115/results.json`。这是待追踪的可靠性限制，不能用重跑通过将其抹去。

最终公共89组在全部ESP构建/专项结束后单独执行，保留原有超时预算，没有用并发SDK编译挤占公共测试时间。

## 正常固件

| 组合 | ZIP | ZIP SHA-256 |
| --- | --- | --- |
| esp32c3-raw-native2 | `out/esp32c3/artifacts/nos-esp32c3-raw-native2-3ab51e7.zip` | `addb7d238443a86eb8dbe5eeae75d764b5e919e4b4f3fc98801d11a22766e6d5` |
| esp32c3-raw-snapshot3 | `out/esp32c3/artifacts/nos-esp32c3-raw-snapshot3-3ab51e7.zip` | `2d603f60c6cd841f7cd5cfc6e7ac162aeb39c762746c3077a35defd263a57eb4` |
| esp32c3-wl-native2 | `out/esp32c3/artifacts/nos-esp32c3-wl-native2-3ab51e7.zip` | `06389740e00e1f1b4c21c0b2370ee1b78d025495c831486ea5a78d12f1c2f51e` |
| esp32c3-wl-snapshot3 | `out/esp32c3/artifacts/nos-esp32c3-wl-snapshot3-3ab51e7.zip` | `ace5050d58fa0bbac09c92bf087e0fbabd1f8093fba4ee900229c34f705414be` |
| esp32s3-raw-native2 | `out/esp32s3/artifacts/nos-esp32s3-raw-native2-3ab51e7.zip` | `99974bd1575de9ca8dcbc139816ecf8fa746ec09f984b853469871438f9ebf66` |
| esp32s3-raw-snapshot3 | `out/esp32s3/artifacts/nos-esp32s3-raw-snapshot3-3ab51e7.zip` | `d64555478dd0a3cad1665d77882016f40a424e97042459fbf78a260de06eabd2` |
| esp32s3-wl-native2 | `out/esp32s3/artifacts/nos-esp32s3-wl-native2-3ab51e7.zip` | `7a2bb2d05f5ed069edd9f75609176289cffea15c0e1c91366f2f91b40af33163` |
| esp32s3-wl-snapshot3 | `out/esp32s3/artifacts/nos-esp32s3-wl-snapshot3-3ab51e7.zip` | `326839aa32f20bb3fc11db6120ee388b76c062a000673a6e9c63a515d0c3d1dc` |

ZIP包含正常BIN/ELF、匹配的原始bootloader/分区表/OTA初始化、flash_args、构建与依赖身份、实际启动与持久化日志及报告、SHA256SUMS。CRC、逐文件SHA256及烧录引用已核验。旧失败仅以报告/日志标注，不混入失败固件；模拟eFuse和测试Flash镜像不放入正常烧录包。另存的实际日志在 `out/esp32-boot-logs/3ab51e7/`。

解压后运行 `python -m esptool --chip esp32c3 -p PORT write_flash @flash_args`；S3更换芯片名。UART0为115200，C3 TX21/RX20、S3 TX43/RX44。刷写升级只替换固件分区，不整片擦除，必须保持相同分区表和raw/wl模式；先备份重要数据。

## 边界与后续

活动文件仍完整驻留RAM，共享32KiB扩展预算并不保证物理heap一定满足；路径64字节、每任务11个FD与16条持久记录仍有限制。fsync提交完整挂载，O_SYNC、FAT接入和流式大文件后端尚未实现。开始迁移后应持续使用新版固件，确认前中断可能恢复完整旧版或新版；成功迁移后不支持原地降级。

ESP仍为单核、无MMU共享固件。没有可访问实板；模拟器和故障模型不能证明物理掉电、Flash寿命、射频、USB枚举、DMA、功耗、独立ELF或完整POSIX认证。可靠性优先补充WL逐提交耗时、宿主CPU/I/O采样，追踪同源180秒超时；功能开发继续流式文件后端和资源所有权管理。
