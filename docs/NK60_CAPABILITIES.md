# NK60 60% 能力与扩展约定

<!-- Copyright (C) 2026 NOS contributors -->

NK60 当前硬件按 61 键 ANSI 60%、61 颗逐键 RGB、SPI NOR 和 USB 实现。物理坐标、扫描链、HID usage 和 LED ID 分开表示：改键不改变灯的位置，宽键不复制灯珠，外部旋钮也不挤入61位物理扫描集合。没有可访问实板，本次验收覆盖真实源码、模型I/O与固件交叉构建。

## 1. 当前可用能力

| 能力 | 行为 |
| --- | --- |
| 物理布局 | 五行14/14/13/12/8键，15u×5u，坐标精度1/4u；每个key ID恰对应一颗LED，包含长空格/Shift/Enter宽度 |
| 输入 | 独立防抖、4层、透明/瞬时/切换层、Mod/Layer Tap、One Shot、4个有界宏、媒体控制、6KRO/NKRO和全释放保护 |
| 60%常用行为 | 默认Esc为Grave Escape：普通Esc，Shift或GUI修饰时为Grave；同一扫描同时按下也正确。Fn+Caps切换Caps Word，字母/减号自动Shift，数字/退格/Delete继续，空格等终止，空闲5秒关闭 |
| 逐键灯光 | off、static、layer、breathe、rainbow、wave、reactive、custom；速度、RGB、亮度、每键颜色、Caps Lock/Caps Word指示 |
| 电源策略 | 未配置或USB挂起立即断灯；亮度0断灯；默认空闲300秒熄灯，按键恢复、持续按住不睡；允许配置0禁用空闲熄灯 |
| 输出限制 | 基础模式10帧/秒，启用规则可配置最高25帧/秒，渲染不分配堆；每通道输出上限16/255，统一经过估算电流预算，不允许远端绕过 |
| 配置保存 | 键位1056字节与灯光/规则624字节扩展共同双槽提交；旧记录默认补齐灯光设置，写失败保留上一完整代次 |
| 上位机 | NLP发现、布局、NOR布局、健康信息、改键、策略和61色整帧事务；HID仍是独立接口0 |

Caps Word是NOS明确子集，不声明兼容全部QMK选项：自动Shift绑定到该次按下并在释放时消失，不用操作系统Caps Lock；不改变宏、媒体或tap-hold的既有含义。旧配置不会被自动重映射，恢复默认才获得新的默认Esc/Fn+Caps绑定。

当前RGB电流是软件模型：每颗灯1mA静态、每通道满量程20mA。配置LED预算100..350mA，默认300mA，整板还需预留控制器等功耗；该数值不等于实际测量，未知灯珠/供电需实板校准。所有模式、逐键帧和锁灯覆盖都经过同一亮度与预算限制。亮度0、off和挂起不会被锁灯指示偷偷点亮。

## 2. SPI NOR 与兼容

容量从实际JEDEC探测获得，不按历史驱动支持的最大型号分配。设容量为C：

| 区域 | 范围 | 当前用途 |
| --- | --- | --- |
| preserved/unallocated | `[0,C-16384)` | 保留原内容；未创建文件系统/资源仓，也没有远程裸读写或自动擦除 |
| boot journal | `[C-16384,C-8192)` | NLP持久启动计数，两个4KiB扇区 |
| configuration journal | `[C-8192,C)` | 键位与灯光共同配置，两个4KiB扇区 |

配置记录v1仍为24字节头+1056载荷。记录v2为24字节头+1056键位+最多640字节opaque扩展；头中的payload长度、CRC和最终commit覆盖整个记录。原224字节`KBL1`扩展包含版本、速度、指示开关、空闲/电流策略、静态颜色和61×RGB，尾部25字节为零保留。新记录再附400字节KLFX规则程序，总偏好624字节；224字节旧偏好保留并关闭规则。公开keymap canonical wire仍是1056字节。

新代码的旧save API会保留已存在的扩展，不会因只改键就丢失灯光。未知扩展格式原样保留，并禁止自动/显式保存；物理解锁后显式`defaults`才重置全部设置并允许再次保存。此行为不是旧固件二进制的前向兼容保证；降级固件前应备份配置。

保存仍合并修改，60秒静默且物理键全松开、宏结束、报告队列排空后执行；`--save`请求提前排队，返回SAVE_PENDING，不冒称已持久。进度回调继续处理扫描与USB。没有占用新的尾部扇区，也没有自动把NOR其余空间当空白盘。

## 3. NLP接口

设备管理仍用cap0；NK60本次注册keyboard1、log2、keymap3、lighting4、keyboard_control5。客户端按类型UUID发现，不能依赖这些实例号。新增 `keyboard_control` major1/minor0，只读，需要DISCOVER：

| 操作 | 格式 |
| --- | --- |
| GET_INFO(1) | 空请求；响应32字节：schema/key数/LED数/宽/高各u16，单位分母u8、层数u8，扫描目标us/Flash容量/active_features/reserved_features/generation各u32 |
| GET_LAYOUT(2) | 请求first:u16,max_count:u16,reserved:u32=0；响应first/count/next/total各u16，后接12字节条目key/LED/x/y/width/height各u16；next0结束；布局schema1固定，不随改键变化 |
| GET_STORAGE(3) | 响应56字节：capacity:u32,sector:u16,page:u16,count:u8,reserved[3]；最多3个12字节分区kind:u16,flags:u16,offset:u32,bytes:u32；最后generation:u32,dirty/schema_locked/saving/reserved各u8 |
| GET_DIAGNOSTICS(4) | 响应48字节：uptime_us:u64，scan_overruns/scan_errors/report_overflows/usb_errors/save_errors/rgb_errors/bad_nlp_frames/uart_dropped各u32，estimated_LED_mA:u16，configured/suspended/schema_locked/unlocked/channel_limit/flash_available各u8 |

active_features bit0布局、1诊断、2 NKRO、3改键、4逐键RGB、5配置持久化、6 Caps Word、7 Grave Escape、8声明式灯效规则。按外设初始化结果屏蔽不可用RGB/Flash。reserved_features是未来类型组，不是已启用功能，见下一节。分区flags bit0 NOS拥有、1原子journal、2保留未分配；kind0 preserved、1 boot、2 config。没有Flash时容量/分区数为0。

lighting major1/minor2保留GET_INFO(1)/SET(2)/GET_STATE(3)，增加：

| 操作 | 格式 |
| --- | --- |
| GET_INFO(1) | 原8字节：LED数:u16,max_fps:u16,max_requested_brightness:u8,mode_mask:u8,flags:u16；模式bit0..7对应上述列表，flags bit0限流、1挂起断电、2持久策略、3整帧事务、4空闲策略 |
| SET(2) | 原11字节generation:u32,mode:u8,brightness:u8,reserved:u16=0,rgb[3]；minor1允许追加persist:u8（0/1） |
| GET_STATE(3) | 原11字节格式，RGB现在持久保存 |
| GET_SETTINGS(4) | 空请求，响应16字节generation:u32,speed:u8,flags:u8,idle_seconds:u16,LED_budget_mA:u16,rgb[3],persistence:u8,reserved:u16=0 |
| SET_SETTINGS(5) | 同16字节布局，persistence位置作为persist请求0/1；flags只允许bit0锁灯/Caps Word指示，speed1..255，idle0..3600 |
| READ_LEDS(6) | 请求first:u16,count:u16,expected_generation:u32（0任意）；响应generation:u32,first:u16,count:u16,RGB[count]，读的是持久配色，不是当前动画帧 |
| BEGIN_FRAME(7) | expected_generation:u32，响应transaction_id:u64 |
| WRITE_FRAME(8) | transaction_id:u64,first:u16,count:u16,RGB[count]；允许同内容重复，重叠不同内容CONFLICT |
| COMMIT_FRAME(9) | transaction_id:u64,generation:u32,persist:u8,reserved[3]；响应generation:u32,persistence:u8,reserved[3] |
| ABORT_FRAME(10) | transaction_id:u64 |

所有写操作都要CONFIG_WRITE和当前peer的物理解锁。逐键事务必须覆盖全部61键，固定5秒期限，全部校验后才复制到活动配色并选择custom。仅接受创建会话，断线/撤权/超时丢弃暂存，generation变化返回CONFLICT。RPC请求缓存处理同请求ID重传；新的请求ID在事务已关闭后返回STALE。persistence0已应用但未承诺落盘、1待保存、2已持久。没有通用任意动画脚本执行或远端DMA地址。

## 4. 为未来保留的能力

reserved_features当前bit0..6分别表示下面七组设计插槽，读取它不表示硬件存在。未注册类型/未实现opcode返回UNSUPPORTED；未来能力有独立UUID、版本、权限与配额，不能复用现有动作含义或旧字段padding。

| 插槽 | 接入边界与完成条件 |
| --- | --- |
| combo/leader | 放输入动作层，先规定冲突优先级、延迟上限、press-time binding、撤销和容量，再发布新配置schema；不挤占已有宏参数 |
| tap dance/多tap-hold策略 | 为行为独立定义版本；保留当前hold-on-other-press语义；覆盖快速滚键、重叠按键、阈值、回绕和队列满 |
| profile/宏与资源库 | 未来用显式格式化/迁移建立目录、资产ID、分区版本、容量配额与磨损策略；opaque扩展只存引用，不把大脚本/字体/帧缓冲塞进F401 RAM；默认不擦除保留区 |
| display | 独立NLP display实例、tile/profile与原点；字体/资源按能力协商；当前板的RGB画布不是屏幕 |
| encoder/扩展按键 | NLP input来源用UID/boot/cap/control ID，转为配置动作；引用计数合并并在断线/租约到期释放。61位仅代表本地61键，不把新模块塞进空余3位 |
| wireless/split | 独立BT/Wi-Fi能力及输入sink owner；先选模块/接口/供电，再做配对、断链全释放和所有权切换；发现未知模块不自动注入键盘 |
| update/recovery | 独立firmware能力、分区及签名根；只有安全staging、引导再验证、确认/回退可用时才广告APPLY；当前不冒称安全OTA |

扩展端口由board resource manifest分配：SPI1已给NOR，SPI2/DMA1 Stream4用于RGB，UART1是维护文本，PA9不能同时作USB VBUS sensing。未确认的I²C引脚、片选和PA0用途不预设为可用。每新增模块先校验冲突、DMA权限、RAM/栈/Flash预算，再注册能力；禁止为了占位提前开启GPIO或扩大内核MMIO白名单。

NLP SDK当前固定8能力/endpoint、4会话、2通道、6共享PDU槽。NK60现在有5个普通能力，后续较大功能优先放独立节点；不能仅增加目录条目却忽略会话、控制队列和用户16KiB静态区。参见 [NLP实现](protocols/nlp/implementation.md)。

## 5. 配置示例与验证

UART物理解锁后：

```text
rgb 7 180
color 40 100 255
led 29 255 0 0
effect 100 300 150
save
```

NLP经USB vendor接口；写操作保持连接等待物理解锁：

```sh
python3 scripts/nlp_host.py --usb 0eff:0001 --physical info
python3 scripts/nlp_host.py --usb 0eff:0001 --physical layout
python3 scripts/nlp_host.py --usb 0eff:0001 --physical storage
python3 scripts/nlp_host.py --usb 0eff:0001 --physical health
python3 scripts/nlp_host.py --usb 0eff:0001 --physical lighting-set --mode wave --brightness 180 --rgb 40,100,255 --save --wait-unlock 60
python3 scripts/nlp_host.py --usb 0eff:0001 --physical lighting-policy --speed 100 --idle-seconds 300 --led-budget 150 --save --wait-unlock 60
python3 scripts/nlp_host.py --usb 0eff:0001 --physical leds-get --file /tmp/nk60-colors.json
# 编辑该文件的61组RGB（每分量0..255）后：
python3 scripts/nlp_host.py --usb 0eff:0001 --physical leds-put --file /tmp/nk60-colors.json --save --wait-unlock 60
```

`leds-get --file`拒绝覆盖已有文件；上面的USB命令用于首次实板验收，尚未在实板运行。自动化已经用独立Python CLI连接真实C NK60服务执行同样的协议；夹具模拟USB/Flash及物理解锁按键，不证明电气或真实人在场。

冻结验收运行 `tests/acceptance.py --section boards --require-frozen-source`，包含实际引擎10万状态扫描、逐字节掉电、service模型/独立CLI、USB/硬件/权限模型、MPU0/1固件与stock QEMU用户启动专项。固件记录用户静态区、bin、已知配置调用链的编译器栈帧预算；不把静态统计称为真实高水位。

可组合动画、按键行为绑定、JSON示例及lighting minor2程序操作见 [自定义灯效](NK60_LIGHTING.md)。
