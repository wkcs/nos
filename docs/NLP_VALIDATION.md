# NLP v1 实现验收记录

<!-- Copyright (C) 2026 NOS contributors -->

日期：2026-10-23。实现分支：`dev/nlp-v1-implementation`。本报告在全部冻结验证结束后生成；未随后修改固件、工具、测试或构建输入。协议仍是实验实现，未进行实板或独立安全审计。

## 冻结身份

验收基于父提交 `1476b0b1095636c32533807e1cc5f0ab1a71dced` 加本分支实现修改。boards、host NLP 与两次 QEMU 矩阵均记录前后源码身份一致。boards 记录的 tracked diff SHA256 为 `5db6f079a39056ed61446ebd8cfa6e2b8072774e6df2e5e0172353b9e2babcbd`；完整 source 对象（包含未跟踪实现文件摘要）按 JSON sort_keys 编码后的 SHA256 为 `27e3922b96adf01890c202cb8651b1573d9bda8fd151cbd37df9b6eb911c3769`。本报告及随后对第三方 LICENSE 行尾空白的格式整理不在该测试前身份内；许可证文字与条款未变。

## 执行结果

| 验证 | 结果 | 本地原始记录 |
| --- | --- | --- |
| `tests/acceptance.py --section boards --require-frozen-source` | 10/10通过，源码未变 | `out/microkernel/acceptance/1792768446383074868/results.json` |
| `tests/host_nlp_protocol.py` | 17组格式/注册表/文档检查通过 | boards 的 `nlp_reference_wire_registry.log` |
| `tests/host_nlp.py` | 128/1024两配置、13组C测试、各30000异常输入、7固定wire向量、SHA256/独立Noise比对、密钥生成及独立TCP互通通过 | `out/microkernel/host-nlp/1792768458722340712/results.json` |
| `tests/qemu_matrix.py --nlp-test --timeout 180` | 原八平台/C库组合全部通过，共886项guest检查 | `out/microkernel/reports/1792768442507606104/results.json` |
| `tests/qemu_matrix.py --nlp-test --platform stm32 --libc minimal --mpu --timeout 180` | 92项guest检查通过 | `out/microkernel/reports/1792768442516777459/results.json` |
| NK60 MPU=0/1实际交叉构建与镜像检查 | 两种固件均通过128KiB Flash、64KiB SRAM和16KiB用户区约束，协议/散列函数位于用户text | `out/microkernel/nk60-firmware/1792768577361823272/results.json` |
| NK60 Cortex-M启动专项 | 两种MPU配置通过；GDB明确跳过未建模F401时钟初始化 | boards 的 `nk60_cortex_m_startup_stubbed_clock.log` |
| Native C工具、注册头一致性、`git diff --check` | 通过 | `make -f apps/nlp-host/Makefile`、`scripts/gen_nlp_registry.py --check` |

原八组合为 ARM minimal/newlib、ARM64 minimal/musl、RV64 minimal/musl、STM32 minimal/newlib。每个镜像必须出现5项 NLP 实际 C 用户态探针、原有必需标记以及成功退出；STM32 使用原版 `netduinoplus2`。这次没有改动内核公共核心、内存、运行库或 syscall ABI，没有将局部 boards/协议验证称为完整 POSIX/Linux 或全部历史验收。

Host sanitizers 使用 ASan/UBSan/LSan；回环测试在允许本地socket及LSan的执行环境运行。C测试实际调用协议、会话、capability及协调者/树路由源码；存储、屏幕、无线和多级链路由受控后端模拟。网络测试以独立 Python 客户端连接实际 C/OpenSSL 服务，验证双方UID排序下的握手、能力/拓扑/订阅、错误pin/降级/AEAD篡改拒绝，并启动真实主机PTY执行命令、取得退出状态7。没有将模型固件APPLY描述成已部署引导器。

NK60服务模型经模拟USB字节流走真实发现、只读配置、未解锁拒绝、物理解锁、完整SHA256配置上传/提交、重复提交和新peer不能继承授权。USB/DWC2模型覆盖复合描述符、EP2收发、背压及HID独立性。设备白名单模型验证只读工厂UID和精确EP2范围。

## 固件资源

| 配置 | bin字节 | ELF SHA256 | 用户静态结束 | MSP余量 |
| --- | --- | --- | --- | --- |
| MPU=0 | 78552 | ELF SHA256 `ccb8f98181f5fcb3f5d5291c9933373910a4d0e5fd1c5a1a917d9a247b660b4a` | `0x2000b7d0` | 18480字节 |
| MPU=1 | 79536 | ELF SHA256 `a087b773340b30bbac11a3037064933ace277df85d5ffb3c3f0fb296b4752c52` | `0x2000b7d0` | 18480字节 |

SHA栏是ELF摘要而非bin摘要，ELF包含调试信息，文件长度不能当作烧录长度。真实栈/队列高水位、USB供电与电气未测量。

## 尚需平台验收的边界

当前NK60暴露device/keyboard/log/keymap/lighting，使用明确物理信任，不广告安全Noise、shell、firmware或radio。NOR尾部16KiB的启动计数/配置布局必须在首次实板迁移前备份确认。STM32硬件熵、密钥存储、可信引导及真实断电恢复不由软件模型推断。

通用SDK已提供shell、固件、显示、输入和无线能力后端接口；NOS-native nshell、NK60安全OTA、具体屏幕/旋钮/蓝牙/Wi-Fi模块与多物理端口装配仍需后端/硬件工作。CAN匿名自动枚举、拓扑推送和运行中能力增删仍未实现；power/time为保留类型。详见 [实现状态](protocols/nlp/implementation.md) 与 [主机工具](../apps/nlp-host/README.md)。

当前源码与构建已无旧NDK协议或兼容适配器，仅保留历史说明与Git历史。原始工作树 `/workspace/nos` 的1230项既有删除未修改或暂存。
