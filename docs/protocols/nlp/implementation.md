# NLP 的实现、接入与验收

<!-- Copyright (C) 2026 NOS contributors -->

NLP v1 是实验实现，已接入 NK60 用户态键盘服务。协议栈、独立主机客户端与模型测试可运行；没有可访问实板，不据此宣称 USB 电气、无线模块、安全 OTA 或完整 NOS nshell 已验收。

## 1. 源码入口

| 位置 | 职责 |
| --- | --- |
| `include/nos/nlp/` | 可移植 C API、明确的 wire 字段与固定资源上限 |
| `servers/nlp/core/` | codec、TLV、可靠会话、共享池、RPC、发现、权限、Noise adapter、启动日志 |
| `servers/nlp/transports/` | USB/TCP 长度分帧、UART COBS、I²C/SPI mailbox、CAN classic/FD |
| `servers/nlp/routing/` | 有根树、源子树检查、协调者两阶段路由安装、目录、租约与撤销 |
| `servers/nlp/capabilities/` | 标准能力处理器及平台后端接口 |
| `servers/keyboard/link*` | NK60 USB、实际键盘引擎/配置/灯光与 NLP 的连接 |
| `apps/nlp-host/` | Linux C/OpenSSL 回环服务与可选真实 PTY 后端 |
| `scripts/nlp_host.py`、`nlp_crypto.py` | 独立 Python 客户端、安全握手、配置、拓扑与终端 |
| `protocols/nlp/v1/` | 注册表、固定向量；`gen_nlp_registry.py` 生成 C 注册头 |

业务均在用户态；未给内核增加远程 parser、命令执行或任意 Flash/MMIO 接口。C 栈不依赖 NOS syscall、堆、socket 或硬件 HAL。发送回调只接收完整 PDU；适配器持有复制后的有界发送队列，返回 BUSY 时不得消费调用者的消息。

应用先初始化 endpoint/config，注册能力，再由链路分帧回调调用 `nlp_endpoint_receive()`，周期调用 `nlp_endpoint_poll()` 及能力 poll。`now_ms` 是可回绕 u32 单调毫秒，相邻 poll 间隔必须小于半周期；endpoint 累加成 u64 提供事件时间。断链必须调用 `nlp_endpoint_disconnect()`，不能只丢弃 socket。错误码为负的注册状态；wire status 是其正值。

多端口程序为每个直接邻居提供一个 endpoint；连接共享 coordinator/router 的 callbacks、身份、端口和权限策略。`nlp_fabric_*` 实现异步 PREPARE/COMMIT/REVOKE 和带快照 revision 的目录，`nlp_route_control()`/dispatch 负责注册的 wire 控制消息，应用负责将协调者的安装回调送往正确管理会话。当前 NK60 只连接一个 USB 对端，没有装配成多物理端口路由产品。

## 2. 已实现功能与后端边界

| 功能 | 当前交付 | 仍需平台工作 |
| --- | --- | --- |
| Wire/会话/RPC | CRC32C、严格长度/TLV、累计 ACK/绝对信用、重传、去重、重放窗口、关闭释放、结果缓存与 OUTCOME_UNKNOWN | 链路排队与工作量预算 |
| 安全 | Noise XX、JOIN HMAC、按能力/方向/通道派生 AEAD；OpenSSL 与独立 Python 双向互通 | 固定公钥配对 UI、设备熵、密钥存储、独立安全审计 |
| 发现/级联 | HELLO/JOIN、目录分页、GET_TOPOLOGY、8层两阶段路由、租约、父节点消失清理 | 多端口物理集成；拓扑推送、运行中能力增删尚未实现 |
| keyboard/input | 状态快照、短租约、全释放屏障、来源合并、按钮/旋钮提供端、owner | 扩展模块到 NK60 动作/层/宏的持久映射 UI 与具体硬件 |
| keymap/lighting | SHA256 事务、generation、原子配置提交、持久状态；NK60 模式/亮度/RGB | 远程配置 GUI；逐键配色的实际电气/掉电实测 |
| log | UTF-8 边界、有界 ring、过滤、限速、可选可靠订阅 | 平台日志源接入与隐私策略 |
| display | bounds/CRC tile、progressive/atomic 后端、owner、断开取消 | 指定屏幕驱动与显存策略 |
| shell | 可靠字节流、尺寸、INT/TERM、退出与断线清理；主机真实 POSIX PTY | NOS-native nshell/VFS/TTY 后端；NK60 不注册 shell |
| firmware | 实际 Ed25519 签名验证接口、manifest/SHA256、兼容/回滚检查、持久 offset、恢复、APPLY gate | 实板 staging、bootloader 再验证、A/B 试启动/确认/回退；NK60 不注册 firmware |
| bluetooth/wifi | 类型化参数、加密授权、异步 job、凭据不回显 | 指定模块驱动、真实无线链路和功耗；不是 Wi-Fi/TCP 协议栈 |
| CAN/I²C/SPI | classic/FD 分段重组、超时、mailbox token/PEEK/ACK | 控制器驱动、电气仲裁、SPI CS 事务、transfer ID 重用 guard；CAN-COMMISSION-1 尚未实现 |

power/time 为保留类型。没有后端的能力不注册；不对不支持的操作返回伪成功。固件测试中的持久存储/启动后端为模型，并非已部署安全启动产品。

## 3. NK60 固件

复合 USB 保留 interface0 HID/EP81，新增 interface1 vendor class FF/subclass4E/protocol1，EP02 OUT 与 EP82 IN；完整配置描述符57字节。HID 和 NLP 独立排队、背压和 halt。NLP 不向操作系统自动注入按键，不制造第二个 HID 键盘。UART 继续运行原维护文本命令，不能直接对该 UART 运行 NLP 客户端。

NK60 在这条 USB 链路作为根节点1，提供 device、keyboard、log、keymap，keyboard_control，以及 RGB 可用时的 lighting。出厂 UID96 与固定前缀组成 UID128。外置 NOR 最后16KiB明确分配为两扇区启动计数日志、两扇区旧配置日志。未知非空启动日志扇区保留原内容并禁用 NLP；不能自动擦除旧数据。计数写入并读回校验后才启用链路；无法获得持久 boot 时保留 HID/UART，关闭 NLP vendor。

F401 没有经过验证的熵/crypto 后端，因此固件明确使用 PHYSICALLY_TRUSTED，不广告认证会话、shell、firmware 或 radio。初始仅允许发现、观察与日志。Esc+PN+Fn 按住两秒，将30秒 CONFIG_WRITE 权限授给当时连接的 UID/boot；更换对端、USB reset 或到期撤销。改键使用现有完整1056字节 canonical 配置，SHA256/语义检查后原子替换，返回易失/待保存状态；Flash 安全空闲保存规则仍生效。设备稳定 UID 和 CRC 日志不构成抗回滚安全存储。

## 4. 实际有界配置

| 项目 | 当前实现 |
| --- | --- |
| payload | 默认128，编译支持128..1024；完整加密 PDU 为 payload+68 |
| 每 endpoint 邻居 / 能力 / 会话 | 1 / 8 / 4（管理1+业务最多3） |
| 每 session 通道 / 窗口 / 在途 RPC | 2 / 默认1、最大2 / 1 |
| PDU 共享池 | 6槽，其中2槽控制类；每类 RX 留出至少1个 TX 槽避免响应死锁 |
| 目录 / 路由 / 端口 / 最大深度 | 16 / 16 / 4 / 8 |
| 在途协调者 admission | 1；短地址在 epoch 内不复用 |
| 日志 | 4条，payload128时记录数据512字节；最多16条/秒、burst1 |
| CAN 重组 | 2槽、100ms期限；FD 使用合法 DLC padding |

注册表 `mcu_profile` 保留设计目标，实际值以 `implementation` 和 C 头常量为准，不能把8个目标会话描述成当前固件容量。NK60 链接器强制前128KiB应用 Flash、64KiB SRAM、16KiB共享用户静态区及用户栈/MSP余量。实际占用由 `tests/nk60_firmware.py` 输出 ELF/map 验证，未测量真实板上的栈/队列高水位。

## 5. 可重复验收

```sh
python3 scripts/gen_nlp_registry.py --check
python3 tests/host_nlp_protocol.py
python3 tests/host_nlp.py
source /workspace/.nos-env/activate.sh
python3 tests/acceptance.py --section boards --require-frozen-source
python3 tests/qemu_matrix.py --nlp-test --timeout 180 --output-root out/microkernel/nlp-matrix
python3 tests/qemu_matrix.py --nlp-test --platform stm32 --libc minimal --mpu --output-root out/microkernel/nlp-mpu
```

Host NLP 测试在128/1024两配置运行实际 C 源码、ASan/UBSan/LSan、异常输入、真实8层 coordinator/router、签名固件状态机、独立 Noise/字节向量，并通过 loopback TCP 检查双方向安全握手、拒绝错误 pin/明文降级/篡改及真实 PTY。测试身份仅为临时确定向量；生产密钥使用 keygen。云端 traced sandbox 的 LSan 与本地 socket 测试需在允许它们的执行环境运行。

NK60 服务模型经模拟 USB 字节流执行真实 discovery、权限拒绝、物理解锁、SHA 配置上传/提交与新 peer 撤权。boards 还包含实际 USB/DWC2/设备白名单源码回归、两种 MPU 固件交叉构建和明确跳过 F401 时钟初始化的 Cortex-M 启动专项。

QEMU 的 `--nlp-test` 在真实用户态执行5个 C 栈探针：wire/CRC、UART/TCP分帧、CAN重组、SHA256、树路由提交/转发/过期；默认原八平台/C库组合及 STM32 MPU 使用 stock netduinoplus2。它不模拟真实 NK60 USB/NOR/键盘，也不验证安全握手或无线硬件。完整 POSIX/Linux 合规不由此次局部协议验收推断。

各 runner 将命令、必要标记、退出状态和身份/源码摘要写入 `out/microkernel/` 下独立结果目录。最终测试期间冻结源码、文档和构建输入；物理 USB 枚举、时序、真实断电恢复和独立安全审计单独验收。

## 6. 后续平台接入顺序

先实板验证复合 USB、持久 boot、键盘配置和供电，再为已选定的旋钮/屏幕接入 UART 或 mailbox。随后装配多端口协调者与输入映射，接入真实蓝牙/Wi-Fi模块及 CAN。NOS nshell 需先接入用户态终端后端；安全升级需先完成并验证板级 bootloader/分区/恢复入口。这些工作需要明确硬件与平台实现，不能由通用协议测试替代。
