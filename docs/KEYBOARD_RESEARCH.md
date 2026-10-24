<!-- Copyright (C) 2026 NOS contributors -->

# nk60_v2：主流键盘项目调研与开发取舍

调研日期：2026-10-23。本文直接读取官方 GitHub API 与固定提交的源文档；“当前”仅指当日取回的默认分支状态，不代表发布日期、最新稳定版或项目间兼容性。VIA/Vial 官网在本环境返回代理 403，因此使用其官方网站仓库的 Markdown 原文。本文是设计依据与验收建议；实现状态和测试结果以本次适配文档为准。

没有可访问的 nk60_v2 实板。主机单元测试可以验证状态机、描述符和存储格式，不能证明 HC595 时序、USB 电气、LED 波形、电源电流或真实 flash 掉电行为。其他 STM32 板的 QEMU 启动也不能替代 nk60_v2 外设验证。

## 1. 官方来源与固定版本

| 项目 | 本次读取的官方仓库 / 分支 | 提交（完整 SHA） | 提交 UTC 日期 | 定位 |
| --- | --- | --- | --- | --- |
| QMK | [qmk/qmk_firmware](https://github.com/qmk/qmk_firmware/tree/180f394b489c238cb231c2bf8b080262155e03c0)，master | `180f394b489c238cb231c2bf8b080262155e03c0` | 2026-10-16 | C 固件、广泛有线键盘功能；按板启用功能 |
| ZMK | [zmkfirmware/zmk](https://github.com/zmkfirmware/zmk/tree/4dd95d7840eb3d5a3f59bb29fbf1ab054715ef85)，main | `4dd95d7840eb3d5a3f59bb29fbf1ab054715ef85` | 2026-10-09 | Zephyr、设备树行为配置、无线与低功耗 |
| KMK | [KMKfw/kmk_firmware](https://github.com/KMKfw/kmk_firmware/tree/8afe56905af4df3b9e4dcb0a21b670a1bd3aab18)，main | `8afe56905af4df3b9e4dcb0a21b670a1bd3aab18` | 2026-04-09 | CircuitPython 模块化配置；此版本 README 明确声明不再积极维护 |
| VIA 固件分发 | [the-via/firmware](https://github.com/the-via/firmware/tree/bc32102518533741af10b05ebbad38c4eef34997)，master | `bc32102518533741af10b05ebbad38c4eef34997` | 2026-08-13 | 预编译固件分发，不能当作独立 USB 协议规范 |
| VIA 官方文档 | [the-via/website](https://github.com/the-via/website/tree/cfdff9d8404547f55e0128ff278ea237c8a39bee)，master | `cfdff9d8404547f55e0128ff278ea237c8a39bee` | 2026-08-13 | 配置定义、协议与固件接入约束 |
| Vial | [vial-kb/vial-qmk](https://github.com/vial-kb/vial-qmk/tree/dd43959ae5c08d8a28d38a1acf7b04e86b14a344)，vial | `dd43959ae5c08d8a28d38a1acf7b04e86b14a344` | 2026-07-26 | QMK 分支与运行时配置 |
| Vial 官方文档 | [vial-kb/vial-kb.github.io](https://github.com/vial-kb/vial-kb.github.io/tree/4c996015fd64b8e8120ce66251e0634d8dc72dbd)，main | `4c996015fd64b8e8120ce66251e0634d8dc72dbd` | 2026-05-20 | 移植流程、物理解锁与敏感操作保护 |
| TinyUSB | [hathach/tinyusb](https://github.com/hathach/tinyusb/tree/04d3d6ee5df2f15c724da4054d65bad730b6b53d)，master | `04d3d6ee5df2f15c724da4054d65bad730b6b53d` | 2026-10-22 | USB 设备类实现与示例，作为 HID 行为核对参考 |

仓库提交时间不同于文档中每一项功能的引入时间。尤其 QMK 的 `usb_nkro.txt` 含 2010 年操作系统测试记录，不能把这些记录当作现代 Windows/macOS/Linux 或 UEFI 的兼容性验收。

## 2. 功能对比与 nk60_v2 取舍

| 能力 | 官方项目中的实际设计 | nk60_v2 建议 |
| --- | --- | --- |
| 扫描 / 消抖 | QMK 提供逐键、逐行、全局及 eager/defer 策略；其默认为全局 `sym_defer_g`。ZMK 默认类似逐键 `sym_defer_pk`，按键持续稳定才接受变化；扫描周期影响实际延迟。[Q1][Z1] | HC595 原始扫描与消抖解耦；采用逐键、按时间、按下/释放对称的稳定时间消抖。保留参数与统计，不把固定延时当作性能目标。 |
| 层 / 透明键 | QMK 从最高活动层向下查找，透明键透传；层操作包含瞬时、切换等。[Q2] | 恢复原始 DEF/FN/PN 布局；区分透明与禁用。首次按下绑定动作，释放时不重新按当前层查找。 |
| 双功能键 | QMK tap-hold 有明确的中断策略；ZMK 有 hold-preferred、balanced、tap-preferred 等 flavor；KMK 同样区分首次按下或释放中断。[Q3][Z2][K1] | 支持可配置 tap/hold 超时及一种写清楚的中断策略，分别测快速连打、交叠按键、超时边界和 modifier 释放；不宣称完全复刻其他项目的所有策略。 |
| Combo / 一次性动作 | ZMK combo 在正常 keymap 前处理，支持位置、超时、层限制、部分或完全重叠；QMK 有独立 combo 配置。[Z3][Q4] | 候选键在决策前延迟发送，失败后按顺序回放；重叠优先级和释放策略必须确定。一次性 modifier/layer 需有消费、取消、超时与 reset 规则。 |
| 宏 | QMK 有编译期及动态宏；KMK dynamic sequences 文档明确录制内容在重启后丢失。[K2] | 有界事件队列、非阻塞定时播放、可取消、重入与容量限制；持续按住的键由所有者计数，取消宏不能错误释放实键。持久宏要显式定义并受配置写入保护。 |
| HID / NKRO | QMK 资料讨论 bitmap NKRO 与 boot 兼容性；TinyUSB 当前 HID 类实现分别处理 report、idle、protocol 请求。[Q5][T1] | Boot keyboard 的固定 8-byte / 6KRO 语义与 NKRO report 分开验证；溢出不能悄悄丢键。增加 consumer usage、主机锁灯输出。真实 BIOS/UEFI 兼容必须另测。 |
| 在线改键 | VIA 使用动态 keymap、宏、非易失配置和 JSON 键盘定义；Vial 在 QMK 分支提供运行时配置；ZMK Studio 提供 USB/BLE 改键且有额外 RAM 开销。[V1][V2][L1][Z4] | 先建立稳定的 NOS 配置结构、能力查询、版本与错误码。VIA/Vial 是完整协议与 GUI 兼容工作，不能由一个 Raw HID endpoint 代表完成。 |
| 持久化 / 磨损 | QMK 提醒 EEPROM 寿命和布局问题；ZMK 使用 settings 子系统，部分设置延迟保存，默认保存防抖为 60000 ms。[Q6][Z5] | 使用已存在的 W25Q 存储能力前先探测实际 ID/容量；版本、长度、CRC、双槽或日志、最后提交标记、延迟/显式保存。CRC 只检测损坏，不能当作认证。 |
| 配置安全 | Vial 默认锁定，需用户按指定实键数秒解锁；宏写入、跳 bootloader 和分配 RESET 受保护；matrix-state 命令即使 insecure 也禁用。[L2] ZMK Studio 要求加入 unlock 行为。[Z4] | 改宏、恢复出厂、重启升级等能力必须声明权限；解锁使用物理输入并有超时，reset/unplug 后锁定。不要暴露可远程读取逐键状态的接口。 |
| RGB / 显示 | QMK RGB Matrix 提供效果与映射；ZMK 将 lighting、display、低功耗分开；KMK 有独立 RGB、display 和 power 模块。[Q7][Z6][K3] | 恢复 61 灯的布局、亮度、基础效果与层/锁灯指示；效果低优先级，不阻塞扫描/USB。旧“display”实际是 RGB 动画服务，不能据此声称板上有屏幕。 |
| Suspend / wake | TinyUSB 示例配置可声明 remote-wakeup；ZMK 区分 idle、sleep 与 hard-off，不同硬件保留能力不同。[T2][Z6] | USB suspend 时关闭 LED 电源并暂停昂贵动画；remote-wakeup 只在 host 允许时执行，必须满足 USB 时序。缺少实现时不要在描述符宣传支持。 |
| Bootloader / 恢复 | VIA 文档把启动按键恢复、EEPROM reset 和 bootloader 作为完整接入流程；Vial 将 bootloader 跳转列为敏感能力。[V2][L2] | 保留可靠 SWD 恢复路径；软件升级入口需核对具体 MCU 的系统 ROM 地址、时钟/中断/USB 清理及启动方式；先完成手动实板验收再开放远程命令。 |
| 无线 / 分体 | ZMK 与 KMK 的无线能力依赖 BLE 控制器、供电和对应栈；KMK 明确依赖 CircuitPython 在所选控制器上的 BLE 能力。[K4] | 现有 STM32F401 有线板没有被证实存在射频、电池或充电硬件；BLE、无线分体及电池功能不能仅通过本次软件适配实现。 |

### VIA / Vial 的兼容边界

VIA 官方 [Configuring QMK][V2] 当前说明：协议 12 及以前使用旧 `RGB_*` 键码；协议 13 使用 `UG_*`/`RM_*`，并要求可识别的 QMK keycode-version 响应，否则客户端在 Errors 页面报错并拒绝初始化改键。定义还依赖 USB VID/PID、矩阵尺寸、物理布局和配置菜单一致。[Specification][V1] 所称“官方支持”是其自身 QMK 与仓库接入流程，不能据此推断独立 NOS 固件不能实现协议，也不能省略客户端联调。

因此后续适配应选择并冻结客户端/协议版本，实现命令集和失败响应、动作与 QMK 键码的转换、动态层/宏/lighting 存储、JSON 定义、边界检查和真实客户端测试。Vial 还需处理其额外命令、键盘标识/定义、解锁与 GUI 行为，不能简单复用 VIA 名称。USB HID transport、NOS 自定义配置协议、VIA 兼容与 Vial 兼容应分别报告。

## 3. 历史 nk60_v2 的可证实硬件基础

依据 NOS 历史提交 `4c31189ffe24278449a12d7fd1dad11b03c98f09`；以下是源码证据，电平、器件型号及板级装配仍需原理图或实板确认。

- [按键驱动][N1] 实际读取 **61 个键**，通过 **72 位 HC595 选择链**，PB12 为 latch/enable、PB13 为 clock、PB15 为串行数据、PB14 为共享输入。不是常见矩形 row×column 阵列，不能直接套用其扫描或 ghost 检测逻辑。
- HC595 选择位置含空位，逻辑键顺序也不是简单递增；必须保留映射并对 72 个 selector 位置及 61 个逻辑键做覆盖检查。未见完整电气证据时不能声称存在某种二极管阵列。
- 历史消抖扫描两次，中间阻塞 10 ms，结果按位 AND。新驱动应输出原始状态，时间消抖交由独立状态机，避免阻塞设备服务。
- 历史布局有 DEF、FN、PN 三层。旧代码已经在首次按下时保存选中的键值；新实现需保留这一正确性质，不能重新引入跨层释放错误。
- [LED 驱动][N2] 为 61 个 RGB 灯，PB0 控制电源；每个数据 bit 编成 SPI 字节 `0xc0` 或 `0xf0`，使用 DMA1_Stream4，完整缓冲为 `61 × 3 × 8 = 1464` bytes。[display 服务][N3] 将其映射成 14×5 逻辑画布做动画，没有证明存在 LCD/OLED。
- [W25Q 驱动][N4] 使用 SPI1、PB2 片选，包含多个容量型号的识别表。这不证明每块实板都装了同一容量，不能根据表中最大型号分配存储地址。

61 个 RGB 灯的峰值耗电可能大于普通 USB 口承载能力，实际值取决于灯珠型号、限流和板上供电。固件应从保守亮度启动，遵守配置描述符的电流预算，支持全局亮度/电流上限和 suspend 断电；不能仅凭灯珠数量给出未经测量的整板额定电流。

## 4. 适合本次交付的范围与后续优先级

### 本次有线移植基线

1. STM32F401 启动、时钟、链接地址、滴答、GPIO、72→61 键位映射、独立逐键消抖；保持 kernel 仅提供机制，键盘策略与配置放服务层。非 MMU Cortex-M 的保护边界必须准确描述。
2. USB FS 枚举与 HID control 请求，boot/6KRO、NKRO、consumer、主机锁灯、配置/reset/suspend 状态；异步发送需背压，最终 key-up 不能被忙状态吞掉。
3. DEF/FN/PN 默认键位、透明键、瞬时/切换层、press-time binding、共享 modifier 引用计数；有界 tap-hold、combo、one-shot、宏按实现的明确语义测试。
4. LED 基础效果、亮度和层/锁灯反馈，W25Q 探测与配置存储；不通过高频 flash 擦写模拟即时保存。测试数据损坏、过期格式、未知版本和恢复默认。
5. 可复现 ELF/bin/map、烧录/诊断工具、主机测试与实板验收清单。交付时分别列出自动化通过项、未执行实板项和暂未实现项。

以上是建议的完整基线，不是本文对每项已经交付的声明。若某项本次仅完成主机模型而未接入固件，必须写明。

### 后续优先级与完成条件

| 优先级 | 工作 | 具体完成条件 |
| --- | --- | --- |
| P0 | 接通实板与恢复链路 | SWD 可恢复；实测 HSE/PLL/USB 48 MHz、全部 61 键、USB 重连与三种桌面系统；记录板版本和固件哈希。 |
| P0 | USB 稳定性与延迟 | 使用 host/USB trace 检查枚举、重复配置、control stall、忙/超时、长按、同时按键、suspend/resume；验证 key-up 不丢、锁灯不串、正常打字延迟分布。 |
| P0 | 供电与外设时序 | 示波器/逻辑分析仪核对 HC595 settle、LED SPI/DMA 波形、W25Q 时序；测最大允许亮度及 suspend 电流，验证供电不足时可恢复。 |
| P1 | 掉电安全配置 | 在擦除、payload、校验、commit 各阶段故障注入；旧槽仍可用，下一次启动能选中最新完整提交；保存合并和磨损计数可观测。 |
| P1 | 有界运行时改键 | NOS 配置协议具版本和能力协商、长度/索引/动作校验、物理解锁、session timeout、默认拒绝敏感命令；提供导入导出与恢复默认。 |
| P1 | VIA 或 Vial 正式兼容 | 固定协议及客户端版本，提供完整定义和键码转换；真实 GUI 完成读取、改键、宏、lighting、保存/重启恢复与锁定失败路径。分别列兼容范围。 |
| P1 | 升级路径 | 校验具体 F401 ROM bootloader 与线路条件；实测物理入口、失效恢复和升级后配置迁移，再考虑经授权的主机触发。 |
| P2 | 高级输入与体验 | 扩展 tap-hold 策略、重叠 combo、tap dance、leader、caps word、mouse keys，逐项给出确定的仲裁规则及资源上限。不要用宏隐藏不可靠的基本输入。 |
| P2 | LED 与布局编辑器 | 自定义动画/层色、布局预览、亮度预算、帧率上限；效果不阻塞输入。真正屏幕或旋钮必须以新增硬件为前提。 |
| P3 | 无线与分体新板 | 先确定无线 MCU/模块、电池/充电、电源开关和通信接口，再移植 BLE、配对、安全、断链状态恢复与功耗策略。 |

## 5. 许可和复用边界

本次按官方文档独立设计动作状态机、协议和硬件适配，不复制 QMK/Vial 的 GPL 固件代码。已读取的 QMK 根 LICENSE 为 GPLv2，ZMK 根 LICENSE 为 MIT；KMK 文档与源码许可还需在实际复用前定位对应文件核对。NOS 根目录未发现明确的统一 LICENSE，不能默认允许混入任何来源的代码。将来引入 TinyUSB、第三方库或直接移植功能时，应先核对相应文件许可并保留要求的声明；“参考开源项目”不能替代来源与许可核对。

## 6. 烧录镜像校验的独立自动化补充

已在本环境运行 `python3 tests/test_nk60_flash_tool.py -v`，17 个测试全部通过。测试用标准库生成 ELF32 fixture，覆盖 ARM/Thumb、program header、文件截断、应用分区越界、重叠段、向量表、栈、重置入口以及可执行代码的虚实地址一致性；同时允许合法的 RAM 数据段 VMA / flash LMA。测试没有调用 OpenOCD，也没有接触硬件，不代表固件启动或烧录已实测。其余固件构建及设备验证结果另见适配验收记录。

## 7. 固定源码链接

以下链接均指向本次实际读取的官方固定提交。

[Q1]: https://github.com/qmk/qmk_firmware/blob/180f394b489c238cb231c2bf8b080262155e03c0/docs/feature_debounce_type.md
[Q2]: https://github.com/qmk/qmk_firmware/blob/180f394b489c238cb231c2bf8b080262155e03c0/docs/feature_layers.md
[Q3]: https://github.com/qmk/qmk_firmware/blob/180f394b489c238cb231c2bf8b080262155e03c0/docs/tap_hold.md
[Q4]: https://github.com/qmk/qmk_firmware/blob/180f394b489c238cb231c2bf8b080262155e03c0/docs/features/combo.md
[Q5]: https://github.com/qmk/qmk_firmware/blob/180f394b489c238cb231c2bf8b080262155e03c0/docs/usb_nkro.txt
[Q6]: https://github.com/qmk/qmk_firmware/blob/180f394b489c238cb231c2bf8b080262155e03c0/docs/feature_eeprom.md
[Q7]: https://github.com/qmk/qmk_firmware/blob/180f394b489c238cb231c2bf8b080262155e03c0/docs/features/rgb_matrix.md
[Z1]: https://github.com/zmkfirmware/zmk/blob/4dd95d7840eb3d5a3f59bb29fbf1ab054715ef85/docs/docs/features/debouncing.md
[Z2]: https://github.com/zmkfirmware/zmk/blob/4dd95d7840eb3d5a3f59bb29fbf1ab054715ef85/docs/docs/keymaps/behaviors/hold-tap.mdx
[Z3]: https://github.com/zmkfirmware/zmk/blob/4dd95d7840eb3d5a3f59bb29fbf1ab054715ef85/docs/docs/keymaps/combos.md
[Z4]: https://github.com/zmkfirmware/zmk/blob/4dd95d7840eb3d5a3f59bb29fbf1ab054715ef85/docs/docs/features/studio.md
[Z5]: https://github.com/zmkfirmware/zmk/blob/4dd95d7840eb3d5a3f59bb29fbf1ab054715ef85/docs/docs/config/settings.md
[Z6]: https://github.com/zmkfirmware/zmk/blob/4dd95d7840eb3d5a3f59bb29fbf1ab054715ef85/docs/docs/features/low-power-states.md
[K1]: https://github.com/KMKfw/kmk_firmware/blob/8afe56905af4df3b9e4dcb0a21b670a1bd3aab18/docs/en/holdtap.md
[K2]: https://github.com/KMKfw/kmk_firmware/blob/8afe56905af4df3b9e4dcb0a21b670a1bd3aab18/docs/en/dynamic_sequences.md
[K3]: https://github.com/KMKfw/kmk_firmware/blob/8afe56905af4df3b9e4dcb0a21b670a1bd3aab18/docs/en/power.md
[K4]: https://github.com/KMKfw/kmk_firmware/blob/8afe56905af4df3b9e4dcb0a21b670a1bd3aab18/docs/en/Officially_Supported_Microcontrollers.md
[V1]: https://github.com/the-via/website/blob/cfdff9d8404547f55e0128ff278ea237c8a39bee/docs/specification.md
[V2]: https://github.com/the-via/website/blob/cfdff9d8404547f55e0128ff278ea237c8a39bee/docs/configuring_qmk.md
[L1]: https://github.com/vial-kb/vial-kb.github.io/blob/4c996015fd64b8e8120ce66251e0634d8dc72dbd/docs/docs/porting-to-vial.md
[L2]: https://github.com/vial-kb/vial-kb.github.io/blob/4c996015fd64b8e8120ce66251e0634d8dc72dbd/docs/docs/security.md
[T1]: https://github.com/hathach/tinyusb/blob/04d3d6ee5df2f15c724da4054d65bad730b6b53d/src/class/hid/hid_device.c
[T2]: https://github.com/hathach/tinyusb/blob/04d3d6ee5df2f15c724da4054d65bad730b6b53d/examples/device/hid_composite/src/usb_descriptors.c
[N1]: https://github.com/wkcs/nos/blob/4c31189ffe24278449a12d7fd1dad11b03c98f09/drivers/key/nk60_v2_key.c
[N2]: https://github.com/wkcs/nos/blob/4c31189ffe24278449a12d7fd1dad11b03c98f09/drivers/misc/nk60_v2_led.c
[N3]: https://github.com/wkcs/nos/blob/4c31189ffe24278449a12d7fd1dad11b03c98f09/app/display/nk60_display_server.c
[N4]: https://github.com/wkcs/nos/blob/4c31189ffe24278449a12d7fd1dad11b03c98f09/drivers/spi/nk60_v2-w25qxx.c

## 8. 逐键RGB与产品能力补全

本次继续读取上述固定提交的QMK RGB Matrix/Caps Word/Grave Escape、ZMK hold-tap与Vial运行时配置源码。取舍是：先让61颗LED可独立配置并可靠保存，提供标准60%物理布局、灯效/休眠/预算、Grave Escape与Caps Word，再预留模块化扩展。QMK的可配置动效、指示覆盖和timeout用于功能参考；Vial的动态配置及物理解锁用于授权交互参考；ZMK明确区分hold-preferred/tap-preferred/balanced，NOS保留当前确定语义，未借相同名字冒称实现所有策略。实现独立编写，不复制这些项目固件代码。

| 对照项 | 本次实现 | 保留后续 |
| --- | --- | --- |
| QMK逐键RGB | 61个独立颜色、位置波浪、呼吸/彩虹/响应、速度/锁灯/限流/睡眠、共同持久化 | 更多动画、可视化编辑器，不支持任意动画脚本 |
| QMK 60%使用习惯 | Grave Escape、Fn+Caps Word及已有层/媒体/宏 | combo、tap dance、leader、mouse keys需独立仲裁 |
| Vial动态设置与安全 | NLP独立客户端、布局预览数据、物理解锁绑定当前peer、整帧原子提交 | 不兼容Vial/VIA GUI/Raw HID，兼容层需独立验收 |
| ZMK行为与扩展 | 保持输入/灯光/载体/能力边界，未知模块不自动注入 | 多tap-hold策略、独立显示/旋钮、无线和分体需要具体模块与协议profile |
| SPI NOR | 现有双槽版本化扩展、旧API保留、未知schema保护、分区可查询 | 资源/多profile仓库须显式分区迁移，不自动格式化历史内容 |

详细接口与扩展完成条件见 [NK60能力](NK60_CAPABILITIES.md)。以下为本次实际取回的固定源码摘要：

| 来源 | 内容SHA256 |
| --- | --- |
| [qmk_rgb](https://raw.githubusercontent.com/qmk/qmk_firmware/180f394b489c238cb231c2bf8b080262155e03c0/docs/features/rgb_matrix.md) | `921a7d4e8c2b2a282b2f76a4592ec6a282abb9b07044264adc43f19fbd943160` |
| [qmk_caps](https://raw.githubusercontent.com/qmk/qmk_firmware/180f394b489c238cb231c2bf8b080262155e03c0/docs/features/caps_word.md) | `4efccb00b1f49166e3ae8cf6ecf6971d88c677f61f7269ff1b35473ebef528f2` |
| [qmk_grave](https://raw.githubusercontent.com/qmk/qmk_firmware/180f394b489c238cb231c2bf8b080262155e03c0/docs/features/grave_esc.md) | `f43a22af191473350cab893d6c011cc0fe337f398e59810773bc892d16599c7b` |
| [zmk_holdtap](https://raw.githubusercontent.com/zmkfirmware/zmk/4dd95d7840eb3d5a3f59bb29fbf1ab054715ef85/docs/docs/keymaps/behaviors/hold-tap.mdx) | `7e5bdaba59800fad7564fb023645e3eb42b2a31a1814fabfb8dd75f21d6250e2` |
| [vial](https://raw.githubusercontent.com/vial-kb/vial-qmk/dd43959ae5c08d8a28d38a1acf7b04e86b14a344/quantum/vial.c) | `713c5a9ba680a3bd39f25fea00cbff66d9506ef8951be33b36aed50b0cc4b52b` |
