# NLP v1 标准能力

<!-- Copyright (C) 2026 NOS contributors -->

C SDK 已实现 keyboard/log/input/keymap/display/shell/firmware/bluetooth/wifi 的有界处理器；lighting 接入 NK60。power/time 仍保留。能力处理器通过显式后端回调连接硬件和操作系统，仅在后端具备功能时注册。NK60 当前只公布 device/keyboard/log/keymap/lighting/keyboard_control；没有宣传终端、OTA、屏幕或无线实板支持。

所有条目均为**待实现的标准接口设计**。稳定类型由 `UUIDv5(NAMESPACE_URL, "https://github.com/wkcs/nos/nlp/capability/" + name)` 生成，完整 UUID/操作码见注册表。厂商类型使用自己控制的名称空间；不占用下表类型后改变格式。同一类型可有多个实例。

除管理预会话外，REQUEST/RESPONSE 都带 [wire](wire.md) 的 8 字节 RPC 前缀；下文长度不含该前缀。EVENT/DATA 不带 RPC 前缀，使用其明确格式。未定义字段/功能必须拒绝，不能返回空成功。大对象显式分块，每块都必须适配协商 payload。

## 1. 设备管理 `device`（内置实例0）

管理服务提供设备描述、分页目录、端口、拓扑事件、授权会话的打开/关闭和租约。普通业务不能向它传任意内存地址、Flash 物理地址或原始 MMIO 指令。核心操作码和标准能力操作码是不同名字空间。

能力描述除目录的 28 字节条目，还必须说明：权限、方向（PROVIDE/CONSUME）、origin（NATIVE/PROXY）、owner UID、可选硬件身份、格式 major/minor、功能位、最大消息/通道/在途请求、事件/输入租约范围和资源限制。代理不能伪造被代理芯片的固件升级、认证或持久性。

## 2. 日志 `log`

操作：SUBSCRIBE(1)、SET_FILTER(2)、GET_STATS(3)、UNSUBSCRIBE(4)；事件 RECORD(0x1001)。过滤包括级别 mask、最多4个 component_id、每秒记录上限和丢弃策略。默认非可靠、priority3、最多16条/秒，当前 burst 参数只接受1；设备按自身 ring/链路预算进一步限制，不能阻塞中断、键盘扫描或内核临界区。

RECORD 固定头24字节：`monotonic_us:u64, record_seq:u32, dropped_total:u32, component_id:u16, level:u8, flags:u8, text_len:u16, reserved:u16`，随后 text_len 字节 UTF-8。reserved=0；flags bit0 TRUNCATED，其余0；level0..5。消息总长必须精确匹配。时间来自该设备本次 boot 的单调时钟，不冒充全网 UTC；dropped_total/record_seq 按 u32 计数，工具结合 boot_id 和序号显示丢失。

读取日志和修改日志级别可有不同权限。敏感日志不因使用有线载体而默认公开。GET_STATS 返回四个u32：ring字节容量、当前记录数(0..4)、累计覆盖/丢弃、累计记录序号；不能用“可靠日志”占满控制缓冲。

## 3. 终端 `shell`

操作 OPEN_TERMINAL(1)、RESIZE(2)、INTERRUPT(3)、CLOSE_TERMINAL(4)、GET_EXIT(5)；DATA BYTES(0x2001)、事件 EXIT(0x1001)。shell 是独立终端会话，不是把 RPC 字符串交给内核 eval。

OPEN 参数：`columns:u16, rows:u16, flags:u16, term_len:u16, term[term_len]`，列1..512、行1..256、term_len≤32，当前后端 term 限可打印 ASCII（UTF-8 子集）；flags bit0 请求独立 stderr，其余0。成功响应固定12字节：`terminal_id:u32, features:u32, lease_ms:u32`，features bit0独立stderr，其余0，当前 lease_ms 返回所属 NLP 会话租约（默认15000ms），由有效会话流量续期；尚未另设终端空闲超时。基线在可靠 channel1 传双向有序终端字节，服务端提供 stdin 与合并的 stdout/stderr；独立 stderr 仅在描述符和 OPEN 确认后用 channel2。不做 NUL 结尾处理，允许终端控制字节；INTERRUPT 在保留控制队列处理，不能被大量输出饿死。

RESIZE 为 `terminal_id:u32, columns:u16, rows:u16`，INTERRUPT 为 `terminal_id:u32, signal:u16, reserved:u16`（协议值2 INT / 15 TERM，不透传其他OS信号号）；CLOSE/GET_EXIT 为 terminal_id:u32。BYTES 为 `terminal_id:u32 | data[]`；EXIT 为 `terminal_id:u32, exit_status:i32`。输出和关闭均有背压、取消与会话撤销语义；字节流不跨重连重放，断开后结果可能 UNKNOWN。

能力描述必须指明后端：完整 nshell/PTY，或另一种明确命名的维护终端。当前 NK60 固件只有 UART 维护命令，不链接通用 nshell/VFS，不能直接宣称 `shell` 已可用；初期可保持不暴露该能力。

## 4. 固件 `firmware`

操作：GET_TARGETS(1)、BEGIN(2)、MANIFEST_CHUNK(3)、WRITE(4)、QUERY(5)、VERIFY(6)、APPLY(7)、ABORT(8)、CONFIRM(9)。这是组件/分区 API，不提供任意 Flash 地址写入。

GET_TARGETS 分页返回逻辑 component UUID、board/hw 兼容标识、活动版本、允许镜像大小、擦写粒度、更新模式（STAGING_ONLY/A_B/RECOVERY）、签名要求、rollback_index、最大块及是否真正支持 APPLY。不能把未分配的 Flash 或旧常量地址当作可写分区。

更新流程：

1. BEGIN 为 `component_uuid[16], manifest_length:u16, encoding:u16, manifest_sha256[32]`。v1 encoding=1，manifest_length≤512；响应返回持久 update_id:u64、分块上限及状态。相同 component+manifest_digest 查询/恢复既有更新，而不是重复擦除。
2. MANIFEST_CHUNK 为 `update_id:u64, offset:u32, length:u16, reserved:u16, bytes[length]`。manifest 为注册表字段的升序 TLV：board兼容UUID、hw范围、component UUID、image_size:u32、image_sha256[32]、firmware_version:u64、rollback_index:u64、签名 key_id:u32、Ed25519 signature[64]。签名输入为 ASCII `NLP-FW1` 加除 signature TLV 外的完整规范 manifest 字节；未知必须字段、重复项、无效签名、rollback 值或超分区长度在擦写镜像前拒绝。manifest 整体 SHA 与 BEGIN 匹配后才接受 WRITE。
3. WRITE 格式同 MANIFEST_CHUNK。有效数据上限 `payload_limit-8(RPC)-16`，按 Flash page 划分/验证。响应区分 `accepted_offset:u32` 与 `durable_offset:u32`，不能把 RAM 接收或链路 ACK 说成已持久化。顺序写入，相同 offset/内容可幂等确认；不同内容冲突，不隐式覆盖。
4. 持久 checkpoint 按擦除块合并，不能每几十字节磨损一个日志扇区。QUERY 返回持久偏移和待恢复状态；复位后可擦除最后一个未提交的 staging 扇区并从 durable_offset 重传，绝不擦活动镜像或其他能力的配置扇区。
5. VERIFY 只在全量数据、签名、兼容性和 SHA256 都验证后进入 VERIFIED。VERIFY/APPLY/ABORT/CONFIRM body 为 update_id:u64。APPLY 以持久 update_id 和 boot metadata 提交，丢响应后先 QUERY；重复 APPLY 不得翻转两次镜像槽。
6. 支持 A/B 或可恢复引导时，新镜像进入 PENDING，启动尝试数有上限（默认3），健康确认才 COMMITTED；失败回退旧可用镜像。保护 rollback_index 的推进不能先让唯一可用旧镜像失效；具体可信存储/bootloader 必须经过断电验收。

ABORT 只删除本事务拥有的 staging/未提交元数据。已切换活动镜像后不可伪装 ABORT 成功撤销升级。状态机为 EMPTY→MANIFEST→RECEIVING→VERIFIED→PENDING→COMMITTED，错误保留可恢复状态。查询有界、块和分区算术无溢出。

**当前 NK60 没有经过验收的安全 OTA 引导链。** 128 KiB 应用区之外保留的内部 Flash 不能自动视作空闲 A/B 槽，外置 NOR 最后两扇区已用于键盘配置。完成明确 staging 分区、签名验证、恢复引导和断电测试前，不公布可用 APPLY；只实现上传或校验时必须标记 STAGING_ONLY。

## 5. 键盘 `keyboard`

操作 GET_STATE(1)、SUBSCRIBE(2)、SET_LEDS(3)、SET_OUTPUT_OWNER(4)、GET_STATS(5)；事件 STATE(0x1001)。provider 是聚合后的逻辑 HID 键盘，和下节原始按键/旋钮输入分开。

STATE 固定52字节：`state_seq:u32, monotonic_us:u64, modifiers:u8, flags:u8, bitmap_len:u16=32, usage_bitmap[32], consumer_usage:u16, reserved:u16=0`。modifier 为 E0..E7 的8位，bitmap 中对应位必须0；flags bit0 ALL_UP_BARRIER，其余0；usage0..3不置位，consumer≤0x3ff。GET_STATE 响应返回同格式；变化事件必须可靠、有序。

SUBSCRIBE 为 `lease_ms:u16, refresh_ms:u16`；响应给出实际值。默认租约300 ms、全量刷新100 ms，合法租约100..5000 ms且刷新≤lease/3。路径轮询/拥塞无法达到该预算时拒绝绑定或显式选择更长租约，不能悄悄制造卡键。每个事件是完整状态；断开/超租约/boot变化/撤销后消费者立即清除该来源，重新加入先全松开再接受新按键。队列溢出时发送全释放并设置 ALL_UP_BARRIER，后续物理全松开才恢复。

SET_LEDS 为 `mask:u8, reserved[3]=0`，低5位为 Num/Caps/Scroll/Compose/Kana。SET_OUTPUT_OWNER 为 `sink_device_uid[16], sink_cap:u16, mode:u8, reserved:u8`，mode=0 LOCAL_USB、1 NLP_SINK；属于独立授权操作。观察订阅不获得输出所有权。

NLP host SDK 默认只观察，不自动注入系统输入。NK60 本地 USB HID 与 NLP→host/Bluetooth/Wi-Fi 输出只能按明确的 owner/binding 策略生成输入，避免同一次按键重复两次。切换 owner 先对旧 sink 释放全部键，再建立新 sink 和全松开屏障；sink 失联时不重放旧按键。

## 6. 改键 `keymap`

操作 GET_LAYOUT(1)、BEGIN(2)、READ_PAGE(3)、WRITE_PAGE(4)、VALIDATE(5)、COMMIT(6)、ABORT(7)、GET_STATUS(8)。描述符明确 action schema major、物理 control ID、层数、宏数/步数、wire配置长度和保存策略；不能把当前 C struct 直接作为公开格式。

BEGIN 带 `expected_generation:u32, total_length:u32, sha256[32]`，返回 transaction_id:u64；READ/WRITE 为 `transaction_id:u64, offset:u32, length:u16, reserved:u16`，WRITE 后接 bytes。VALIDATE/COMMIT/ABORT 为 transaction_id:u64，COMMIT 另带 `expected_generation:u32, persist:u8, reserved[3]=0`。generation 不匹配返回 CONFLICT，先整体语义校验再原子替换；修改时释放按键并建立全松开屏障。

当前 NK60 1056 字节键盘配置可作为 `nos-keymap-1` schema 的输入，但公开编码、动作数值、保留字段及 CRC/日志必须和现有实现逐项对齐后再启用。COMMIT 响应区分 APPLIED_VOLATILE / SAVE_PENDING / PERSISTED，不能把延迟60秒保存冒称落盘。物理解锁窗口沿用 Esc+PN+Fn 2秒/30秒，并与已认证客户端和目标能力绑定；不能变成全网通用管理员令牌。

## 7. 原始控制器 `input`

操作 GET_CONTROLS(1)、GET_STATE(2)、SUBSCRIBE(3)、BIND(4)、UNBIND(5)；事件 BUTTON_STATE(0x1001)、RELATIVE(0x1002)。BIND 需要主设备明确授权，不按远端自报“keyboard”就自动信任。

v1每实例最多16个 control，control_id为0..15，类型 BUTTON/ENCODER。BUTTON_STATE 固定16字节：`state_seq:u32, monotonic_us:u64, button_bits:u16, reserved:u16=0`；非BUTTON位置必须0。RELATIVE 固定16字节：`event_seq:u32, monotonic_us:u64, control_id:u16, delta:i16`；delta非0，超出单事件范围拆成多个有序事件。事件可靠发送，重传去重；重连先 GET_STATE 设置新基线，不补发旧旋钮转动。GET_CONTROLS 描述类型和有效范围，格式见下文；名称/累计位置留作显式扩展，刷新/租约通过SUBSCRIBE协商；更多按键用多个实例，而不是偷偷扩大当前 NK60 的61位矩阵。

GET_STATE 响应为 BUTTON_STATE；SUBSCRIBE 为 keyboard 同格式 lease/refresh；BIND 为 `binding_id:u32, expected_boot_id:u64, control_mask:u16, reserved:u16`，UNBIND 为 binding_id:u32。映射到本地动作由主设备配置决定。多个来源持有同一 HID usage 时用引用计数合并；某来源掉线只释放自己的引用，不释放仍由其他来源按住的键。接收端使用本地到达顺序仲裁，远端时间戳不能任意改变优先级。

## 8. 显示 `display`

操作 GET_INFO(1)、BEGIN_FRAME(2)、END_FRAME(3)、ABORT_FRAME(4)、SET_POWER(5)；DATA TILE(0x2001)。GET_INFO 返回宽高、RGB565_LE/MONO1像素格式、最大tile与更新模式，格式见下文；v1每实例同时最多一个帧。

BEGIN_FRAME 为 `frame_id:u32, tile_count:u16, mode:u8, reserved:u8`，mode=0渐进更新、1原子呈现。TILE 为 `frame_id:u32, tile_index:u16, x:u16, y:u16, width:u16, height:u16, format:u8, reserved:u8, data_len:u16, data[]`，固定头18字节；format=0 RGB565_LE / 1 MONO1。RGB565_LE 每像素2字节；MONO1 每行从 bit7 开始，行字节数 ceil(width/8)，末字节未用位为0。END 为 `frame_id:u32, tiles_crc32c:u32`，CRC按 tile_index 顺序覆盖全部TILE payload；ABORT为frame_id:u32；SET_POWER为 enabled:u8和3字节0。

坐标、乘积、行跨度、tile数量/顺序和总长须严格检查；tile必须适配payload_limit，不能一次提交整张超过 MCU RAM 的帧。原子模式仅在模块确有双缓冲/staging且广告该标志时可用，否则返回 UNSUPPORTED。渐进模式中已显示的 tile 无法通过 ABORT 回滚，响应必须如实说明；不支持整帧回读不能伪造验证成功。

普通 SSD1306/ST77xx 等芯片可由 NK60 本地驱动代理；带自身 MCU/NLP 的屏幕则是独立设备。二者 origin、固件能力和认证边界不同。

## 9. 灯光、蓝牙/Wi-Fi、供电和时间

- `lighting`：GET_INFO(1)、SET(2)、GET_STATE(3)。SET body `generation:u32, mode:u8, brightness:u8, reserved:u16, rgb[3]`；mode0关闭/1静态/2层色，minor1新增3呼吸/4彩虹/5波浪/6按键响应/7逐键自定义，未公布模式拒绝。描述符列灯数、最大刷新/亮度、功耗策略；NLP 不能绕过本地电流限制或 USB suspend 关灯。
- `bluetooth`：GET_INFO(1)、SCAN(2)、PAIR(3)、UNPAIR(4)、BIND(5)、GET_STATE(6)，事件 SCAN_RESULT(0x1001)、LINK_STATE(0x1002)、JOB_DONE(0x1003)。通过有界参数 TLV 选择角色/目标和已授权输入绑定；扫描时长、结果数和配对窗口有上限。蓝牙 HID sink 是模块实现的角色，不表示 NK60 自身具备蓝牙射频。
- `wifi`：GET_INFO(1)、SCAN(2)、CONFIGURE(3)、CONNECT(4)、DISCONNECT(5)、GET_STATE(6)，事件 SCAN_RESULT(0x1001)、LINK_STATE(0x1002)、JOB_DONE(0x1003)。凭据只在已认证加密会话中提交，不回显到日志。可以把无线数据口作为新的 NLP 邻接载体，也可提供显式 host tunnel，不能自动把私有链路整个桥接到公网。
- 无线控制使用下述有界TLV profile；芯片命令ABI留在模块内部。实际不支持的角色必须返回UNSUPPORTED。无线模块自己暴露的子设备遵循同一加入/授权/级联模型。
- `power`：GET_INFO/GET_STATE/SET_POLICY；`time`：GET_INFO/GET_TIME/SYNC_SAMPLE。当前仅预留类型和操作，详细格式未定稿，描述符不得广告为 v1 稳定能力。TIME 可用于显示/日志对齐，不能让未认证远端直接修改安全计时或更新回滚索引。

能力格式和 feature 位在 major 内保持兼容；每一新能力启用前都必须有双方实现、错误路径和互通向量，不能只注册名字就暴露空接口。

### 无线控制 TLV 格式

BT/Wi-Fi REQUEST/RESPONSE 在RPC前缀后携带升序TLV；EVENT以 `event_seq:u32 | monotonic_us:u64` 开始，再跟TLV。表内为低15位tag，必须参数由操作定义，未知必须参数拒绝。值是原始字节或小端定宽整数；不返回密码。

| tag | 类型 | 字段/限制 |
| --- | --- | --- |
| 1 | u32 | job_id，非0，本次boot内不复用 |
| 2 | u8 | role：BT 0 BLE_HID_DEVICE / 1 BLE_CENTRAL / 2 CLASSIC_HID_DEVICE；Wi-Fi 0 STATION / 1 AP；只接受描述符已公布角色 |
| 3 | u32 | duration_ms，扫描100..60000 |
| 4 | u16 | max_results，1..32 |
| 5 | bytes[6] | BT peer_address，按显示顺序，不作为设备可信身份 |
| 6 | u8 | BT address_type：0 PUBLIC / 1 RANDOM；不能假装BLE隐私地址永久稳定 |
| 7 | UTF-8≤32字节 | peer_name，展示用 |
| 8 | bytes[16] | 已授权的keyboard源device_uid |
| 9 | u16 | keyboard源capability_id |
| 10 | u8 | BT pairing_method：0 PHYSICAL_CONFIRM / 1 PASSKEY；不支持时拒绝 |
| 11 | u32 | passkey，0..999999，仅加密提交，不在响应/事件中回显 |
| 12 | bytes[1..32] | SSID原始字节，非UTF-8也合法，UI转义显示 |
| 13 | bytes[8..63] | v1 Wi-Fi密码；仅WPA2-PSK/WPA3-SAE公布并支持的UTF-8口令模式，不接受歧义hex/裸key混用 |
| 14 | u8 | Wi-Fi security：0 OPEN / 1 WPA2_PSK / 2 WPA3_SAE；OPEN不得带密码 |
| 15 | u8 | state：0 IDLE / 1 SCANNING / 2 PAIRING / 3 CONNECTING / 4 CONNECTED / 5 ERROR |
| 16 | u16 | result_status，协议状态值 |
| 17 | i16 | RSSI dBm（仅结果/状态） |
| 18 | bytes[4]或[16] | IP地址（仅状态），长度区分IPv4/IPv6 |
| 19 | u32 | supported_roles位图（GET_INFO），bit n对应role n |
| 20 | u32 | dropped_results，累计丢弃的scan事件数 |

SCAN必须有tag1/3/4，可选role；PAIR必须有1/5/6/10，PASSKEY另需11；UNPAIR有5/6。BIND必须有8/9，并检查该源的授权与输出owner。Wi-Fi CONFIGURE必须有12/14，非OPEN另需13；CONNECT/DISCONNECT无密码参数，可携带1用于异步状态查询。GET_INFO/GET_STATE/UNPAIR/BIND等其余操作不得接收与操作无关的已知参数，避免“忽略后看似成功”。

每实例最多一个异步job；RPC成功表示已接受job，不表示连接完成。事件SCAN_RESULT带job_id、地址或SSID、可选RSSI；LINK_STATE带state、可选当前peer/SSID/IP但绝不含tag11/13；JOB_DONE带job_id/result_status/dropped_results。收不到JOB_DONE时用GET_STATE查询，不无限重复PAIR/CONNECT。扫描结果超上限丢弃并计数，不能占满输入队列。蓝牙sink通过已授权的keyboard能力会话消费STATE，不使用未定义的原始HCI/AT字节隧道替代标准profile。


## 10. 小模块的描述与范围

input GET_CONTROLS采用wire章节的12字节分页请求/响应头，每条目固定16字节：`control_id:u16 | type:u8 | flags:u8 | minimum:i32 | maximum:i32 | resolution:u16 | reserved:u16=0`。type=1 BUTTON（min0/max1/resolution1）、2 ENCODER（相对delta范围-32768..32767，resolution为每机械档的增量数且非0）；v1 flags=0，不公布尚未定义的绝对累计位置。UTF-8名称作为后续可选描述扩展，不放进固定条目。单实例总数≤16，分页revision失效返回STALE。

display GET_INFO响应固定16字节：`width:u16 | height:u16 | formats:u16 | modes:u16 | max_tile_bytes:u16 | max_tiles:u16 | min_frame_interval_ms:u16 | reserved:u16=0`。formats bit0 RGB565LE / bit1 MONO1；modes bit0 PROGRESSIVE / bit1 ATOMIC；坐标和尺寸均为逻辑旋转后的面板坐标，v1不支持隐式旋转。max_tile_bytes含18字节TILE头，不得超过会话payload；max_tiles限制帧内tile数。SET_POWER为`state:u8,reserved[3]=0`，state0关1开；BEGIN的mode=0/1必须对应modes中已公布的bit0/bit1模式，END在最后一个tile被校验后才成功，实际扫描出屏延迟由面板决定。

尚未逐字段冻结的扩展（完整keymap action schema、固件目标目录与bootloader metadata、日志过滤/统计细项、power/time、产品专用功能位）必须在相应P阶段补充独立格式表和向量后才能声明互通。它们不影响本次固定wire/发现与核心输入格式检查，也不能因有type UUID就宣传为已实现能力。

NK60的lighting minor1扩展与keyboard_control能力的完整布局、逐键事务和存储格式见 [NK60能力](../../NK60_CAPABILITIES.md)。未注册扩展能力不表示可用；实例ID不得当类型硬编码。

lighting minor2增加400字节声明式程序的查询/分页读取/原子上传，支持输入动作和状态绑定、条件、区域和混合；协议格式见 [自定义灯效](../../NK60_LIGHTING.md)。
