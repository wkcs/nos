# NLP v1 身份、授权与重放

<!-- Copyright (C) 2026 NOS contributors -->

本页为协议设计，不是已实现/审计的密码学。校验和、USB 连接、CAN ACK、已加入 Wi-Fi 和能力自报都不构成业务授权。

## 1. 信任建立

默认通过显式配对绑定 `device_uid ↔ device static public key`；可用工厂证书/预置指纹，或本地物理确认后的 TOFU。相同 UID/不同 key 拒绝并报告冲突；名称、拓扑位置、短地址不能覆盖已存信任。更换设备/转移所有权有明确撤销与重新配对流程。

初次发现只公开最小 HELLO 信息，控制预认证状态数量/大小/超时。无线链路必须认证和加密后才能开放日志、shell、输入绑定、改键、无线凭据和升级。物理总线可显式配置 `PHYSICALLY_TRUSTED` 模式支持无密码学小模块，但描述符/host UI 必须标示；不能通过这种桥把一个物理弱信任模块升级成独立认证身份。

## 2. 密码学 profile 与会话键

v1 安全 profile 提案为 `Noise_XX_25519_ChaChaPoly_SHA256`，静态 key 在配对时校验。使用成熟库；不能自行把 CRC、XOR 或口令直接当加密。Noise 握手只在受限核心 AUTH 消息中传输，基线128字节payload可容纳空应用payload的XX握手消息；更大的证书链必须显式分块/单独profile，不能无界塞入握手。

Noise prologue 绑定 NLP major、fabric UID/epoch、双方 UID/boot、握手 nonce、管理 session ID 和公开协商参数。AUTH 成功形成端点间安全 association，在有有效节点租约后打开 capability0 的管理 session。随后 OPEN 在已认证管理会话内申请具体能力；能力版本、功能、授权范围和新 session ID 进入派生上下文。重建 association 关闭旧会话，不能保留旧 key/计数器。

同一 association 中由 UID 按字节序较小的端点统一分配 session_id；使用该分配端本次 boot 的单调全局计数，不重复。较大端通过管理请求申请 ID。碰撞/倒退拒绝；计数器耗尽需换 boot/重新入网，不能回绕。

从 Noise Split 结果和 handshake hash 用 HKDF-SHA256 派生各方向 master，再以固定标签 `NLP1/channel` 加 session_id、channel_id、src/dst UID、boot和能力绑定派生方向 key[32] 与 nonce_prefix[6]。不能把两个方向或两个能力的数据放在同一 key/nonce 域。HKDF-Extract 的 salt 为 handshake_hash[32]、IKM 为该方向 Split key[32]。HKDF-Expand info 依次为 ASCII `NLP1/channel`（12字节）、session_id:u32、channel_id:u16、src_uid[16]、dst_uid[16]、src_boot:u64、dst_boot:u64、src_cap:u16、dst_cap:u16、type_uuid[16]、major:u16、minor:u16；src/dst按该方向定义，类型与版本为会话绑定值，输出38字节分成key[32]和prefix[6]。不能用含糊字符串拼接。

业务 AEAD 使用 IETF ChaCha20-Poly1305：nonce=`prefix[6] | channel_id:u16 LE | packet_counter:u32 LE`，恰12字节，tag16字节；AAD 按 wire 文档归零 hop/头CRC。packet_counter 每通道/方向独立增长，重传复制原包，任何新的 payload/ack/credit 组合必须使用新 counter。密钥更新通过新 association/session 完成，v1 不做原地重置 counter。

F401 不能被假定有硬件随机数源。安全握手需要经过验证的熵/CSPRNG；可用生产注入的高熵种子加可靠、抗回滚的启动计数驱动DRBG，但必须验证持久性和断电行为。缺少安全熵/计数时返回 UNSUPPORTED 或保持受限物理模式，不能悄悄切成时间戳随机的“安全模式”。

## 3. 去重与重放

- 未认证数据不进入业务序号、信用和 RPC 缓存，不能通过伪造 ACK 释放发送缓冲。
- 可靠数据使用不回绕的 reliable_seq；`seq <= 已消费水位` 不再交付，窗口内新seq在认证后接收，乱序缓存有界。不能仅因 ACK/其他流推进了短 counter 位图，就丢掉合法的旧计数器可靠重传。
- 非可靠数据用每通道/方向的64包 counter replay window；过旧、重复丢弃。ACK 仅能增加已确认水位和 receive_limit，旧ACK不回退状态；receive_limit已授予的信用不可撤回，不能把旧窗口重新当成额外额度。
- 纯ACK和控制也使用新 counter；同一密钥域禁止 nonce 重用。通道ID关闭后不在同一session内重用；会话/boot/epoch变化后旧包不再匹配。
- 握手响应必须匹配本次 nonce/阶段/端点绑定；静态描述符不能替代握手证明。开销高的操作前有长度、配额、超时和配对状态检查。

## 4. 业务授权

授权记录至少包含 `(peer_uid/key, target_uid, capability_id/type, allowed_ops, expiry, limits)`。标准角色拆成 DISCOVER、OBSERVE_INPUT、BIND_INPUT、CONFIG_WRITE、LOG_READ、SHELL、FIRMWARE_STAGE、FIRMWARE_APPLY、RADIO_CONFIG。观察按键不赋予注入权，上传不赋予启动新固件权。

物理解锁授予短期、目标和 peer 绑定的授权，不是全网通用 token。NLP 设备可能经过多个桥，末端必须检查自己的授权；桥不能用自己的更高权限把下游请求当成本地管理员调用。路由控制由协调者管理，业务授权仍由能力 owner 决定。

拓扑公开也应限速/限可见范围。未知模块默认不参与输入、输出owner或自动升级；用户确认后绑定具体 UID/boot/能力和动作。配置/firmware 缓冲区与共享总线驱动的内存、DMA 权限保持现有 NOS 隔离边界。

## 5. 固件信任与可恢复性

传输配对密钥与固件签名根分离；管理员连上设备不能任意改变固件验证根。使用签名 manifest 的产品/硬件/component 范围、SHA256、版本和 rollback_index；引导器再次独立验证实际待启动镜像。

只有具备经过验证的 staging/双槽元数据、尝试次数、确认/回退与恢复入口时才能广告 APPLY。CRC 双槽日志只提供撕裂写入检测，不自动提供抗回滚或安全启动。没有 bootloader/可信计数硬件时，要如实声明升级模式，不能把普通外置 Flash 写成功称为安全 OTA 已完成。

## 6. 路由器与资源攻击

中间节点只需邻接信任、源子树检查、epoch/路由/队列限额，不持有所有端点业务密钥；故障或恶意桥仍能丢包、延迟和观察流量元数据，协议不承诺防 DoS。明文头暴露节点/能力索引和流量长度；NLP v1 不提供匿名路由。

验证头/总长后才重组或查会话；限制每端口未认证peer、每peer重组、目录条目、会话、流、未完成RPC和消息速率。队列满返回 BUSY/NO_SPACE 或按明确日志策略丢弃，不占用无限堆，也不能阻塞键盘全释放和授权撤销。

参考资料：[Noise framework](https://noiseprotocol.org/noise.html)、[RFC 8439](https://www.rfc-editor.org/rfc/rfc8439)、[RFC 5869](https://www.rfc-editor.org/rfc/rfc5869)、[RFC 9562](https://www.rfc-editor.org/rfc/rfc9562)。这些是设计依据；互通与安全审计仍是后续验收任务。


## 7. 入网前的邻接确认

未分配节点的AUTH仅限已发现的直接物理邻居，prologue已绑定所选fabric/epoch而不依赖临时node_id。Noise完成后，双方先建立受限邻接association；JOIN由父节点代理协调者处理，候选端尚不能发送路由业务。JOIN_REQUEST/ACCEPT用control TLV22携带独立HMAC-SHA256，防止明文JOIN地址/租约在握手后被替换。

bootstrap方向key为HKDF-SHA256(Extract salt=handshake_hash, IKM=该方向Split key；Expand info=`b"NLP1/join" | management_session_id:u32 | src_uid[16] | dst_uid[16] | src_boot:u64 | dst_boot:u64 | fabric_uid[16] | fabric_epoch:u32`，输出32字节)。HMAC输入为规范头（hop与header CRC归零，但payload_len包含末尾36字节认证TLV）加完整CONTROL payload、去掉末尾的tag22 TLV。数值仍按wire小端，tag22必须最后、长度32；不与业务AEAD复用key。HMAC只在Noise认证完成后验证；control_id从1单调、无回绕，已完成id只允许逐字节相同的重复并返回原结果，状态有界且超时即销毁邻接association，不能跨boot恢复。

此profile及Noise/KDF具体集成仍须独立安全审查和互通向量；本次参考codec不生成或验证HMAC/AEAD，普通CRC通过不能被描述为已认证JOIN。
