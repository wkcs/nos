# ESP32 磨损均衡冻结验收（2026-10-26）

<!-- Copyright (C) 2026 NOS contributors -->

被测实现为干净提交 `0399da6c4bf68d1ea60e8a23e771ac98fcf15a87`。最终四组专项开始/结束源码身份一致、status为空。本表包括相同ELF的raw串行复验，不代表首轮并行全部通过。本文为结束后的文档补录。

## 最终矩阵

| 范围 | 结果 | 原始报告 |
| --- | --- | --- |
| C3 raw normal/runtime/ecosystem及七阶段复启 | 10次QEMU启动通过；运行时/生态29/31标记，普通固件32项UART交互，七阶段25项交互，首轮失败后同ELF串行复验 | `out/esp32c3/wear-validation-raw/reports/1792946393722052753/results.json` |
| C3 wl normal/runtime/ecosystem及七阶段复启 | 10次QEMU启动通过；运行时/生态30/32标记，普通固件32项UART交互，七阶段23项交互 | `out/esp32c3/wear-validation-wl/reports/1792945129861435617/results.json` |
| S3 raw normal/runtime/ecosystem及七阶段复启 | 10次QEMU启动通过；运行时/生态29/31标记，普通固件32项UART交互，七阶段25项交互，首轮失败后同ELF串行复验 | `out/esp32s3/wear-validation-raw/reports/1792946432398951940/results.json` |
| S3 wl normal/runtime/ecosystem及七阶段复启 | 10次QEMU启动通过；运行时/生态30/32标记，普通固件32项UART交互，七阶段23项交互 | `out/esp32s3/wear-validation-wl/reports/1792945133194072109/results.json` |
| C3 WL普通生态固件 | 构建、NOS/WL符号及32项UART交互通过 | `out/esp32c3/wear-normal-ecosystem-wl/results.json` |
| S3 WL普通生态固件 | 构建、NOS/WL符号及32项UART交互通过 | `out/esp32s3/wear-normal-ecosystem-wl/results.json` |
| 公共驱动框架 | 实际框架四类回归、ASan/UBSan/LSan通过，输入稳定 | `out/microkernel/host-drivers/1792945145962142829/results.json` |

最终完整矩阵包含14个固件镜像、42次成功启动；四份普通runtime及两份普通WL生态固件各32项UART交互。加上首轮失败前的三次启动，本批共45次固件启动尝试，不能称为一次42/42并行全绿。所有最终runner实际退出0，harness收齐标记/交互后结束模拟器的记录保留。

四个WL回归镜像均完成128次连续快照提交：221个数据/备用物理块全部参与擦除，每镜像4736次成功擦除，不计三个元数据块。C3 VFS栈余量8100字节，S3两配置均7880字节。此结果是模拟器上的物理分布验证，不是Flash寿命或实板断电认证。

raw七阶段保留快照头/数据损坏回退。WL七阶段验证提交后恢复、未同步修改丢弃、单副本修复后首次读取正确，以及双副本损坏、未知非空内容拒绝初始化；后两者分区字节完全不变。WL允许数据在专用分区内轮转，不能套用raw固定物理未使用区不变的断言。

宿主执行未经修改的IDF WL_Flash/Partition/CRC源码及实际NOS保护层，注入NOR/ROM边界模型，验证600轮写入/flush、重新打开、单副本修复、损坏/未知内容拒绝、读取失败阻止写入，以及I/O错误锁定和释放不暗中flush；ASan/UBSan/LSan通过。首轮与复验共各六份WL和raw模型报告，输入哈希与路径在 `out/esp32-wear-evidence.json`。协议、USB、无线故障模型、组件管理器2.5.2和QEMU归档审计也通过。

raw→wl、wl→raw两方向的已有目录切换均在进入构建前拒绝，目录内容保持不变，见 `out/esp32-wear-final/format-guards/results.json`。

## 首轮失败与复验边界

两份首轮报告保持passed=false、runner退出1，没有改写：

- `out/esp32c3/wear-validation-raw/reports/1792945123409656621/results.json`

- `out/esp32s3/wear-validation-raw/reports/1792945126748933088/results.json`

C3在生态镜像的软件定时器测试中等待done信号量超过3秒来宾期限，此前runtime通过。S3在runtime的 `errno == 7 && thread_cookie == 0 && tls_deleted == 2` 复合断言失败，未记录具体子项值，不能直接推断TLS损坏或idle未回收。两者均在Flash存储测试前失败。

全部并行构建和WL专项结束后，使用 `--no-build` 逐个重跑raw完整专项，两者通过。180秒harness期限和所有来宾断言未改变。normal/runtime/ecosystem的ELF和BIN哈希逐一等于首轮，源码身份始终为干净0399da6。失败回执/runner日志在 `out/esp32-wear-final/attempt-concurrent/`，串行回执为 `out/esp32-wear-final/serial-retry-exits.json`。

串行成功不是根因证明。不能把失败确定归因于主机负载，也不能宣称调度稳定性已修复；该时序问题列为下一轮最高优先级，需要增加诊断和受控复现。

本批未改公共内核、内存、VFS服务或共享运行库实现，未重跑完整公共89组；最后89/89基线仍为bfd67ab，见 [ESP32_VFS_VALIDATION.md](ESP32_VFS_VALIDATION.md)。不扩大完整POSIX、射频、USB或实板断电结论。

## 正常固件与日志

| 固件ZIP | ZIP SHA-256 |
| --- | --- |
| `out/esp32c3/artifacts/nos-esp32c3-raw-0399da6.zip` | `ddeb5d25e551d4c80dc611378db383315bab527cb3490d56ed8516da1bebe893` |
| `out/esp32s3/artifacts/nos-esp32s3-raw-0399da6.zip` | `d98aecaa734b2729801f4e16c47d7c6ec8481ba1b1979edcde3841df2ce073f1` |
| `out/esp32c3/artifacts/nos-esp32c3-wl-0399da6.zip` | `2958eacd3a682f12a0323c995b103d0da941781a8c0ad94d0e9984c4f4863b64` |
| `out/esp32s3/artifacts/nos-esp32s3-wl-0399da6.zip` | `322281bf460eb0c4326d8361c3e26eaa2dc3d1ed35864b665f30b83ab715a914` |

- esp32c3-raw BIN：313472字节，SHA-256 `3a32777b595071ea8ba70393e4bf98b7eb947929a1528bfb9e38be278761904f`。
- esp32s3-raw BIN：320352字节，SHA-256 `f7bd1b6bc03d0bf1823ae34da8da77aed95936a65372be41b08effe7b4221880`。
- esp32c3-wl BIN：320384字节，SHA-256 `c689359e1fb7e5f617692fca0c660c9332b66d37408e73f187d85089b2061a5f`。
- esp32s3-wl BIN：326528字节，SHA-256 `ced50a42e4795fa4db0060d56c1e53c389408bf6d9b2731f8b4e0cbfb03d9ea3`。

ZIP含正常BIN/ELF、原始bootloader、匹配分区/OTA初始化、flash_args、构建/依赖身份、实际UART和七阶段日志及报告；CRC、逐文件SHA256SUMS和烧录引用均已核验。不含压力回归代码或模拟eFuse。日志另存 `out/esp32-boot-logs/0399da6/`，按平台/格式/阶段区分，未合成或清理。

raw subtype0x40与WL subtype0x41不自动迁移，切换前备份板级数据并使用匹配的分区表。解压运行 `python -m esptool --chip esp32c3 -p PORT write_flash @flash_args`，S3替换芯片名。UART0为115200，C3 TX21/RX20，S3 TX43/RX44；ROM USB下载口可能不同于诊断串口。

用法与状态保护见 [ESP32_WEAR_LEVELING.md](ESP32_WEAR_LEVELING.md)。仍为全局16节点、每文件512字节的共享固件profile；后续项见 [ESP32_NEXT_STEPS.md](ESP32_NEXT_STEPS.md)。
