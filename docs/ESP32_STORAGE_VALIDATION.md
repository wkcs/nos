# ESP32 原生Flash持久化冻结验收（2026-10-25）

<!-- Copyright (C) 2026 NOS contributors -->

被测实现为干净提交 `7577e74928919c86bee97e9c078d57505e824690`。两平台专项和额外普通生态配置专项的开始/结束源码身份一致，status为空；公共驱动框架输入哈希也保持一致。五个最终runner均实际退出0。本文件为验收后的文档补录，不改变原报告身份。

## 结果

| 范围 | 结果 | 原始报告 |
| --- | --- | --- |
| C3 normal/runtime/ecosystem + 持久化 | 构建、NOS入口/编译/ELF审计和10次真实QEMU启动通过；运行时29/生态31个必需标记，普通固件32项UART交互，另七次持久化启动共25项交互 | `out/esp32c3/storage-validation/reports/1792915880454494874/results.json` |
| S3 normal/runtime/ecosystem + 持久化 | 构建、NOS入口/编译/ELF审计和10次真实QEMU启动通过；运行时29/生态31个必需标记，普通固件32项UART交互，另七次持久化启动共25项交互 | `out/esp32s3/storage-validation/reports/1792915887036574113/results.json` |
| C3 普通生态配置 | 正常构建、NOS/Flash符号和32项UART交互通过 | `out/esp32c3/storage-normal-ecosystem/results.json` |
| S3 普通生态配置 | 正常构建、NOS/Flash符号和32项UART交互通过 | `out/esp32s3/storage-normal-ecosystem/results.json` |
| 公共驱动框架 | 实际框架四类回归与ASan/UBSan/LSan通过，输入稳定 | `out/microkernel/host-drivers/1792915893665019472/results.json` |

共八个实际固件镜像、14次额外持久化复启，总计22次真实QEMU启动，均退出0。harness收齐标记/交互后终止模拟器的记录保留在报告和原始日志。普通生态配置允许链接裁剪未使用后端，完整生态回归镜像仍严格审计保留全部规定入口，不混淆两个范围。

七次持久化启动使用同一正常固件并复用实际Flash镜像：首次写入/同步；重新挂载读取、rename/append并提交第二代；验证第二代并作未同步修改；复启确认未同步修改丢弃；分别损坏最新bank头和数据后恢复第一代；两个bank都损坏时返回EIO，shell继续运行且分区字节保持不变。未使用的存储区也保持不变。复用Flash时检查bootloader、应用和分区表字节未改；原始bootloader首次启动会更新OTA选择元数据，因此该可变数据区不与空白初始化文件强行比较。

来宾另外验证BLOCK控制器独占、非init挂载拒绝、init PID1、512字节含零二进制stdio往返与fflush/fsync。C3存储操作后的VFS栈余量为8100字节；S3运行时7880、生态7888字节，均高于512字节断言。原16/24任务槽、私有标准流和退出回收检查全部保留并通过。

NOR实际适配器的宿主ASan/UBSan/LSan故障模型覆盖指定分区/只读/加密/几何拒绝、擦除态到零映射、二进制往返、范围/溢出拒绝、SDK读/擦除/部分写失败、部分完成计数及相邻擦除块保护。两份冻结输入模型报告如下：

- `out/esp32-flash-model/1792915885491217577/results.json`

- `out/esp32-flash-model/1792915892679133104/results.json`

原有协议、USB和无线故障模型、组件管理器2.5.2及QEMU归档校验同样通过。汇总证据为 `out/esp32-storage-evidence.json`，实际退出回执在 `out/esp32-storage-final/`。

本批修改ESP专用适配器、任务槽预留、shell、分区、测试及公共驱动服务枚举（末尾追加BLOCK，不改变已有值）。未修改公共内核、内存、VFS服务或共享运行库实现；本批未重跑公共89组矩阵，最后完整89/89基线仍为 `bfd67ab`，见 [ESP32_VFS_VALIDATION.md](ESP32_VFS_VALIDATION.md)。不将旧基线称为本提交的完整验收。

## 正常固件与日志

| 正常runtime固件ZIP | ZIP SHA-256 |
| --- | --- |
| `out/esp32c3/artifacts/nos-esp32c3-7577e74-storage.zip` | `1145e6f80fbde3f9a5c52beeba27570afb479aff26777fb3760f3b3dc9b7720e` |
| `out/esp32s3/artifacts/nos-esp32s3-7577e74-storage.zip` | `1e008bd4b84667c67e0bbcb29f444e239b6bb13e38456c71d729facc70008f3d` |

C3应用BIN为313424字节，SHA-256 `69019b35ef642099d89d6f5ebc3c7584775f89da097ebb38bd361efdec3c4767`。

S3应用BIN为320304字节，SHA-256 `47e13c9b7df6bb76ccdbab8469821c8b91c5d7108e643aaa4b00d55e643b7fa4`。

ZIP包含正常BIN/ELF、原始bootloader、匹配分区/OTA初始化、烧录参数、构建/依赖身份、实际日志、持久化和驱动验证报告；已核对CRC、逐文件SHA256SUMS及烧录引用。没有放入回归固件或模拟eFuse。原始日志另存 `out/esp32-boot-logs/7577e74/`，包含两平台正常、普通生态配置及七阶段持久化UART捕获，没有合成或清理。

解压后运行 `python -m esptool --chip esp32c3 -p PORT write_flash @flash_args`，S3替换芯片名。新分区表的storage subtype为0x40，旧SPIFFS内容不会被导入或自动格式化；修改已有板级布局前保存原数据。UART0为115200，C3 TX21/RX20，S3 TX43/RX44，ROM USB下载端口可能与诊断串口不同。

nshell 示例：`mount /data`、`write /data/note "saved"`、`sync`；复启后再次挂载并读取。普通write/fflush/fclose不隐式提交NOR；C stdio需fflush后fsync。当前全局16节点、每文件512字节，没有磨损均衡，面向低频配置保存。设计、映射和预算见 [ESP32_STORAGE.md](ESP32_STORAGE.md)。

本轮是QEMU实际NOR操作与离线损坏夹具验证，不是实板断电、Flash耐久、无线或USB验收。磨损均衡/FAT、分配器统一、独立ELF、NLP绑定及实板外设验证仍待完成，也不代表完整POSIX认证。
