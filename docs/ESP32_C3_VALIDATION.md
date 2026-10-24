# ESP32-C3 / S3 启动与 nshell 验证

<!-- Copyright (C) 2026 NOS contributors -->

最新被测实现为干净提交 `afb0818f916e788771668121c010f8085f53d382`。五个 runner 均实际退出0；两个芯片专项开始/结束身份相同，源码、文档及构建输入在测试期间冻结。本文件是结束后的结果补录。

| 检查 | 结果与报告 |
| --- | --- |
| C3 三种固件 | normal/runtime/ecosystem 构建、RISC-V ELF/port 审计、真实 `esp32c3` QEMU 通过；`out/esp32c3/nshell-validation/reports/1792884401549386511/results.json` |
| S3 三种固件 | 相同范围、Xtensa ELF/port 和真实 `esp32s3` QEMU 通过；`out/esp32s3/nshell-validation/reports/1792884403539264806/results.json` |
| nshell parser/editor | ASan/UBSan/LSan，解析/编辑各100000次及10000次带引号往返；`out/microkernel/host-shell/1792884401620680125/results.json` |
| 设备框架 | 生命周期、资源和外设契约 sanitizer 通过；`out/microkernel/host-drivers/1792884403701412671/results.json` |
| 受管理服务 | 失败保留/重试、回滚、重入、四线程4000次调用通过；`out/microkernel/host-managed-devices/1792884401460679445/results.json` |

普通固件常驻 shell，不再从 `app_main()` 返回。两款芯片各通过15项实际 UART0 命令/编辑交互：带引号 echo、版本、help、原生统计、任务表、控制台资源归属、退格、历史调用/列表、错误引号、超长输入、Ctrl+C、未知命令、VFS未绑定边界及恢复。测试按请求截取响应，不能把输入回显或早先启动日志算作成功。

runtime19个、ecosystem21个必需标记仍齐全。C3 的16槽/S3的24槽任务池耗尽与回滚、40轮任务栈回收，以及设备域剩30槽的耗尽/销毁回收均通过。SDK 原 tasks.c 未编译；原始 RISC-V/Xtensa 上下文代码与 NOS select_task 实际链接。C3不链接不存在的OTG后端，生态入口数分别为26/35。无线/USB宿主故障模型、组件管理器配置也包含在专项内。

六次QEMU均退出0，全部检查后由 harness 主动终止，普通固件未自行退出或关机。生态镜像使用虚拟出厂校准eFuse；SDK/模拟器源码未改。未从这些结果推断射频、USB或实物总线已验证。接口与边界见 [C3平台](ESP32_C3.md) 和 [nshell](ESP32_NSHELL.md)。

## 原始日志与正常固件

| 目标 | 原始启动/交互日志 | 固件归档及 SHA-256 |
| --- | --- | --- |
| C3 | `out/esp32-boot-logs/afb0818/esp32c3-startup-nshell.log` | `out/esp32c3/artifacts/nos-esp32c3-afb0818-nshell.zip`，`7a50c685f40df13cb01d2e7f7670140f0913052c9cec64a476a51525b9375329` |
| S3 | `out/esp32-boot-logs/afb0818/esp32s3-startup-nshell.log` | `out/esp32s3/artifacts/nos-esp32s3-afb0818-nshell.zip`，`59b4377390c49c00d6eb7d3aed0f7e4a6ae852cd4055d436a739ae4b37d750f6` |

C3 应用BIN为206272字节，SHA-256 `449519ba007f27632d488bf77552a757d8b9d6142ff162ea661b93f0d5dd5d46`；S3为219216字节，SHA-256 `14ac4ff731f7404ef2c48449386082ce04821914c320d10bf261fdf11953e356`。归档含正常BIN/ELF、完整烧录参数、构建身份、日志、报告及逐文件校验；CRC、烧录引用和每个文件哈希已核对。未包含修改NVS/OTA槽的回归镜像。证据汇总为 `out/esp32-nshell-evidence.json`。

归档身份是上述被测提交，后续文档补录不改写原报告。实板UART0与ROM USB下载端口可能不同，见nshell说明。

## 先前 C3 批次的边界

`eae2305` 的原始C3/S3专项各三次启动、19/21标记通过，但当时正常应用仅打印后返回，没有nshell。两份报告为 `out/esp32c3/final-validation/reports/1792880804051269760/results.json` 和 `out/esp32s3/c3-shared-validation/reports/1792880803475888451/results.json`。

该提交的公共完整验收 `out/microkernel/acceptance/1792880831657456881/results.json` 在41/89组通过后被中断，未产生成功退出收据，不能称为完整89/89。当前shell批次未修改公共内核/内存/运行库或通用shell源码，执行上述相关专项，没有重跑或宣称完整POSIX符合性。
