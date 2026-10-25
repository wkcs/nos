# ESP32 调度与 UART 停止冻结验收（2026-10-26）

<!-- Copyright (C) 2026 NOS contributors -->

被测实现为干净提交 `7812b0389f00d955e1638c0ea07dc747572fed9b`。四组专项均首次通过，另有四轮并行复测全部通过；开始和结束的源码身份完全一致。本文为结束后的文档补录，前置失败另列，未计为通过。

## 修正及受控证据

诊断基线ca3e2e7保留旧行为。C3/S3各一次真实QEMU验证：嵌套暂停50ms期间tick从0到5，等待节点被提前移除，合同断言按预期失败。修正后四组的八份回归镜像均为`before=during=nested=0 linked=1`，暂停期累计4至5个tick，最外层恢复后tick为4至6且不少于累计值。tick相位与恢复后的额外中断可影响采样值；检查暂停期链表不变、恢复不丢累计tick，不要求固定恰好5个。

受控idle饥饿用例同时证明：worker退出两个tick后TLS回调可以尚未发生，而errno/thread cookie仍正确；允许idle运行后回调完成。TLS回归现在分别等待worker与清理完成，保留errno、cookie、两次精确回调断言，四阶段共享从创建worker开始的原3秒预算。任务堆回收检查等待实际记录释放，静态信号量避免回调被抢占后访问已释放对象。

前置c8dfd22首轮四组验证中，三组通过，S3 WL在设备阶段持续报IRQ8未处理，报告保留为失败。固定版本QEMU的S3中断矩阵在重路由时不撤销旧CPU线。对同一失败版本ELF，受控qtest在SDK释放临界区注入UART1 TX空中断，捕获源28从CPU8改成禁用值6后，CPU pending仍为0x100、IRQ8处理函数已删除，停在xt_unhandled_interrupt入口。这证明可重复的待处理中断卸载机制；原始随机失败未采样具体UART状态位，不能追溯其精确状态。

NOS设备停止先调用原版IDF uart_disable_intr_mask，再调用原版uart_driver_delete；错误保留资源所有权供重试。回归包装在关闭之前制造真实待处理中断，关闭后及删除入口均要求状态为0，连续64次经NOS服务启动/读写/停止。未修改vendor HAL、ISR或QEMU二进制。机制详见 [ESP32_TIMING.md](ESP32_TIMING.md)。

## 最终冻结矩阵

| 范围 | 结果 | 原始报告 |
| --- | --- | --- |
| C3 raw | normal/runtime/ecosystem、七阶段复启，10次启动通过；运行时/生态33/35标记 | `out/esp32c3/timing-quiesce-raw/reports/1792950743941809754/results.json` |
| S3 raw | 同上，10次启动通过；33/35标记 | `out/esp32s3/timing-quiesce-raw/reports/1792950744076200308/results.json` |
| C3 WL | 同上，10次启动通过；34/36标记 | `out/esp32c3/timing-quiesce-wl/reports/1792950743927927441/results.json` |
| S3 WL | 同上，10次启动通过；34/36标记 | `out/esp32s3/timing-quiesce-wl/reports/1792950744477548431/results.json` |
| 历史失败配置并行复测 | C3 raw生态/S3 raw运行时，同ELF四轮并行，8次启动通过 | `out/esp32-timing-quiesce-final/repeat/results.json` |

共12个固件镜像、48次成功QEMU启动。四组主runner及并行复测runner实际退出码均为0。每份普通固件完成32项UART/nshell交互；每组另有七阶段持久化检查。所有启动保留原180秒宿主预算，来宾3秒任务/TLS/定时器期限未扩大。

全部16次压力回归启动各完成64轮嵌套暂停/软件定时器检查和64次带待处理中断的UART启停，共各1024轮；TLS/FPU、原始定时器与通知、事件/队列、优先级继承、任务容量、私有stdio、描述符与退出回收继续通过。四份普通固件均审计确认不含诊断、快照、UART包装及注入符号，也不编译原FreeRTOS tasks.c或SDK应用启动器。

四份WL回归镜像各完成128次快照提交，221个数据/备用物理块全部参与、4736次擦除。WL用例VFS栈余量C3为8100字节，S3为7880字节。raw与WL各四份宿主故障模型报告通过ASan/UBSan/LSan，实际保护层及未经修改的SDK WL源码输入哈希稳定；协议、USB、无线故障模型、组件管理器及固定QEMU归档审计也通过。

源码、报告、模型输入与日志哈希索引：`out/esp32-timing-evidence.json`。runner回执：`out/esp32-timing-quiesce-final/*-exit.json`。未修改公共内核、内存、VFS或共享运行库实现，本批未重跑完整公共89组；最后89/89基线仍为bfd67ab，见 [ESP32_VFS_VALIDATION.md](ESP32_VFS_VALIDATION.md)。

## 保留的前置及开发失败

- ca3e2e7两架构预期合同失败：`out/esp32{c3,s3}/timing-before/probe/results.json`，含原始日志哈希。
- c8dfd22首轮S3 WL失败：`out/esp32s3/timing-validation-wl/reports/1792949044083122270/results.json`，runner退出1；其他三份同批报告及回执保留在`out/esp32-timing-final/`索引中，没有改写成成功。
- 同ELF受控UART负例：`out/esp32-timing-final/irq-inject-qtest/results.json`，含调试寄存器、默认处理函数表、路由和ELF哈希。调试器停在未处理函数入口，不把它算成功启动。
- 开发态S3 WL在四组新构建同时运行时耗尽180秒宿主预算，已通过UART64但停在WL压测完成标记之前，没有断言或IRQ错误。日志单独不能证明耗时原因；记录为失败，见`out/esp32-timing-final/uart-dev-wl-budget/results.json`。最终冻结两份S3 WL均在相同180秒预算内完成，不将开发超时计为通过。
- 开发时一次定时器测试先arm再暂停，可合法在暂停前触发；已改成先暂停再arm，保留失败于`out/esp32s3/timing-dev/start-before-suspend-attempt/`，不算生产缺陷。

0399da6旧S3复合断言没有子项值，旧C3超时没有当时完整状态；本批证明确定的暂停语义缺陷和回收同步错误，并验证相应修正，不能将旧随机失败的全部内部状态倒推为已知。持续负载和长期稳定性仍需后续观测。

## 正常固件与日志

| ZIP | ZIP SHA-256 |
| --- | --- |
| `out/esp32c3/artifacts/nos-esp32c3-raw-7812b03.zip` | `b2efaab72f38704a45ddfeccbb88a0e91568dc44e8388fa739a310b747a1f2ec` |
| `out/esp32s3/artifacts/nos-esp32s3-raw-7812b03.zip` | `a342c033460efe737f42bcb126022c626e49b8af5b21e4f40f2807bf3ca3a822` |
| `out/esp32c3/artifacts/nos-esp32c3-wl-7812b03.zip` | `6acc16439cfb6e5d7e6616059252c4a04f5779beb6ccc1130384dddf96e80cf9` |
| `out/esp32s3/artifacts/nos-esp32s3-wl-7812b03.zip` | `d3cce9ac55c7fac7fe4d7b59e031f61e5341d4aed76659db3167e163915981b1` |

ZIP包含正常固件BIN/ELF、匹配的原始bootloader/分区/OTA初始化、flash_args、依赖/构建身份、实际启动/nshell日志、持久化与回归证据。负例日志明确标记，不附带负例固件；不含模拟eFuse。ZIP CRC、逐文件SHA256SUMS和烧录文件引用已核验。实际日志另存`out/esp32-boot-logs/7812b03/`，未合成或清理。

解压后运行`python -m esptool --chip esp32c3 -p PORT write_flash @flash_args`，S3替换芯片名。UART0为115200，C3 TX21/RX20，S3 TX43/RX44。raw subtype0x40与WL subtype0x41不自动迁移，使用匹配分区表并备份已有板级数据。

仍为无MMU共享固件、单核，以及全局16节点/每文件512字节限制；没有独立ELF、完整POSIX或实板验收结论。没有可访问实板，射频、USB枚举、DMA、功耗和真实Flash掉电仍未验证。后续优先级见 [ESP32_NEXT_STEPS.md](ESP32_NEXT_STEPS.md)。
