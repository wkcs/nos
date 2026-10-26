# ESP32 RAM 容量与文件同步冻结验收（2026-10-26）

<!-- Copyright (C) 2026 NOS contributors -->

被测实现为干净提交 `addc280324c47651b5f573e29c66773604786545`。该版本四组ESP专项均首次通过，公共完整验收89/89通过；所有最终runner实际退出0，开始和结束的源码身份一致。本文为结束后的文档补录，此前候选实现与部分验收失败另列。

## 本批实现

ESP RAM文件从16节点/512字节扩展为32节点/每文件8192字节，节点总数包含6个系统节点。目录及空文件不分配数据区，非空文件使用按需512字节首段，超过512字节时使用独立分页，扩展数据预算共32768字节；大文件按全部页计费，小文件首段及元数据另计。每任务11个FD、64字节路径保持不变，物理heap不足仍返回ENOMEM。

增长中任一页分配失败，回滚本次新页，保留旧内容、尺寸、容量与预算。稀疏扩展补零，截断后不暴露旧尾部；截断为0及最后一个引用释放时回收数据页。任务退出也回收未主动关闭、已unlink的大文件。服务启动与持久导入的分配失败均回滚。

原生API追加nos_fsync(fd)及VFS操作码50，现有操作码和消息布局不变。ESP Newlib、musl及glibc网关按FD选择后端：RAM成功且不触发Flash；持久文件提交所属卷；控制台、管道、proc返回EINVAL，错误FD返回EBADF。musl/glibc的fdatasync采用相同保守同步。显式nos_sync及shell sync仍为卷级操作。

持久版本2仍为16条记录（含挂载根目录）、每文件512字节。序列化扫描全部32节点并紧凑写入旧磁盘槽，修正高索引持久节点可能漏存的问题；导入接受旧空槽布局，记录格式、bank地址、CRC与提交顺序保持不变。新固件能读取旧布局，旧固件的内存节点上限仍限制其可导入记录数。设计与边界见 [ESP32_RAMFS.md](ESP32_RAMFS.md)。

## 最终结果

| 范围 | 结果 | 原始报告 |
| --- | --- | --- |
| 公共完整验收 | 89/89组；冻结检查通过，实际退出0 | `out/microkernel/acceptance/1792978243831401404/results.json` |
| C3 raw | normal/runtime/ecosystem及七阶段复启，10次启动；35/37回归标记 | `out/esp32c3/ramfs-paged-raw/reports/1792977181140406975/results.json` |
| S3 raw | 同上，10次启动；35/37标记 | `out/esp32s3/ramfs-paged-raw/reports/1792977181654346312/results.json` |
| C3 WL | 同上，10次启动；36/38标记 | `out/esp32c3/ramfs-paged-wl/reports/1792977180764236552/results.json` |
| S3 WL | 同上，10次启动；36/38标记 | `out/esp32s3/ramfs-paged-wl/reports/1792977181168048078/results.json` |

ESP共12个固件镜像、40次成功QEMU启动，每份普通固件完成32项UART/nshell交互。每组持久化用例在首次挂载前创建20个RAM文件，强制持久目录和文件落到高索引槽，再提交并以独立QEMU进程重启恢复；raw七阶段共46项交互，WL共44项。保持原180秒宿主启动预算及既有来宾任务/TLS/定时器期限。

公共验收覆盖ARM32、ARM64、RV64的原生/真实C库、glibc单核/四核、共享文件页、扩展POSIX用例、原生/FAT持久化及多核组合，还包括STM32 MPU、RV32 MMU/无MMU、时钟边界、故障来源、Lua/LVGL和板级模型。新增fsync通过实际IPC及musl/glibc调用验证；这不等于完整POSIX认证。

八份ESP回归镜像均通过8KiB边界、跨页读写、配额耗尽、截断补零、dup/unlink最终引用回收和任务退出大文件回收，保留原任务容量、私有stdio、TLS/FPU、定时器、UART待处理中断及设备32槽检查。四份WL镜像各完成128次提交、221个数据/备用物理块参与、4736次擦除。实际VFS最小剩余栈：C3 raw 8032字节、C3 WL 7968字节、S3 raw 7448字节、S3 WL 7204字节，均高于512字节检查门槛。

四份ESP宿主VFS模型报告各执行C3/S3资源profile，使用实际共享VFS源码、只允许512字节分配的堆模型、第三次页分配失败、启动/proc及导入失败，验证回滚和泄漏；ASan/UBSan/LSan通过。raw与WL也各有四份宿主故障模型通过，输入哈希稳定。模型中的IPC、堆和设备边界为模拟，真实IPC及CPU由另行QEMU用例覆盖。

最终回执：`out/esp32-ramfs-paged-final/*-exit.json`。完整报告、模型及日志哈希索引：`out/esp32-ramfs-evidence.json`。普通固件的编译/符号审计保留NOS启动和调度归属，排除SDK tasks.c/应用启动器及回归专用注入。

## 保留的失败与修正

- S3开发态64KiB连续缓冲预算：第六个8KiB文件扩容返回ENOMEM，总空闲25456、最大块7680字节。日志/ELF/清单在`out/esp32s3/ramfs-dev/quota-64k-diagnostic/`，最早复合断言负例在同级`quota-first-failure/`。预算调整为32KiB。
- 55547ae冻结候选：C3 raw/WL生态在第四个连续8KiB缓冲上返回ENOMEM，总空闲12844、最大块7680；S3两组通过。四份回执保留于`out/esp32-ramfs-validation-final/`，C3报告仍为失败。最终实现改为512字节分段，未用串行重试替代修正。
- 分段开发态：C3通过RAM及完整任务槽后，设备创建索引22返回ENOSPC，总空闲3836、最大块1088。诊断在`out/esp32c3/ramfs-dev/paged-device-budget-diagnostic/`，最早负例在`paged-device-budget-failure/`。首段按需分配避免32个空槽预占约16KiB，最终两平台保留完整设备容量。
- 首轮公共验收：`out/microkernel/acceptance/1792974442269040318/results.json`在15/89组后中止，14通过、NLP宿主GCC编译超过原60秒预算，子runner退出1，整体中断退出130；回执为`out/esp32-ramfs-acceptance-initial-exit.json`。未运行到该阶段的NLP程序测试，不能称为协议断言失败。
- 第二次公共候选验收：`out/microkernel/acceptance/1792975406313052808/results.json`完成4组且均通过，因修正C3缓冲实现而中止，进程返回-2，回执保留于`out/esp32-ramfs-final/public-final-exit.json`。两次部分运行均未改写为完整通过。

最终公共验收在四组ESP编译/测试结束后独立运行，保留原预算；NLP互通与整个89组通过。最终源码与前置候选不同，上述记录不被算入最终40次成功启动。

## 正常固件

| ZIP | ZIP SHA-256 |
| --- | --- |
| `out/esp32c3/artifacts/nos-esp32c3-raw-addc280.zip` | `36341aa36baa64a1e42f2b2c32a949feb65d49cc060cb78c5cf4b21aaee90040` |
| `out/esp32s3/artifacts/nos-esp32s3-raw-addc280.zip` | `0e72c38e75256a32aa752102dc16a27d76a973d3adfb475182eeff1980645e98` |
| `out/esp32c3/artifacts/nos-esp32c3-wl-addc280.zip` | `dd5128a26b245d09be074df2565572d7753df22736055c110715fa60280c669b` |
| `out/esp32s3/artifacts/nos-esp32s3-wl-addc280.zip` | `877999a6578c6ae5bd9a1d891fba8dae0140badf836690117593db5642e70dda` |

ZIP包含正常BIN/ELF、匹配的原始bootloader/分区/OTA初始化、flash_args、构建与依赖身份、实际启动/nshell/回归/持久化日志及报告。开发失败日志明确标记，不包含失败或压力固件、模拟eFuse。ZIP CRC、逐文件SHA256SUMS及烧录文件引用均已核验，实际日志另存`out/esp32-boot-logs/addc280/`。

解压后运行`python -m esptool --chip esp32c3 -p PORT write_flash @flash_args`，S3替换芯片名。UART0为115200，C3 TX21/RX20，S3 TX43/RX44。raw与WL使用不同subtype且不自动迁移，需匹配分区表并备份已有板级数据。

持久大文件/FAT流式后端、新格式迁移和O_SYNC仍待下一阶段。ESP仍为单核、无MMU共享固件，独立ELF、完整POSIX、射频、USB枚举、DMA、功耗和真实Flash掉电均不能由本批推断。没有可访问实板，所有固件运行结果来自QEMU。
