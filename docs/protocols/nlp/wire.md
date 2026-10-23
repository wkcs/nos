# NLP v1 报文、会话与可靠性

<!-- Copyright (C) 2026 NOS contributors -->

本页定义 [NLP 实验实现](README.md) 的 wire；扩展目标与实际接口限制见 [实现状态](implementation.md)。权威数字见 `protocols/nlp/v1/registry.json`。所有整数为小端；UUID 是 RFC 9562 字节顺序的 16 个原始字节（不是 Windows GUID 混合端序），摘要为原始字节，字符串为有明确长度的 UTF-8，不带 NUL。不得直接序列化 C 结构、指针、枚举宽度或 padding。

## 1. 固定头与完整包

每个 PDU 为 `header[52] | payload[payload_len] | trailer`。明文 trailer 是 CRC32C 4 字节；加密 trailer 是 AEAD tag 16 字节，密文和明文长度相同。全局 payload 上限 1024 字节，实际取端点、会话和路径的更小值；MCU 基础配置为 128 字节。未实现扩展前，v1 `header_len` 必须恰为 52。

| 偏移 | 字段 | 字节 | 规则 |
| --- | --- | --- | --- |
| 0 | magic | 2 | ASCII `NL`，十六进制 `4e 4c` |
| 2 | major | 1 | 1 |
| 3 | header_len | 1 | 52 |
| 4 | kind | 1 | CONTROL=0、REQUEST=1、RESPONSE=2、EVENT=3、DATA=4、ACK=5、CLOSE=6 |
| 5 | flags | 1 | bit0 RELIABLE，bit1 ENCRYPTED，其余必须 0 |
| 6 | priority | 1 | 0 控制、1 输入/交互、2 普通业务、3 日志/批量；不是授权 |
| 7 | hop_limit | 1 | 1..16，仅转发时递减；1只能本地递送，不能再转发 |
| 8 | fabric_epoch | 4 | 当前域代次；0 仅 bootstrap |
| 12 | src_node | 2 | 域内源节点地址 |
| 14 | dst_node | 2 | 域内目的节点地址 |
| 16 | src_cap | 2 | 源能力实例/客户端端点 0 |
| 18 | dst_cap | 2 | 目的能力实例 |
| 20 | channel_id | 2 | 会话内通道，0 为会话控制 |
| 22 | opcode | 2 | CONTROL/CLOSE及cap0管理RPC使用核心表，其余按会话绑定的能力类型解释 |
| 24 | session_id | 4 | 端点间会话标识；0 为 HELLO/JOIN/AUTH 等受限预会话消息 |
| 28 | packet_counter | 4 | 同一会话、通道、方向从 1 单调递增；用于 nonce/重放 |
| 32 | reliable_seq | 4 | 可靠业务包从 1 单调递增；非可靠/纯 ACK 为 0 |
| 36 | ack | 4 | 对端方向已认证并有界接收的连续 reliable_seq，初始 0 |
| 40 | payload_len | 2 | 不含头与 trailer |
| 42 | reserved | 2 | 必须 0 |
| 44 | receive_limit | 4 | 对端可以发送的最大 reliable_seq，累计信用右边界 |
| 48 | header_crc32c | 4 | CRC32C(header[0:48]) |

CRC32C 使用 Castagnoli：反射多项式 `0x82f63b78`，初值和最终 XOR 均 `0xffffffff`，校验串 `123456789 → 0xe3069283`，wire 为小端 `83 92 06 e3`。明文帧 CRC32C 覆盖完整 52 字节头（含头 CRC）和 payload；它不提供身份认证。

接收顺序必须是：限额内分帧 → 头长/版本/保留位/头 CRC → 无溢出的完整长度检查 → trailer/AEAD → 源地址/会话/能力权限 → 序号/去重/信用 → 能力格式 → 调用。不能按外部 length 直接分配内存。非法 CRC、认证失败、超长或截断帧丢弃；只有已建立、已授权的端点可收到有界错误响应。

路由器只修改 hop_limit 并重算头 CRC；明文还需重算帧 CRC。加密 AAD 是完整头的副本，将 hop_limit 与 header_crc32c 清零，其他字节保持；转发者无须业务密钥。hop_limit 是限环机制，不能作为对恶意已入网路由器的安全边界。

## 2. 管理与描述格式

CONTROL 的固定数字注册在核心表：HELLO、HELLO_ACK、JOIN_REQUEST/ACCEPT、LEASE_RENEW/REVOKE、GET_DEVICE、LIST_CAPS、GET_CAP_DESC、GET_PORTS、TOPO_SUBSCRIBE/EVENT、ROUTE_ERROR、OPEN（响应同opcode）、CLOSE、PING/PONG、AUTH_START/STEP/FINISH、ROUTE_INSTALL/RESULT。

预会话包必须 `session_id=counter=seq=ack=receive_limit=0`，不设置 RELIABLE/ENCRYPTED；kind 为 CONTROL，端点和 channel 均为 0。未分配节点的HELLO必须 epoch=0、src=65534、dst=65535、hop=1；回复按物理邻居寻址。该邻居上下文内允许单跳AUTH/JOIN，src/dst可用65534表示未分配端、epoch取已选择fabric代次，不能跨路由器转发。已有地址的 AUTH 可定向路由，仍必须满足入网策略、速率和长度限制。预会话 nonce/握手 ID 位于管理 TLV 中，不借 packet_counter 伪装成已认证会话。

CONTROL 用于注册的 HELLO/JOIN/AUTH/PING/PONG/拓扑事件等核心消息，payload 先带 `control_id:u32 | status:u16 | reserved:u16=0`，其后为操作 TLV；HELLO 等发起消息使用非0 control_id，配对响应回显该值，主动拓扑事件用事件序号。AUTH_STEP 的 TLV 必须包含握手阶段。GET_DEVICE/LIST_CAPS/GET_CAP_DESC/GET_PORTS/OPEN 等管理 RPC 使用 REQUEST/RESPONSE、双方cap=0和下文RPC前缀，不与同opcode的CONTROL混用。完整目录默认在 AUTH 后读取，公开发现只依赖最小HELLO。

管理 payload 由明确的操作字段和 TLV 组成。TLV 为 `tag:u16 | length:u16 | value[length]`，无 padding；tag 高位表示 MUST_UNDERSTAND，低 15 位为编号。按编号升序，同一容器内编号不得重复（列表使用一个TLV承载多个条目）。未知可选项跳过，未知必须项拒绝整个操作。最大 32 项，单个 value 不超过协商 payload；列表使用带 revision 的 cursor 分页，不拆开一个 TLV。只读分页返回的 cursor 是不透明的、绑定 revision 的服务端令牌，不是远端内存地址。

最小 HELLO TLV：发现 nonce:u32、device_uid[16]、boot_id:u64、wire major 区间、最大 payload:u16、端口角色:u16、安全模式位、资源配置 ID。GET_DEVICE 返回 UID/boot、revision、产品/硬件/固件、端口/能力数；LIST_CAPS 返回完整条目 `id:u16 | flags:u16 | type_uuid[16] | major:u16 | minor:u16 | descriptor_revision:u32`（28 字节；flags bit0 PROVIDE、bit1 CONSUME、bit2 PROXY，其余0），并带本页 cursor/revision；每页最后注明 MORE 与 next_cursor。GET_CAP_DESC 另返回该能力的功能位、权限需求、格式/通道/消息上限。列表和描述须来自同一个快照，否则返回 STALE。

核心操作码表（cap0管理上下文）：

| opcode | 名称 |
| --- | --- |
| 1 | HELLO |
| 2 | HELLO_ACK |
| 3 | JOIN_REQUEST |
| 4 | JOIN_ACCEPT |
| 5 | LEASE_RENEW |
| 6 | LEASE_REVOKE |
| 7 | GET_DEVICE |
| 8 | LIST_CAPS |
| 9 | GET_CAP_DESC |
| 10 | GET_PORTS |
| 11 | TOPO_SUBSCRIBE |
| 12 | TOPO_EVENT |
| 13 | ROUTE_ERROR |
| 14 | OPEN |
| 15 | CLOSE |
| 16 | PING |
| 17 | PONG |
| 18 | AUTH_START |
| 19 | AUTH_STEP |
| 20 | AUTH_FINISH |
| 21 | ROUTE_INSTALL |
| 22 | ROUTE_RESULT |
| 23 | GET_TOPOLOGY |

## 3. 打开与关闭

`AUTH → MANAGEMENT_ACTIVE → OPEN → ACTIVE → DRAINING → CLOSED`。AUTH 的 nonce、UID/boot/fabric/epoch 及协商结果进入密码学 transcript，成功建立端点安全association，并在有效地址租约就绪后激活capability0管理session。由UID较小端使用本次boot的单调全局计数分配管理和业务session_id；另一端通过管理RPC申请。OPEN在已认证管理session内申请一项能力、角色、major/minor、功能位、可靠模式、最大 payload、最大在途请求和租约；OPEN 的 RESPONSE 返回选择值或明确错误，不能悄悄取消认证要求。

每个会话绑定两端 UID/boot/节点租约、能力实例和类型版本。默认 channel 0 管会话、1 管业务；当前实现固定两个通道；更多通道需新配置和互通验证。CLOSE(kind6)使用核心CLOSE opcode和8字节控制前缀。SDK 本地撤销时在 channel0 尝试发送无需信用的非可靠 CLOSE，安全会话仍必须 AEAD 认证和重放检查；可靠 CLOSE 也可接收。显式断链仍以本地撤销为准。session_id 在同一端点对/启动内不复用；channel ID 在会话内不复用。双方方向分别维护状态，不能把一个方向的序号当作另一个方向的确认。认证association是内部密钥上下文；设备与能力仍是公开API的两个对象，不能绕过OPEN直接访问别的能力。

序号、计数器和信用均为 u32，**不做模回绕**；接近上限时排空并重新建立会话。源设备重启不能沿用旧密钥、session_id 或计数器。CLOSE 先停止新业务，再有界排空；断链/授权撤销立即结束，不等待对端配合。

## 4. 可靠性与流控

- 可靠通道采用累计 ACK 和有界滑动窗口：当前最大 2 帧，默认 1 帧；注册表全局上限保留给后续配置。接收端有多少实际槽才授予多少信用；乱序只占已授予的槽。`receive_limit >= ack` 且只能增加，不能用新的“小窗口”撤回已授予信用。
- `reliable_seq` 仅为可靠 REQUEST/RESPONSE/EVENT/DATA/会话控制递增。纯 ACK 没有 payload、opcode=0、seq=0、不设 RELIABLE，自身不需要再确认；它仍有新 packet_counter，并可携带 ack/receive_limit。非可靠通道 ack/limit 均为 0。
- ACK 只表示认证且收进有界收件箱。信用要在消费者释放槽后才增加；应用未取走时可以 ACK 前进、limit 不变。这不表示 RPC 已执行或数据已持久化。
- 收到窗口内的乱序帧暂存，不越序交付；重复 seq 不再交付，只回当前 ACK。已消费的 seq 永不重新执行。应用拒绝格式/权限时仍要结束该 seq 的处理，并回复错误或关闭通道，不能无限重试同一坏消息。
- 重传保留完全相同的端到端密文、packet_counter、seq、ack/limit 等字段；路由 hop/头 CRC 可变化。更新捎带确认或信用必须构造一个新计数器的包。发送重传帧不得重新加密同一 nonce 下的不同内容。
- 当前初始 RTO 100 ms，指数退避至 2 s，最多 5 次重传后关闭；尚未实现 RTT 自适应或慢路径 RTO 协商。
- 控制保留队列至少 2 项，数据用尽信用时仍可处理 ACK/CLOSE/撤销。日志拥塞先丢日志，升级/显示不能独占输入队列；当前保留控制类缓冲，并由合作式 poll 限制业务工作量；通用多端口加权调度仍需平台实现。载体不可中断的最大传输时长须计入端口预算。
- 非可靠通道适用于可丢日志和遥测，使用 packet_counter 防重放；不得用于必须保留的键盘按下/释放或 shell 输入。对端断开时返回明确结果，不跨会话重发副作用。

加密 nonce 和重放规则见 [安全设计](security.md)。可靠数据的重放由不回绕的 seq 和已消费状态约束；不能单凭一个跨通道的短 counter 位图丢掉合法重传。channel 分开计数，避免大日志流淘汰交互流的旧包。

## 5. RPC 和业务结果

REQUEST/RESPONSE payload 前 8 字节为 `request_id:u32 | status:u16 | flags:u8 | reserved:u8`。请求 status/flags/reserved 为 0；响应 request_id/opcode 必须匹配，flags/reserved 也为 0。错误 status 使用协议注册值，与本地 errno 数值无关；成功响应后是该操作的数据，错误后可有有界可选诊断 TLV，不能当成成功数据解析。

request_id 从 1 递增，单方向/会话内不复用，最多 2 个在途（可协商至 8）。接收端用 session+request_id+opcode+请求内容摘要去重：同 ID/不同内容返回 CONFLICT；处理中返回 BUSY/当前状态；已完成返回缓存结果。缓存可有界，但必须保留单调完成水位；结果已淘汰的旧 ID 返回 OUTCOME_UNKNOWN，不得重新执行。请求顺序超出窗口直接拒绝。

传输重发、RPC 重发、跨重启恢复是三个层次。传输 ACK 不承诺 exactly-once；跨重启的升级提交、配置应用等必须由能力自己的持久 transaction ID/generation 与 QUERY 实现幂等。shell 字节与相对旋钮事件不得自动跨会话重放。

状态：OK=0，UNSUPPORTED=1，INVALID=2，DENIED=3，BUSY=4，NOT_FOUND=5，STALE=6，NO_SPACE=7，INTEGRITY=8，RANGE=9，TIMEOUT=10，CONFLICT=11，OUTCOME_UNKNOWN=12，DISCONNECTED=13，NO_ROUTE=14，MTU_TOO_SMALL=15。路由错误只说明路径问题，不可冒充远端能力的业务成功/失败。

## 6. 版本与对象大小

能力描述和 OPEN 都明确各自版本及资源限制。路径 payload_limit 是所有转发节点和双方的最小值；路径变窄必须关闭并重开，不允许中间节点无界重新分片。长度计算使用减法/上界检查，先验证坐标、计数及乘积，再访问缓冲区。

参考 codec 与固定向量只覆盖明文头、CRC、TLV 和 UART/TCP 封装。未知标志、加密帧、错误 CRC、截断、尾部垃圾、超大长度都不能被参考解码器当作有效业务消息。

## 7. 管理字段注册与分页细则

本节补齐最小互通所需格式；所有 RPC body 仍位于8字节RPC前缀之后。字段版本与本草案一起演进，不用 JSON 文本替代 wire。

CONTROL TLV 编号如下（数值也在注册表）：

| tag | 类型 | 含义 |
| --- | --- | --- |
| 1 | u32 | NONCE，HELLO发起者的发现nonce，HELLO_ACK回显 |
| 2 / 3 | bytes[16] / u64 | DEVICE_UID / BOOT_ID |
| 4 | u8,u8 | WIRE_RANGE，最小/最大major，v1均1 |
| 5 | u16 | MAX_PAYLOAD，v1范围128..1024，MCU基础128 |
| 6 | u16 | PORT_ROLES：bit0可做上行，bit1可接下级，bit2可做协调者 |
| 7 | u16 | SECURITY_MODES：bit0受限物理模式，bit1 Noise_XX；选择结果进入transcript |
| 8 | u16 | RESOURCE_PROFILE：1 MCU_BASE / 2 GENERAL，不能替代实际限额协商 |
| 9 / 10 / 11 / 12 | bytes[16] / u32 / u16 / u32 | FABRIC_UID / FABRIC_EPOCH / NODE_ID / LEASE_MS |
| 13 / 14 | bytes[16] / bytes[16*n] | PARENT_UID / ANCESTORS，根到父，n≤8，分页或压缩不得伪造祖先 |
| 15 / 16 / 17 | u8 / bytes[≤96] / u32 | HANDSHAKE_STAGE(1..3) / NOISE_MESSAGE / MANAGEMENT_SESSION_ID |
| 18 / 19 / 20 / 21 | u32 / u8 / u16 / bytes[16] | ROUTE_REVISION / ROUTE_ACTION(1 PREPARE / 2 COMMIT / 3 REVOKE) / ROUTE_NODE / ROUTE_CHILD_UID |

HELLO/ACK必须包含tag1..8。已入域的HELLO_ACK另含tag9/10，供候选端验证和选择fabric。JOIN_REQUEST使用9/10/13/14，JOIN_ACCEPT使用9..12/18。LEASE_RENEW/REVOKE包含9..12。PING/PONG仅8字节控制前缀，PONG回显control_id。AUTH_START/STEP/FINISH分别携带stage1/2/3的Noise消息，并带17；候选management session_id由较小UID方预留，较大UID方先以不带16的AUTH_START请求分配，不得自行抢号。Noise transcript确认前该ID不产生已授权会话。

加入前的Noise association先用于认证相邻节点；尚无地址租约时不激活管理session。此时JOIN_REQUEST/ACCEPT必须附最后一个tag22 AUTHENTICATOR（HMAC-SHA256[32]），由Noise Split方向key派生的独立bootstrap key保护，见security。JOIN控制id单调且不复用，响应匹配本次请求；业务、目录和无认证JOIN都不能借此进入ACTIVE。获得有效JOIN_ACCEPT后才将已认证association绑定节点租约并激活管理session。受限物理模式可省HMAC，但必须在本地端口策略中显式启用，不能作为安全模式协商降级。

8层祖先列表可能超过MCU payload，因此JOIN_REQUEST仅发给直接父节点，tag14可省略；父节点从已提交拓扑补全并由协调者验证，不能信任候选设备自报路径。协调者与已加入路由器通过受权、可靠且加密的cap0管理session交换ROUTE_INSTALL(21)/RESULT(22)，使用CONTROL格式及tag12/18..21；安装指向一个已验证直接子邻居。PREPARE逐级确认均OK后，根以相同revision向所有祖先发送COMMIT，全部确认后才允许父节点向候选设备发送JOIN_ACCEPT；错误回滚已准备项，超时未提交项不得转发业务，已提交项也受tag12指定租约约束。撤销先停止新业务并失效子树会话，再回收地址，不能立即把旧地址配给新UID。父节点对未分配邻居的JOIN代理是显式管理功能，不转发任意匿名PDU。节点需先完成受限邻居认证，路由控制不得使用未认证HELLO的自报身份授权。

`GET_DEVICE`请求body为空；成功body=`revision:u32 | device_uid[16] | boot_id:u64 | capability_count:u16 | port_count:u16 | info_tlvs[]`，共32字节固定区。info TLV局部tag1产品UTF-8≤32、2硬件版本u32、3固件版本u64；超出一包则截短展示名，不能截UID/版本。

`LIST_CAPS / GET_PORTS`请求为 `revision:u32 | cursor:u32 | max_entries:u16 | reserved:u16=0`，首次revision/cursor=0。成功body固定12字节：`revision:u32 | next_cursor:u32 | count:u16 | flags:u16`，随后count个完整条目；flags仅bit0 MORE。MORE=0时next_cursor=0，MORE=1时cursor必须推进且count>0；客户端有页数/总项配额。revision变化返回STALE。

LIST_CAPS条目为上文28字节。GET_PORTS条目固定16字节：`port_id:u16 | transport:u8 | state:u8 | roles:u16 | max_payload:u16 | neighbor_node:u16 | reserved:u16=0 | link_generation:u32`。transport=1 USB/2 IP/3 UART/4 I2C/5 SPI/6 CAN；state=0 DOWN/1 PROBING/2 NEIGHBOR/3 JOINING/4 ACTIVE；尚无邻居时neighbor_node=0。端口的IP/物理地址属于受权扩展，不能把该索引误当永久设备身份。

`GET_CAP_DESC`请求为 `revision:u32 | capability_id:u16 | reserved:u16=0 | cursor:u32`。成功body是相同12字节分页头，count为完整TLV数，后接descriptor TLV；不得把一个TLV拆成两个。局部tag：1 roles:u8(PROVIDE=1/CONSUME=2位图)，2 origin:u8(NATIVE=0/PROXY=1)，3 owner_uid[16]，4 features:u64，5 required_permissions:u32，6 max_payload:u16，7 max_channels:u8，8 max_rpc_inflight:u8，9最小/最大输入租约两个u16，10 format_name UTF-8≤32。tag1..8必须最终齐全；不能用分页省略不支持的安全限制。权限位n对应security文档角色列表从0开始的顺序。

PROVIDE表示产生该类型数据/执行服务；CONSUME表示接收同类型数据（例如蓝牙keyboard sink）。OPEN先选择提供数据的源能力和消费者端点：普通host用cap0客户端端点，模块可用声明CONSUME的同类型能力实例。双方cap绑定/方向进入密钥派生，sink不能借订阅改变来源身份。

`OPEN`请求固定52字节：`proposed_session_id:u32 | target_cap:u16 | local_cap:u16 | type_uuid[16] | major:u16 | minor:u16 | features:u64 | permissions:u32 | max_payload:u16 | reliable:u8 | max_channels:u8 | max_rpc_inflight:u8 | reserved[3]=0 | lease_ms:u32`。较小UID发起时填其预留的新ID，较大UID发起时填0请求对端分配。可靠模式0非可靠/1可靠，其余拒绝；权限是申请，不能自授。成功body固定24字节：`session_id:u32 | features:u64 | max_payload:u16 | reliable:u8 | max_channels:u8 | max_rpc_inflight:u8 | reserved[3]=0 | lease_ms:u32`；选择值不得超过请求与路径/端点配额。首帧可靠发送前，每通道双方先用ACK(seq=ack=0)授予已预留的初始receive_limit；OPEN成功本身不授予数据池信用。

`TOPO_SUBSCRIBE`请求body为空，响应为当前directory_revision:u32和event_seq:u32。CONTROL TOPO_EVENT带tag2/3/11/18，status为OK，control_id为事件序号；tag19在此上下文为event_kind:u8（1 DEVICE_ONLINE、2 DEVICE_REMOVED、3 CAP_ADDED、4 CAP_CHANGED、5 CAP_REMOVED），tag20在CAP事件中为capability_id:u16。事件只提示失效，不含完整目录；任何序号间隙都重新分页同步。ROUTE_ERROR的status为NO_ROUTE/MTU_TOO_SMALL/TIMEOUT，仅向已知已认证源回报，不代替原业务响应。

这里只规定注册操作；新增公共管理操作必须先分配opcode和字段定义，不能让“管理TLV”成为随意执行私有命令的通道。

## 8. 当前目录与路由控制编码

GET_TOPOLOGY(23) 是管理 REQUEST/RESPONSE。请求12字节：`revision:u32, cursor:u32, max_entries:u16, reserved:u16=0`；首次 revision/cursor 为0。响应12字节：`revision:u32, next_cursor:u32, count:u16, more:u8, reserved:u8=0`，后接40字节条目：`uid[16], boot:u64, node:u16, parent:u16, path_payload:u16, cap_count:u16, descriptor_revision:u32, remaining_lease_ms:u32`。快照变化返回 STALE。没有全域目录时返回本地与直接对端。TOPO_SUBSCRIBE/TOPO_EVENT/ROUTE_ERROR 仍保留注册号，不能推断已实现推送。

ROUTE_INSTALL(21)/ROUTE_RESULT(22) 使用管理会话 channel0 的 CONTROL 格式，权限 COORDINATE(bit9)。TLV2 为目标UID、12为租约ms、18为revision、19为action、20为短地址、21为下一跳子设备UID；PREPARE/COMMIT/REVOKE 的 control_id 与 revision 一致。中间节点仅经应用提供的 UID→直接端口查询安装路由。协调者等待全部祖先 COMMIT 确认才公布新节点。路由器在本地首发时不减 hop，转发才递减；hop1可本地递送。
