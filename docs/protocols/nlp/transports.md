# NLP v1 载体绑定

<!-- Copyright (C) 2026 NOS contributors -->

载体提供 `probe / peer_up / send_pdu / receive_pdu / poll / cancel / peer_down`。接口返回实际接收、忙、超时、链路关闭等结果，不把“排进 DMA 队列”当成端到端完成。每个适配器声明 PDU 上限、最坏不可抢占时间、轮询周期、在途/重组槽数及安全模式。线路参数与角色不放进业务能力格式。

## 1. 共用规则

- PDU 格式见 [wire](wire.md)，明文最大 1080 字节，加密最大 1092 字节（52+1024+trailer）；MCU 配置按 128 字节 payload 保留最大 196 字节 PDU。
- 无边界字节流采用 `length:u16 LE | PDU[length]`。length 是完整 PDU 长度，必须在 `[56, negotiated_max_pdu]`，必须等精确 length 个字节再交付，不允许一次 read 对应一次消息的假设。
- 单跳分段先在静态槽内重组成一个完整 PDU，再验证/转发。段不能直接调用能力。每 peer 最多 1 个进行中的重组；START 替换旧 transfer 必须取消旧状态；超时、乱序、重复段内容冲突、超长都丢弃整个 transfer。
- 分段完成仅代表送给 NLP 检查。端到端重传可以使用新的单跳 transfer_id，但 PDU 的 packet_counter/密文必须不变。
- 从已关闭链路到来的迟到 completion 必须带端口 generation 丢弃，不能误释放新会话的缓冲区。缓冲区在 DMA 完全停止前不能重用。

## 2. UART / RS-485

UART 为全双工点到点，默认 115200 8N1，可协商更高波特率；波特率变化必须两端确认并允许回退。推荐 RTS/CTS，但不能以硬件流控替代 NLP 的能力信用。

分帧为 `COBS(length:u16 | PDU) | 00`；length 在 COBS 编码内部。delimiter 前不允许包含 00。解码后长度必须精确匹配、头/帧 CRC 有效；溢出后丢弃直到下一个 delimiter，半帧默认 250 ms 取消。使用有界 ring，不因一个伪造长度无限等待或分配。最大编码缓冲按原始长度 `n=2+max_pdu` 的 `n+floor(n/254)+2` 保留。

上电双方可发送单跳 HELLO，使用退避和已知 magic 区分控制台噪声；不得在同一无复用规则的 UART 上同时混入原始 nshell/日志文本。迁移时配置 NLP 模式或单独串口，日志/shell 必须经各自能力。

RS-485 多点半双工是独立可选绑定：必须配置主节点、唯一链路地址、方向控制、时隙/轮询和静默时间。裸 UART HELLO 不能安全解决 RS-485 总线竞争；未实现该绑定时只声明 UART 点到点。

## 3. Wi-Fi / IP

Wi-Fi 先完成 802.11 入网，NLP v1 使用其上的 IP，不定义裸 802.11 MAC 协议。基线为 TCP，UDP 可选；地址与端口可配置，实验默认端口 38447，**不是 IANA 已注册服务端口**。

- TCP 使用共用 u16 长度前缀。TCP 连接是一条邻接边；必须保留 NLP 端到端流控/确认，因为后续路径可能变为 UART/CAN，不能把中间 TCP 成功当成最终操作完成。
- UDP 一个 datagram 恰好一个 PDU，不再加长度前缀。最大 datagram 受路径 MTU 限制，禁止依靠 IP 分片；NLP 的可靠模式仍负责确认/重传，不能直接用于未授权广播 shell。
- 可用 DNS-SD/mDNS `_nos-nlp._tcp.local` / `_nos-nlp._udp.local` 发现服务；TXT 只公布 wire major、最小角色和可选匿名标识，可信 UID 以已认证 HELLO/AUTH 为准。mDNS 仅是发现提示，不是配对证明，也不跨路由器泛洪。
- 无 mDNS 的网络允许静态地址、上位机扫描用户指定的端点列表，或显式开启有配额的本地发现；禁止默认扫描整个网段/Internet。
- 不把 SSID 密码当作 NLP 身份。经过多层代理仍使用 NLP 端到端保护；TLS 可作为本地附加保护，不能隐式授权中间桥访问 shell/固件能力。

## 4. USB

上位机的首个实际候选载体是 USB vendor interface，bulk OUT/IN 各一个端点；使用共用 u16 长度前缀，允许跨 64 字节 packet、粘包、短包、取消与断开。HELLO 在该接口发现相邻设备，不能把 USB VID/PID 或接口号当永久 UID。

NK60 保留原 HID boot/report 接口用于系统直接打字；NLP 独立使用建议 EP02 OUT / EP82 IN。具体端点、FIFO、WinUSB 描述符/host SDK 须在实现阶段核对。当前 DWC2 驱动只实现 EP0/EP1，**尚未具备该 vendor interface**。F401 FS 共 320 words FIFO；候选分配 RX128 + EP0TX64 + EP1TX64 + EP2TX64 = 320，需实际枚举/压力测试，不能只加描述符就宣称可用。

USB reset/取消配置/拔线必须撤销 NLP 邻居、会话和输入租约。主机应用的 NLP keyboard 观察通道不能同时自动注入 OS 键盘，否则会与原生 HID 重复。

## 5. I²C

I²C controller 发起所有交易；target 的“主动事件”表现为 READY/IRQ 或等待 controller 轮询，不能假装 target 能独立往总线发包。NLP 自发现从**板级声明的 NLP 地址/复用器端口**开始，不对未知芯片做破坏性探测，不使用 general-call 广播作为通用发现。

本绑定使用 target mailbox，所有寄存器地址 u8：

| 寄存器 | 访问 | 内容 |
| --- | --- | --- |
| 0x00 ID | 只读 | 固定 8 字节：`4e 4c 50 49 01 00 00 00`（NLPI、binding major1、保留） |
| 0x10 STATUS | 只读 | `flags:u16, max_pdu:u16, rx_free:u16, tx_len:u16, tx_token[12]`，共 20 字节 |
| 0x20 RX_COMMIT | 写 | `length:u16 | PDU`，完整交易成功且校验通过后才入箱；length 不得超 max_pdu/rx_free |
| 0x30 TX_PEEK | 读 | `token[12] | length:u16 | PDU`；一次事务内快照固定，读取不弹出 |
| 0x40 TX_ACK | 写 | `token[12]`，仅匹配当前 tx_token 才弹出；重试旧 token 不得弹出下一个消息 |
| 0x50 CANCEL | 写 | `token[12]`，取消该物理 mailbox transfer，不代表撤销已执行业务 |

rx_free的单位为可接受的完整PDU字节数，不含2字节COMMIT前缀；没有完整接收槽时必须为0，不能把零散容量相加授予一包。STATUS flags：RX_READY=1、TX_READY=2、ERROR=4，其余为 0。空 TX 返回12字节全0 token、length=0。token为`boot_id:u64 | sequence:u32`，每次启动sequence从1递增且同boot内永不复用；重连也不得重置，耗尽必须进入新的boot。新boot_id必须满足设备启动唯一性要求。迟到的旧boot/sequence ACK不匹配新消息，不能只用从1重新开始的u32 token；controller 通过 tx_len 确定 PEEK 长度，处理期间变化必须重新读 STATUS，不返回拼接快照。错误/短写不得部分提交；同一 PDU 被链路重送仍由 NLP 序号去重。

v1 mailbox 要求 controller/target 能完成 `max_pdu+14` 字节的连续交易；硬件 DMA/target 缓冲不足时降低协商 MTU，不能默默拆成不具备事务语义的读写。可选 IRQ 低有效，仅表示“请查 STATUS”，不携带消息计数。无 IRQ 默认轮询 4 ms，实际周期由端口预算声明，输入租约据此计算。

地址由硬件绑带、配置或 I²C mux 保证唯一。两个相同 target 地址不能靠 NLP 自动消歧，必须先隔离/改址。同一个 target 上存在 NLP 并不意味着其下的所有普通 I²C 芯片天然会说 NLP。

## 6. SPI

SPI controller 决定时钟与 CS；每个 CS 是独立邻接端口。上电按板级配置 mode/频率（不盲目扫 mode/频率），用只读 ID 探测候选模块。READY GPIO 可选，缺少时轮询 STATUS。

使用与 I²C 相同的 mailbox 语义、寄存器和 token。一次 CS 事务为 `command:u8 | register:u8 | count:u16 | data[count]`，command=1 READ、2 WRITE，其余拒绝；字段小端。READ 的前 4 字节 MOSI 是命令头，之后 MOSI 为 0 dummy、MISO 返回数据；WRITE 的 MISO 忽略。CS 提前释放丢弃未完成写入，不提交半帧。count 精确对应寄存器读写大小；STATUS/ID 固定长度，PEEK/COMMIT 根据 length 验证，禁止越过 mailbox 边界。

全双工物理不意味着 NLP 两方向同时提交；target 有待发数据仍要等 controller READ。连续交易的最大字节数和 CS 占用时间必须受限；显示/升级块不能长时间饿死键盘和 USB。非 NLP SPI 显示芯片由本地驱动代理成 display 能力，不对裸芯片运行 HELLO。

## 7. CAN / CAN-FD

基线为 29 位扩展 data frame，不使用 CAN remote frame；标准 CAN payload 8，CAN-FD 可至 64。CAN 的硬件 ACK 只说明总线上有人接收，不说明指定 NLP 对端完成了重组、认证或业务。

v1 要求先有唯一单跳 `link_addr`（controller=0，设备1..253，254保留，255广播），与 NLP node_id 无关。可由配置/生产地址或单设备 commissioning 分配；相邻节点在地址已唯一后广播/回应 HELLO 来自发现。无预配置节点可采用下述可选 CAN-COMMISSION-1；只有实现并验收它，才能声明支持 CAN 无预配置即插即用。不得简单让所有匿名节点使用相同 ID 发送不同 UID 数据，造成错误帧和 bus-off。

29 位 ID：`priority[28:26] | src_link[25:18] | dst_link[17:10] | transfer_id[9:4] | subtype[3:0]`。priority0..3 与 NLP 映射，4..7 保留；subtype=0 DATA、1 LINK_CONTROL，其余保留。源255禁止普通发送，广播仅允许限速单跳 HELLO，不转发。

DATA 的前两字节为 `fragment_index:u8 | flags:u8`，flags START=1、END=2、ABORT=4，其余 0。START 必须 index=0，随后加 `pdu_len:u16`；剩余是 PDU 字节，后续 fragment_index 严格递增。最后一段必须精确到 pdu_len 且 END，无尾部填充；CAN-FD DLC padding 在适配器中按已知总长剔除，padding 必须 0。每 transfer 不超过 256 段，因此 Classic CAN 路径最大 PDU 为 1534 字节（4+255×6），本协议的 1092 上限可覆盖。

ABORT必须为独立2字节段，index为下一个预期索引（接收方按src/dst/transfer定位已有上下文后取消），flags仅ABORT，不带长度或PDU字节；不存在的上下文忽略。LINK_CONTROL使用同样分段头与超时，仅组装内容上限64字节，不能将commissioning控制内容当作NLP PDU解析。

transfer_id 为每方向 6 位循环标识。重组超时默认100 ms（慢速总线按公开预算增加），发送端必须有对应 TX deadline 并取消过期硬件发送；ID 在终止后至少 2×重组期限内不复用，耗尽则 BUSY。重复段仅在索引、长度和内容完全相同时忽略；乱序/冲突取消 transfer。CRC/AEAD 最终校验防止误拼接，不能仅信 CAN CRC。

bus-off 必须清空物理重组/发送状态并递增链路 generation；受控恢复后重新发现和加入，不继续旧的半包。CAN-FD 不得在 Classic-only 网络中默认启用。

Classic CAN 上 52 字节 NLP 头有显著开销；事件合并、仅在状态变化时发送、输入优先和 CAN-FD 是正常设计选项。不能把 USB 1 ms interval 宣称为整个多载体路径的 1 kHz 输入保证。


### CAN-COMMISSION-1：可选无预配置地址分配

这是待实现/总线验证的确定性 UID 前缀枚举，不依赖 MCU 伪随机退避。前提是总线波特率一致、只有一个启用的 commissioning controller、设备UID唯一，且设备处于工厂未分配或物理授权的重新分配窗口。已配对设备不能被未认证的网络请求任意改址。

1. controller 地址0暂停普通NLP业务（本地键盘仍运行），先撤销受影响输入绑定。bootstrap使用LINK_CONTROL单跳分段，组装内容为 `opcode:u8 | query_token:u8 | prefix_len:u8 | reserved:u8 | body[] | crc32c:u32`，总长≤64。CRC覆盖前述全部非CRC字节，不能作为认证。
2. ENUM_PREFIX(opcode1)的body是prefix_len个UID前缀字节，prefix_len=0..15。未分配节点仅在UID匹配前缀时，发送下一UID字节的**Classic格式DLC=0**应答。
3. 应答使用保留29位ID：priority=0、src=255、dst=255，低10位为 `(next_uid_byte << 2) | (query_token & 3)`；此空间不用于普通DATA。相同next byte的节点发送完全相同的无数据帧，CAN仲裁/位流一致；不同byte通过ID仲裁，失败者在本轮期限内重试。不能在该ID下附带各不相同的UID payload。
4. controller收集本轮不同byte，逐分支扩展前缀，直到16字节UID完整。每轮只有一个query，reply不能跨轮排队；发送方取消超过本轮TX期限的硬件帧。轮间静默guard和query低2位共同拒绝迟到回复，token回绕前也须满足静默期限。
5. 轮窗口至少覆盖253个最坏DLC0扩展帧和guard：按每帧100bits预算，`window_ms >= ceil(253*100*1000/bitrate)+20`；500kbit/s默认100ms，125kbit/s至少223ms。错误重发/总线故障超出预算时本轮失败重试，不延长为无限等待。
6. ASSIGN(opcode2)使用prefix_len=16，body=`uid[16] | link_addr:u8 | reserved[3]=0 | commission_epoch:u32`。只匹配完整UID的节点改址，并从新地址回应ACK(opcode3、相同body/token)，随后完成标准HELLO/认证/加入；地址分配不是配对成功。
7. 地址1..253由controller唯一租用，资源不足停止枚举并报告。超过深度16、总查询4096或总commissioning期限则退出并清理临时状态。重复UID、异常同ID不同数据、非法匿名发送及bus-off按错误策略隔离；重新开窗不能沿用旧半包/旧回复。

这个算法不能区分物理上复制了同一UID和同一密钥的克隆设备；制造唯一性仍是前提。自发现不能替代电气验收：实现阶段必须验证同ID/DLC0多发送者、ID仲裁、硬件TX取消、迟到token、125k/500k、bus-off与已有正常设备的共存。未通过这些检查时只启用预配置地址模式。

## 8. 自发现的实际含义

发现的是“端口上可安全寻址的 NLP 邻居”，随后才是统一设备和能力目录。Wi-Fi 入网、I²C 地址冲突、SPI CS 布线、RS-485 方向和 CAN commissioning 都有载体自身的前提。对固定外设使用显式代理；代理必须标记 `origin=PROXY`、owner UID、端口和硬件身份可信度，不能冒充拥有独立设备密钥的原生 NLP 模块。
