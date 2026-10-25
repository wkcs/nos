# ESP32 原生 VFS 冻结验收（2026-10-25）

<!-- Copyright (C) 2026 NOS contributors -->

被测实现为干净提交 `bfd67ab8f6059a2fa85698d6ab353a0e1242b069`。公共完整验收和两平台专项的开始/结束源码身份一致，测试期间源码、文档及构建输入冻结。三个主runner与两个额外正常全生态runner均实际退出0。本文件为结束后的结果补录。

## 结果

| 范围 | 结果 | 原始报告 |
| --- | --- | --- |
| 公共完整软件验收 | **89/89通过**；complete=true，source_changed_during_run=false，每组returncode=0 | `out/microkernel/acceptance/1792907005224629555/results.json` |
| C3 normal/runtime/ecosystem | 构建、入口/ELF/实际VFS源码审计和真实esp32c3 QEMU全部通过；正常固件32项UART交互，运行时23/生态25个必需标记 | `out/esp32c3/native-vfs-validation/reports/1792907004839020459/results.json` |
| S3 normal/runtime/ecosystem | 同等范围，真实esp32s3 Xtensa QEMU全部通过 | `out/esp32s3/native-vfs-validation/reports/1792907005295137831/results.json` |
| C3 正常全生态固件 | 非回归固件构建及32项UART文件/控制台交互通过 | `out/esp32c3/vfs-normal-ecosystem/results.json` |
| S3 正常全生态固件 | 非回归固件构建及32项UART文件/控制台交互通过 | `out/esp32s3/vfs-normal-ecosystem/results.json` |

ESP32共八个实际镜像启动，所有QEMU退出0。正常固件验证文件创建、相对cwd、读写/追加、改名、删除、只读拒绝及编辑/错误恢复；回归固件验证dup偏移共享、unlink后打开描述符仍可读、管道EOF/EPIPE、FD耗尽、512字节文件边界、端点接收权限拒绝，以及超过进程槽数的原生EXIT文件表回收。VFS实际运行公共 `servers/vfs/server.c`，shell的标准输入输出也经过原生IPC再转到受管理UART。

公共89组包含：八组合基线1组、构建/宿主3组、板级/驱动/协议17组、共享文件3组、长路径3组、扩展OpenPOSIX3组、POSIX归档/熵3组、glibc单/四核6组、正常启动7组、存储13组、SMP12组、MPU2组、RV32两组、64位时钟4组、故障处理6组、独立应用/库4组。STM32使用原版netduinoplus2；NK60时钟桩和模型验证的边界仍保留在各报告中。该既定矩阵通过不等于完整POSIX认证或实板外设验收。

汇总证据为 `out/esp32-vfs-evidence.json`。实际退出回执位于 `out/esp32-vfs-final/`，包含三个主runner的exit.json及额外正常全生态的退出记录。

## 内存修复与历史尝试

初始 `3dc31ab` 上C3专项通过，但S3全生态镜像的静态DRAM溢出7768字节。为修复这一问题主动停止当时的公共验收；该次仅20/89组完成、没有已完成组失败，退出为SIGTERM，不能计完整通过。记录保存在 `out/microkernel/acceptance/1792906336684508259/results.json` 和 `out/esp32-vfs-final/attempt-3dc31ab/`。

最终实现将ESP版VFS打开文件描述和进程文件表移到服务启动时的内部capability heap一次性分配，失败则停止服务；其他平台保留静态表。没有减少C3的16/S3的24任务槽，也没有缩减每进程FD和总打开文件描述容量。随后在新冻结提交上重新执行了上述完整验收，全部通过。此前 `eae2305` 的41/89历史中断同样不能作为完整通过记录；最新完整记录以本文件为准。

## 原始日志与正常固件

- C3：`out/esp32-boot-logs/bfd67ab/esp32c3-startup-nshell.log`
- S3：`out/esp32-boot-logs/bfd67ab/esp32s3-startup-nshell.log`
- 两个正常全生态日志在同目录下的 `esp32c3-normal-ecosystem.log` / `esp32s3-normal-ecosystem.log`。

日志保留原始字节、bootloader输出、警告、实际交互和harness结束信号，没有合成或清理。实际正常启动包含：

```text
NOS: init task running under NOS scheduler
NOS: init online; managed devices ready
NOS: native VFS online (IPC, RAM/proc, managed console)
```

| 正常固件ZIP | ZIP SHA-256 |
| --- | --- |
| `out/esp32c3/artifacts/nos-esp32c3-bfd67ab-native-vfs.zip` | `a6eff53ce01803735493678aaabcdaf78417409f44271afb898bd16884d11c3e` |
| `out/esp32s3/artifacts/nos-esp32s3-bfd67ab-native-vfs.zip` | `5a889816a97e655586f11d1e488cd0182c048b9afd721aeb2f5fd20faa24d2c0` |

C3应用BIN为224080字节，SHA-256 `eac581c3bd8863fe2d591ac832966b0cd4951d2782445082b5ed6d285a95d7ba`；S3为236480字节，SHA-256 `c5328e9ef052f5a8b1da9c03d342d90aae2528e6739fe51aded4a8cea0ddf199`。

ZIP包含正常runtime固件、原始bootloader、分区/OTA初始化、ELF、烧录参数、构建与依赖身份、原始日志和验证证据；CRC、包内SHA256SUMS及烧录文件引用已核验。不含硬件修改型回归固件或模拟eFuse镜像。C3解压后使用 `python -m esptool --chip esp32c3 -p PORT write_flash @flash_args`；S3将芯片参数替换为 `esp32s3`。UART0为115200：C3 TX21/RX20，S3 TX43/RX44。

当前是易失RAM文件系统和共享固件地址空间。Newlib描述符适配、持久块设备、独立ELF、分配器统一和NLP绑定仍未完成；本轮没有新增无线/USB/物理Flash实板结论。能力及预算见 [ESP32_VFS.md](ESP32_VFS.md)。
