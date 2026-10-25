# ESP32 Newlib 冻结验收（2026-10-25）

<!-- Copyright (C) 2026 NOS contributors -->

被测实现为干净提交 `106338b2008cde81bfd4b099fc911aff77395a80`。两平台主专项与额外普通生态配置专项的开始/结束源码身份相同，status为空；四个最终runner均实际退出0。本文件为结束后的文档补录，不改写原报告身份。

## 结果与范围

| 范围 | 结果 | 原始报告 |
| --- | --- | --- |
| C3 normal/runtime/ecosystem | 实际构建、NOS 入口/编译/ELF 审计与真实 QEMU 通过；正常固件32项UART交互，运行时28/生态30个必需标记 | `out/esp32c3/newlib-validation/reports/1792913877786530733/results.json` |
| S3 normal/runtime/ecosystem | 实际构建、NOS 入口/编译/ELF 审计与真实 QEMU 通过；正常固件32项UART交互，运行时28/生态30个必需标记 | `out/esp32s3/newlib-validation/reports/1792913884616823050/results.json` |
| C3 普通生态配置 | 构建、NOS/Newlib 符号和32项UART交互通过 | `out/esp32c3/newlib-normal-ecosystem/results.json` |
| S3 普通生态配置 | 构建、NOS/Newlib 符号和32项UART交互通过 | `out/esp32s3/newlib-normal-ecosystem/results.json` |

共八个实际镜像启动，所有QEMU退出0，并保留harness收齐标记后结束模拟器的记录。正常固件按runtime和ecosystem配置各构建一次；生态回归镜像强制保留完整外设入口，普通生态配置允许链接器裁剪未使用的后端。不能将后者说成全部外设符号都在最终镜像。

新增实际来宾验证包括：原始Newlib fopen/fprintf/fscanf/freopen、FD与原生API互操作、flags/errno、目录、pread/pwrite与偏移、管道和poll/select位转换、真实SDK注册文件系统FD与NOS FD3并存、跨任务FILE/DIR关闭拒绝、私有stdout、正常退出刷新、native EXIT/外部删除丢弃缓冲，以及超过进程槽数的反复回收和精确heap比较。启用私有标准流后，C3的16/S3的24个任务槽均实际创建并运行，未缩减原容量。原任务栈/TLS回收检查也通过。

两平台均执行协议、USB与无线故障模型的ASan/UBSan/LSan检查、组件管理器2.5.2配置探测及固定QEMU归档校验。模型不等于实板RF/USB验证。

本轮只增加ESP私有libc端口并修改ESP任务/设备钩子、init、组件及测试，没有改动公共内核、内存、VFS服务或共享运行库源码。没有重新执行公共89组矩阵；最后完整89/89基线仍为 `bfd67ab`，见 [ESP32_VFS_VALIDATION.md](ESP32_VFS_VALIDATION.md)。不将旧基线冒充本提交的完整验收。

证据汇总 `out/esp32-newlib-evidence.json`；退出回执 `out/esp32-newlib-final/`。首次额外普通生态检查误用回归专用的全部后端符号保留要求，两平台均退出1；原日志及退出值在 `out/esp32-newlib-final/attempt-normal-symbol-retention/`。随后仅修正工作区临时辅助工具，按普通固件的编译/核心符号/启动范围复验；仓库源码始终冻结，主生态回归严格要求未改变。

## 原始日志与正常固件

普通固件真实启动包含：

```text
NOS: init task running under NOS scheduler
NOS: init online; managed devices ready
NOS: native VFS online (IPC, RAM/proc, managed console)
NOS: Newlib filesystem online
[nos] nshell: starting (ESP shared-firmware profile)
NOS SHELL READY
```

原始UART捕获在 `out/esp32-boot-logs/106338b/`，保留bootloader、警告、全部交互和harness结束信号，没有拼接或清理。每个平台有 `*-startup-nshell.log` 和 `*-normal-ecosystem.log`。

| 正常runtime固件ZIP | ZIP SHA-256 |
| --- | --- |
| `out/esp32c3/artifacts/nos-esp32c3-106338b-newlib.zip` | `2bb6ff82f7a465d4af3bbc629d7170017757441ea63b55748e4f9b8706069922` |
| `out/esp32s3/artifacts/nos-esp32s3-106338b-newlib.zip` | `ce4c22f81b7b7693c630d0cbea538201df4434f31917327b40b3177f235fb1f0` |

C3应用BIN为230144字节，SHA-256 `3cee5d07d526aa61356cac14439d4a15197d6ce63350121c416627427cc87eed`。

S3应用BIN为241472字节，SHA-256 `35be3560c821fcbbe57ca4ea67a0f35f1717bb980535a2dec0e39ca025dff660`。

ZIP包含正常固件BIN/ELF、原始bootloader、分区/OTA初始化、flash_args、构建/依赖身份、日志与报告，已核对CRC、逐文件SHA256SUMS及烧录引用。不含会修改测试NVS/OTA的回归镜像或模拟eFuse镜像。解压后使用 `python -m esptool --chip esp32c3 -p PORT write_flash @flash_args`；S3替换芯片名为esp32s3。UART0为115200，C3 TX21/RX20，S3 TX43/RX44；ROM USB下载端口与诊断串口可能不同。

当前仍是易失RAM文件与共享固件地址空间，fsync/O_SYNC明确不支持。持久存储、分配器统一、独立ELF、NLP绑定与实板无线/USB验证继续待办；不是完整POSIX认证。实现与FD/FILE生命周期边界见 [ESP32_NEWLIB.md](ESP32_NEWLIB.md)。
