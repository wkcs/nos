# NLK (NOS Link Kit) 分布式软总线协议与架构设计规范

> **状态:** 正式设计规范 (Specification v1.0)
> **前身:** `NDK` (`app/server/ndk/`) 与 `Connect Server` (`app/server/connect/`)
> **适用范围:** NOS 全系目标板（20 KiB `wl-001`、64 KiB `nk60-v2`/`zj-v3`、128 KiB `qemu-stm32`、多架构 `arm64`/`riscv64`/`x86_64`）与上位机/协处理器生态

---

## 1. 背景与现状诊断

### 1.1 现有 `NDK` 与 `Connect Server` 的局限性
当前仓库中同时存在两套早期点对点通信实现（`app/server/ndk/` 与 `app/server/connect/`），在演进为跨载体分布式协议时存在以下核心瓶颈：

| 维度 | 现有 `NDK` (`app/server/ndk/`) | 现有 `Connect` (`app/server/connect/`) | `NLK` (NOS Link Kit) 重新设计目标 |
| :--- | :--- | :--- | :--- |
| **物理载体绑定** | 强耦合 `winusb` 设备，控制消息依赖 USB EP0 回调 (`0x03`)，数据消息走 Bulk 端点 | 强耦合 `winusb` 设备，固定等待信号量 | **全载体无关抽象 (`nlk_bearer`)**：统一支持 USB (WinUSB/HID/CDC)、UART、SPI、I2C、CAN/CAN-FD、Wi-Fi (UDP/TCP)、BLE/2.4G 与本地微内核 IPC |
| **链路成帧与校验** | 无帧同步字 (`SOF`)，字节流下无法定界；`crc` 字段硬编码填 `0` 未校验；EP0 回调直接裸指针赋值存在并发覆盖风险 | 无 CRC 校验，仅靠简单的 `DATA0/DATA1` 翻转与固定 100ms 超时 | **自适应链路成帧 (Link Framing Profile)**：流式载体采用 `SOF + Len + HdrCRC8 + Packet + CRC16`；支持小 MTU 载体自动分片重组 (SAR) |
| **网络拓扑与级联** | 仅支持 `Host <-> 单设备` 1 对 1 直连，无地址与路由概念 | 仅支持 `Host <-> 单设备` 1 对 1 直连 | **多层树状/网状级联与零表项路由**：16-bit 层次化节点地址，支持最多 4 层级联转发与多宿主网关（USB / 蓝牙 / 2.4G / Wi-Fi 动态切换） |
| **设备与能力模型** | 顶层 `switch` 硬编码 3 条命令 (`HEART_BEAT`、`GET_VERSION`、`FW_UPDATE`) | 顶层 `switch` 硬编码 5 条命令 | **Device + Capability 双层对象模型**：对标 USB Device 与 Function，支持设备与能力自发现、描述符枚举、热插拔事件上报 |
| **通信原语** | 仅支持 `Ctrl + 固定包长 Data + Ack`，升级时硬编码 `msleep(10)` | 停等式单包读写 | **4 类标准交互通道**：`RPC`（命令请求/响应）、`Event`（低延迟事件上报）、`Stream`（双向字节流管道）、`Bulk`（滑动窗口分块传输） |

---

## 2. 核心概念与五层协议栈架构

### 2.1 两大核心抽象：设备 (`nlk_device`) 与能力 (`nlk_capability`)

NLK 借鉴 USB 架构中 **Device（设备）** 与 **Function/Interface（功能接口）** 的设计哲学，并针对嵌入式多跳总线进行了轻量化改造：

1. **NLK 设备 (`nlk_device` / Node)**
   - 软总线上的每一个物理或逻辑节点（如上位机 PC、NK60 主控、副屏模块、旋钮模块、额外小键盘、蓝牙/无线模组）都是一个 **NLK 设备**。
   - 每个设备拥有全局唯一的硬件标识（96-bit/64-bit `UUID`）、厂商/产品 ID（`VID:PID`）、设备类型（`device_class`）、级联拓扑地址（`nlk_addr_t`），并固定在 `cap_id = 0` 处暴露 **核心管理能力 (`NLK_CAP_CORE`)**。
   - 具备多个物理接口的设备可同时作为 **级联路由节点 (`Bridge / Hub`)**，向下游挂载子设备。

2. **NLK 能力 (`nlk_capability` / Function)**
   - **能力**是设备对外暴露的最小功能单元，编号为 `cap_id = 1 .. N`（类似 USB 的 Interface/Endpoint 组合）。
   - 每种标准能力由全局唯一的 16-bit **能力类型 (`cap_type`)** 标识，并规定了该能力专属的**描述符结构**与**通信报文格式**。
   - 一个 NLK 设备可自由组合并暴露多种标准或自定义能力。例如 **NK60 主键盘**可同时暴露：
     - `Cap 0`: `NLK_CAP_CORE`（设备发现、级联路由表、心跳、拓扑通知）
     - `Cap 1`: `NLK_CAP_FW_UPDATE`（固件 OTA 升级）
     - `Cap 2`: `NLK_CAP_LOG`（内核 `printk` 实时日志流与等级控制）
     - `Cap 3`: `NLK_CAP_SHELL`（远程 `nshell` 虚拟终端 TTY）
     - `Cap 4`: `NLK_CAP_KEYBOARD`（多形态键盘配列/层配置、全键无冲/磁轴RT、按键矩阵事件、HID 报表，以及绑定于键盘的 RGB 矩阵/底灯/指示灯控制与跨模块光效锁相同步）
     - `Cap 5`: `NLK_CAP_DISPLAY`（板载点阵/屏幕显示控制）

### 2.2 五层协议栈架构图

```text
+---------------------------------------------------------------------------------------------------+
| L5: Capability Application Layer (标准与扩展能力层)                                               |
|  +---------------+ +---------------+ +---------------+ +---------------+ +----------------------+ |
|  | 0x0000 CORE   | | 0x0001 FW_UPD | | 0x0002 LOG    | | 0x0003 SHELL  | | 0x0004 VFS / STORAGE | |
|  +---------------+ +---------------+ +---------------+ +---------------+ +----------------------+ |
|  +---------------------------------+ +---------------+ +---------------+ +----------------------+ |
|  | 0x0100 KEYBOARD (+RGB LIGHTING) | | 0x0101 ENCODR | | 0x0200 DISPLY | | 0x0300 WIRELESS/BLE  | |
|  +---------------------------------+ +---------------+ +---------------+ +----------------------+ |
+---------------------------------------------------------------------------------------------------+
| L4: Capability Channel & Session Layer (能力会话与交互原语层)                                     |
|  - RPC Channel    : 请求/响应/错误码 + 超时重传 (用于配置读写、控制指令)                          |
|  - Event Channel  : 单向低延迟推送，支持 Unreliable / Reliable ACK (用于按键、旋钮、状态变化)     |
|  - Stream Channel : 双向字节流管道 + 信用窗口流控 Credit Flow Control (用于 nShell、Log 流)       |
|  - Bulk Channel   : 偏移寻址 + 滑动窗口 SACK 位图 + CRC32 校验 (用于固件升级、图片/字库大文件)    |
+---------------------------------------------------------------------------------------------------+
| L3: Network, Topology & Routing Layer (网络寻址、自发现与多层路由层)                              |
|  - 16-bit 层次化树状路由地址 (O(1) 位运算无表转发，支持 4 级级联 + 多上游网关切换)                |
|  - 跨链路 MTU 自动分片与端到端重组 (L3 Fragmentation & Reassembly)                                |
|  - 分布式自发现状态机 (Beacon/Probe -> Address Assign -> Capability Enum -> Topology Event)       |
+---------------------------------------------------------------------------------------------------+
| L2: Data Link & Framing Layer (数据链路成帧与总线仲裁层)                                          |
|  - Stream Framing Profile    (UART / SPI / USB-CDC) : SOF(0x55AA) + Len + HdrCRC8 + Pkt + CRC16   |
|  - Datagram Framing Profile  (WinUSB / HID / UDP)   : Native Packet Boundary + Pkt + Optional CRC |
|  - Micro-Frame Profile       (CAN 2.0 / CAN-FD)     : 29-bit CAN ID 路由映射 + L2 微帧分片        |
|  - Master-Slave Bus Arbiter  (I2C / SPI)            : INT 中断线触发 + 自适应轮询周期调度器       |
+---------------------------------------------------------------------------------------------------+
| L1: Bearer Abstraction Layer (物理载体驱动适配层 `struct nlk_bearer`)                             |
|  - USB (WinUSB Bulk / HID) | UART (IRQ/DMA) | SPI (Master/Slave) | I2C (Master/Slave)             |
|  - CAN / CAN-FD            | Wi-Fi (UDP/TCP)| BLE / 2.4G         | NOS Endpoint IPC (本地软总线)  |
+---------------------------------------------------------------------------------------------------+
```

---

## 3. 拓扑结构、多层设备级联与寻址设计 (L3)

### 3.1 NK60 典型多层级联拓扑图

在 NK60 模块化外设生态中，NK60 既是连接上位机的从设备，又是连接下游外设模块的主控网关（Hub）：

```text
                        [ 上位机 Host PC / WebConfigurator / App ]
                                    (Addr: 0x0000)
                                    /            \
              (有线 USB WinUSB/HID) /              \ (BLE / 2.4G / Wi-Fi 无线隧道)
                                  /                \
        +------------------------------------+      [ 蓝牙 / 2.4G / Wi-Fi 无线模块 ]
        |      NK60 主键盘 (Primary Node)    | <========== (UART / SPI 高速互联)
        |           (Addr: 0x0001)           |             (Addr: 0x0041, Port 4)
        |  暴露能力: Core, FW_Update, Log,   |             暴露能力: Core, FW_Update,
        |  nShell, Keyboard, Lighting        |                       Wireless, Log
        +------------------------------------+
             /             |              \
     (Port 1: UART)  (Port 2: I2C)   (Port 3: SPI)
           /               |                \
 [ 额外按键/数字小键盘 ] [ 智能旋钮模块 ]  [ 独立屏幕模块 TFT/OLED ]
    (Addr: 0x0011)       (Addr: 0x0021)       (Addr: 0x0031)
  Cap: Keypad, Lighting  Cap: Encoder        Cap: Display, FW_Update
         |
   (Port 1: I2C 级联)
         |
 [ 二级级联旋钮/副屏 ]
    (Addr: 0x0111)
  Cap: Encoder, Display
```

### 3.2 16-bit 层次化树状地址 (`nlk_addr_t`)

为保证在 20 KiB RAM 的微控制器（如 `wl-001`）上实现**零动态路由表内存占用**与 **O(1) 级联转发**，NLK 采用 **16-bit 层次化树状编码地址**：

```text
  15      12 11       8 7        4 3        0
 +----------+----------+----------+----------+
 |  Tier 3  |  Tier 2  |  Tier 1  |  Tier 0  |
 | (Port1-F)| (Port1-F)| (Port1-F)| (Root=1) |
 +----------+----------+----------+----------+
```

- **保留与特殊地址：**
  - `0x0000` (`NLK_ADDR_HOST`)：上位机（PC / 手机 / 云端控制端）。
  - `0x0001` (`NLK_ADDR_PRIMARY`)：主控根设备（如 NK60 键盘主控，位于 Tier 0）。
  - `0xFFFE` (`NLK_ADDR_LOCAL` / `NLK_ADDR_UNASSIGNED`)：点对点直连本机默认地址（未枚举前直接可用）或待分配地址。
  - `0xFFFF` (`NLK_ADDR_BROADCAST`)：全网或当前子树广播地址（如 RGB 灯效同步帧）。
- **级联子设备地址分配规则：**
  - **Tier 0（根主控设备）**：固定为 `0x0001`。
  - **Tier 1（根主控的直接子设备，端口 `P1 = 1..15`）**：`Addr = (P1 << 4) | 0x0001`（例如端口 1 上的小键盘为 `0x0011`，端口 2 上的旋钮为 `0x0021`）。
  - **Tier 2（Tier 1 设备的下级子设备，端口 `P2 = 1..15`）**：`Addr = (P2 << 8) | (P1 << 4) | 0x0001`（例如小键盘 `0x0011` 端口 1 上级联的旋钮为 `0x0111`）。
  - **Tier 3（Tier 2 设备的下级子设备，端口 `P3 = 1..15`）**：`Addr = (P3 << 12) | (P2 << 8) | (P1 << 4) | 0x0001`（例如 `0x1111`）。

### 3.3 O(1) 无表转发算法与多宿主上游切换

任意处于第 $k$ 层（`tier = 0..3`，本机地址 `my_addr`，层级掩码 `my_mask = (1 << ((tier + 1) * 4)) - 1`）的节点收到报文时，路由判定仅需数条位运算指令：

```c
static inline int nlk_route_lookup(uint16_t my_addr, uint8_t my_tier, uint16_t dst_addr)
{
    uint16_t my_mask = (uint16_t)((1U << ((my_tier + 1U) * 4U)) - 1U);

    /* 1. 发往本机或直连别名 */
    if (dst_addr == my_addr || dst_addr == NLK_ADDR_LOCAL) {
        return NLK_ROUTE_LOCAL;
    }
    /* 2. 广播报文：本机处理 + 向所有激活下游端口泛洪 */
    if (dst_addr == NLK_ADDR_BROADCAST) {
        return NLK_ROUTE_BROADCAST;
    }
    /* 3. 目标不在当前节点的子树前缀内（包括 dst_addr == 0x0000 上位机）：向上游网关转发 */
    if (dst_addr == NLK_ADDR_HOST || (dst_addr & my_mask) != my_addr) {
        return NLK_ROUTE_UPSTREAM;
    }
    /* 4. 目标在当前节点的子树内：提取下一层 4-bit 端口号 (1..15) 直接转发 */
    uint8_t child_port = (uint8_t)((dst_addr >> ((my_tier + 1U) * 4U)) & 0x0FU);
    return (int)child_port; /* 1..15 对应下游 Bearer 端口号 */
}
```

**多上游宿主切换（USB / 蓝牙 / 2.4G / Wi-Fi）：**
- 在 NK60 (`0x0001`) 上，`NLK_ROUTE_UPSTREAM`（目标为 `0x0000` 上位机）指向当前激活的 `active_upstream_bearer`：
  - 当处于**有线模式**时，`0x0000` 路由至 USB WinUSB/HID Bearer；
  - 当切换至**蓝牙/2.4G/Wi-Fi 无线模式**时，`0x0000` 自动重绑定至连接无线模块的下游端口（如 `Port 4` 上的 `0x0041`），由无线模块将 NLK 报文通过 BLE GATT / 2.4G / Wi-Fi 透传给上位机。上层能力（固件升级、日志、nShell、改键）对有线/无线切换完全无感知。

---

## 4. 报文格式与多载体链路成帧规范 (L2 & L3)

### 4.1 统一网络与传输层报文头 (`struct nlk_pkt_hdr` — 固定 12 字节)

所有载体内部承载的 NLK 报文均采用固定 **12 字节** 小端序（Little-Endian）头部，天然 4 字节对齐：

```c
/* ver_flags 字段定义 */
#define NLK_PROTO_VER_1         (1U << 5)   /* [7:5] 协议版本 v1 */
#define NLK_FLAG_ACK_REQ        (1U << 0)   /* 请求对端回复 ACK/RESP */
#define NLK_FLAG_IS_ACK         (1U << 1)   /* 本包为响应或确认包 */
#define NLK_FLAG_IS_ERR         (1U << 2)   /* 响应发生错误，payload 首部为 int16_t err_code */
#define NLK_FLAG_FRAG           (1U << 3)   /* 本包为 L3 分片报文 */
#define NLK_FLAG_MORE_FRAG      (1U << 4)   /* 后续还有更多分片 (M bit) */

/* msg_type 高 4 位：通信模式 (Mode) */
enum nlk_comm_mode {
    NLK_MODE_SYS    = 0x0,  /* 系统控制、发现、路由与心跳 */
    NLK_MODE_RPC    = 0x1,  /* 请求-响应命令调用 (Request / Response) */
    NLK_MODE_EVENT  = 0x2,  /* 异步事件上报 (Interrupt / Pub-Sub) */
    NLK_MODE_STREAM = 0x3,  /* 连续字节流管道 (nShell TTY / Kernel Log) */
    NLK_MODE_BULK   = 0x4,  /* 大数据块滑动窗口传输 (FW Update / 资源传输) */
};

#define NLK_MSG_TYPE_MAKE(mode, ttl) ((((uint8_t)(mode) & 0x0FU) << 4) | ((uint8_t)(ttl) & 0x0FU))
#define NLK_MSG_MODE(mt)             (((uint8_t)(mt) >> 4) & 0x0FU)
#define NLK_MSG_TTL(mt)              ((uint8_t)(mt) & 0x0FU)
#define NLK_DEFAULT_TTL              8U

struct nlk_pkt_hdr {
    uint8_t  ver_flags;     /* 协议版本 [7:5] + 标志位 [4:0] */
    uint8_t  msg_type;      /* 通信模式 [7:4] + 剩余跳数 TTL [3:0] */
    uint16_t src_addr;      /* 源设备地址 (nlk_addr_t) */
    uint16_t dst_addr;      /* 目的设备地址 (nlk_addr_t) */
    uint8_t  dst_cap;       /* 目的设备上的能力实例 ID (0=CORE, 1..254=功能能力, 255=全能力广播) */
    uint8_t  cmd_op;        /* 能力内部的操作码 (Opcode) 或事件号 (Event ID) */
    uint16_t seq_id;        /* 事务序列号 / 流序号 / Bulk 块序号 */
    uint16_t payload_len;   /* 紧随其后的有效载荷字节数 */
} __attribute__((packed));

_Static_assert(sizeof(struct nlk_pkt_hdr) == 12, "nlk_pkt_hdr must be 12 bytes");
```

**当 `NLK_FLAG_FRAG == 1` 时的分片扩展头（4 字节，紧随 `nlk_pkt_hdr` 之后）：**
当端到端报文超过下一跳链路的 MTU 时，路由层或源端自动拆分为多个分片：
```c
struct nlk_frag_hdr {
    uint16_t total_len;     /* 原始完整 Payload 总长度 */
    uint16_t frag_offset;   /* 当前分片在原始 Payload 中的字节偏移 */
} __attribute__((packed));
```

---

### 4.2 多载体链路层成帧适配 (L2 Framing Profiles)

不同物理总线在包边界、可靠性和主从双工特性上差异显著。NLK 定义了 4 种链路层成帧配置（Framing Profile）：

#### Profile A：字节流成帧 (`NLK_FRAME_STREAM`) —— 适用于 UART / SPI 全双工 / USB-CDC / 蓝牙 SPP
字节流载体无硬件包边界，且易受波特率误差或热插拔毛刺干扰。采用 **`SOF + 长度 + 头部 CRC8 + 载荷 + 尾部 CRC16-CCITT`** 封装：

```text
 +--------+--------+------------+----------+------------------------+------------+
 | SOF_0  | SOF_1  | Frame_Len  | Hdr_CRC8 |   NLK Packet           | Tail_CRC16 |
 | (0x55) | (0xAA) | (2B, LE)   | (1B)     | (12B Hdr + N B Payload)| (2B, LE)   |
 +--------+--------+------------+----------+------------------------+------------+
```
- **抗死锁保护 (`Hdr_CRC8`)**：仅对 `Frame_Len`（2 字节）计算 CRC-8 (多项式 `0x07`)。若线上噪声导致 `Frame_Len` 突变为极大值（如 `0xFFFF`），接收状态机在收到第 5 个字节时即可通过 `Hdr_CRC8` 立即识别错误并重新搜索 `0x55 0xAA`，绝不会陷入长时间假死等待。
- **完整性保护 (`Tail_CRC16`)**：对 `NLK Packet`（12B 头 + Payload）计算 CRC-16/CCITT-FALSE。

#### Profile B：原生数据报成帧 (`NLK_FRAME_DATAGRAM`) —— 适用于 USB WinUSB / USB HID / Wi-Fi UDP / BLE GATT / 2.4G RF
底层硬件已提供帧边界与硬件 CRC：
- **USB WinUSB**：废弃原 `NDK` 将控制包放在 EP0、数据包放在 Bulk 的割裂做法，**所有 NLK 报文统一通过 WinUSB Bulk OUT / Bulk IN 端点收发**（支持双向异步主动推送事件与流数据），EP0 仅保留一条轻量级的 `NLK_USB_CTRL_RESET` 用于宿主异常恢复时复位协议栈状态机。
- **Wi-Fi (UDP / TCP)**：UDP 模式下单包直接承载 `[NLK Packet + CRC16]`，结合局域网 UDP 广播（端口 `19600`）天然支持零配置自发现；TCP 模式下复用 Profile A 定界。

#### Profile C：主从轮询与中断混合成帧 (`NLK_FRAME_MASTER_SLAVE`) —— 适用于 I2C / 半双工 SPI
I2C 与 SPI 的物理限制是**从设备（如 I2C 旋钮、I2C 副屏、SPI 扩展小键盘）无法主动发起总线时钟传输**。NLK 在 L2 层设计了 **主从总线仲裁器 (`nlk_ms_arbiter`)**：
1. **下行写入 (`Master -> Slave`)**：主机直接发起 Write 事务发送 `[Len(2B) | NLK Packet | CRC16(2B)]`。
2. **上行读取 (`Slave -> Master`)**：支持两种触发机制：
   - **硬件中断触发（推荐，低功耗零延迟）**：从机有事件（如按键按下、旋钮旋转）待发送时，拉低共享或独立的 `NLK_INT` 引脚；主机 GPIO 外部中断唤醒接收任务立即发起读取。
   - **自适应时隙轮询（无 `NLK_INT` 引脚时）**：
     - 对暴露了 `NLK_CAP_KEYBOARD` / `NLK_CAP_ENCODER` 的活跃输入从设备，主机以高优先级快周期（默认 `2ms ~ 5ms`）轮询；
     - 对仅暴露 `NLK_CAP_DISPLAY` 等输出型从设备，主机以慢周期（默认 `200ms ~ 500ms`）轮询心跳；
     - 若从设备连续有数据返回，自动缩短轮询间隔；空闲后自动退避。
3. **两阶段读取帧格式**：
   - 阶段 1：主机读取从机 4 字节状态寄存器 `[Magic(0xA5) | Status_Flags(1B) | Pending_Len(2B)]`；
   - 阶段 2：若 `Pending_Len > 0`，主机紧接着读取 `Pending_Len + 2 (CRC16)` 字节的完整 NLK 报文。

#### Profile D：微帧总线成帧 (`NLK_FRAME_CAN`) —— 适用于 CAN 2.0B (8B) / CAN-FD (64B)
CAN 总线单帧载荷极小（8 字节或 64 字节），但具备硬件多主仲裁与 29-bit 扩展帧 ID：
- **利用 29-bit 扩展 CAN ID 承载链路层路由与微帧分片头（L2-SAR）**：
  ```text
   28    26 25      21 20      16 15        8 7     6 5          0
  +--------+----------+----------+-----------+-------+------------+
  | Prio(3)| DstSlot  | SrcSlot  | DstCap(8) |SAR(2) | FragSeq(6) |
  +--------+----------+----------+-----------+-------+------------+
  ```
  - `Prio (3-bit)`：`0` = 紧急按键/旋钮事件（CAN 总线硬件最高优先级仲裁胜出），`2` = RPC 控制，`5` = Shell/Log 流，`7` = 固件升级大块传输（自动让路给实时按键事件）。
  - `SAR (2-bit)`：`00` = 单帧完整包（Single Frame），`01` = 首帧（First Frame），`10` = 连续帧（Consecutive Frame），`11` = 尾帧（Last Frame，末尾附带 CRC16）。

---

## 5. 设备与能力自发现协议 (`Cap 0: NLK_CAP_CORE`, `0x0000`)

每个 NLK 设备必须在 `cap_id = 0` 实现 `NLK_CAP_CORE`。它负责设备发现、动态地址分配、能力枚举、级联拓扑同步与链路保活。

### 5.1 核心管理指令集 (`NLK_CAP_CORE` Opcodes)

| Opcode | 名称 | 模式 | 方向 | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| `0x01` | `CORE_OP_HELLO` | `SYS/RPC` | 双向 | 节点上线宣告或主节点探测（携带设备摘要与 UUID） |
| `0x02` | `CORE_OP_ASSIGN_ADDR` | `RPC` | 父 -> 子 | 父节点为子节点分配 16-bit 级联地址、MTU 与心跳周期 |
| `0x03` | `CORE_OP_GET_DEV_DESC`| `RPC` | 请求 -> 目标 | 获取目标设备的详细设备描述符 (`struct nlk_device_desc`) |
| `0x04` | `CORE_OP_GET_CAP_LIST`| `RPC` | 请求 -> 目标 | 获取目标设备暴露的所有能力摘要列表 (`struct nlk_cap_item[]`) |
| `0x05` | `CORE_OP_GET_CAP_DESC`| `RPC` | 请求 -> 目标 | 获取指定 `cap_id` 的能力专属参数描述符 |
| `0x06` | `CORE_OP_GET_TOPOLOGY`| `RPC` | 请求 -> 网关 | 获取当前节点及其下游所有级联子设备的拓扑树快照 |
| `0x07` | `CORE_OP_PING` | `RPC` | 双向 | 链路心跳保活、RTT 延迟测量与拓扑版本号核对 |
| `0x08` | `CORE_OP_RESET` | `RPC` | 请求 -> 目标 | 软复位协议栈会话或重启目标设备 |
| `0x20` | `CORE_EVT_TOPO_CHANGE`| `EVENT` | 子 -> 父/Host| 下游级联设备热插拔上线 (`ATTACH`) 或离线 (`DETACH`) 事件 |

### 5.2 核心描述符数据结构

```c
/* 设备描述符 (48 字节) */
struct nlk_device_desc {
    uint16_t vendor_id;         /* 厂商 ID，例如 0x4E4B ("NK") */
    uint16_t product_id;        /* 产品 ID，例如 0x0060 (NK60-V2) */
    uint16_t device_class;      /* 主设备类型：0x01=Keyboard, 0x02=Keypad, 0x03=Knob, 0x04=Display, 0x05=Wireless */
    uint8_t  hw_ver[2];         /* 硬件版本 Major.Minor */
    uint8_t  fw_ver[4];         /* 固件版本 Major.Minor.Patch.Build */
    uint8_t  uuid[12];          /* 96-bit 硬件唯一 ID (如 STM32 UID) */
    char     name[16];          /* 设备名称 (以 '\0' 结尾，如 "NK60-V2") */
    uint16_t max_payload_size;  /* 本设备支持的最大重组 Payload 字节数 (如 256 / 1024) */
    uint8_t  cap_count;         /* 暴露的能力数量 (不含 Cap 0) */
    uint8_t  dev_flags;         /* BIT0: 支持级联下游子设备, BIT1: 电池供电, BIT2: 支持休眠唤醒 */
    uint16_t parent_addr;       /* 父节点地址 */
    uint8_t  attached_children; /* 当前已挂载的直接子设备掩码/数量 */
    uint8_t  reserved;
} __attribute__((packed));

/* 单个能力摘要项 (8 字节，由 CORE_OP_GET_CAP_LIST 返回数组) */
struct nlk_cap_item {
    uint8_t  cap_id;            /* 设备内能力实例编号 (1 .. cap_count) */
    uint8_t  cap_ver;           /* 能力协议版本 [7:4]Major . [3:0]Minor */
    uint16_t cap_type;          /* 全局标准能力类型 ID (见第 6 节) */
    uint8_t  sup_modes;         /* 支持的交互模式掩码: BIT0=RPC, BIT1=EVENT, BIT2=STREAM, BIT3=BULK */
    uint8_t  sub_class;         /* 能力子类型 (如 Display: OLED/TFT/RGB_MATRIX) */
    uint16_t desc_size;         /* 能力专属扩展描述符长度 (可通过 CORE_OP_GET_CAP_DESC 读取) */
} __attribute__((packed));
```

### 5.3 自发现与多层枚举时序图

```text
[上位机 Host]              [NK60 主控 (0x0001)]         [数字小键盘 (Port 1)]        [级联旋钮 (Port 1.1)]
  (0x0000)                                                  (0x0011)                    (0x0111)
     |                              |                           |                           |
     |                              | <--- 1. CORE_OP_HELLO --- |                           |
     |                              | --- 2. ASSIGN_ADDR -----> | (分配 0x0011, MTU=256)    |
     |                              | --- 3. GET_CAP_LIST ----> |                           |
     |                              | <--- [Cap1:Keypad, -------|                           |
     |                              |       Cap2:Lighting]      |                           |
     |                              |                           | <--- 4. I2C IRQ/HELLO --- |
     |                              |                           | --- 5. ASSIGN_ADDR -----> | (分配 0x0111)
     |                              |                           | --- 6. GET_CAP_LIST ----> |
     |                              |                           | <--- [Cap1:Encoder] ------|
     |                              | <--- 7. EVT_TOPO_CHANGE --| (上报新节点 0x0111 上线)  |
     | <--- 8. EVT_TOPO_CHANGE ---- | (汇总上报 0x0011 & 0x0111)|                           |
     |                              |                           |                           |
     | --- 9. GET_DEV_DESC (dst=0x0111, cap=0) -------------------------------------------> | (O(1) 跨两跳路由)
     | <-- 10. DEV_DESC_RESP (src=0x0111, "NK-Knob-Mini") --------------------------------- |
```

---

## 6. 标准能力体系与通信格式规范 (L5)

NLK 将能力划分为四大标准能力族：
1. **系统与运维能力族 (`0x0001` – `0x00FF`)**：固件升级、内核日志、远程 nShell、文件访问、系统监控。
2. **人机输入能力族 (`0x0100` – `0x01FF`)**：键盘矩阵与配列、旋钮编码器、扩展宏按键、指点设备。
3. **视觉与反馈能力族 (`0x0200` – `0x02FF`)**：屏幕显示、RGB 灯效同步、蜂鸣器/线性马达。
4. **无线与桥接能力族 (`0x0300` – `0x03FF`)**：蓝牙 BLE、2.4G 专有无线、Wi-Fi 协处理器。

---

### 6.1 固件升级能力 (`NLK_CAP_FW_UPDATE`, `cap_type = 0x0001`)

用于升级本机（如 NK60）或通过级联路由升级任意子设备（如屏幕、旋钮、无线模块）的 Bootloader / Kernel A / Kernel B / 资源分区。

#### 通信模式与操作码
- `FW_OP_GET_PART_INFO (0x01)` [`RPC`]：查询分区表、当前运行槽位、Flash 擦写粒度及推荐窗口大小。
- `FW_OP_PREPARE       (0x02)` [`RPC`]：携带目标分区、固件总大小、预期 `CRC32`、镜像魔数（`0x20302030`）。目标设备校验空间并执行分区擦除。
- `FW_OP_WRITE_CHUNK   (0x03)` [`BULK`]：分块写入固件数据，支持**滑动窗口批量确认（Sliding Window SACK）**，彻底消除旧版 `cmd_fw_update.c` 中的 `msleep(10)` 死等待。
- `FW_OP_VERIFY_COMMIT (0x04)` [`RPC`]：触发全片 CRC32 校验并写入启动标志位（`REBOOT_TO_KERNEL_A/B`）。
- `FW_OP_REBOOT        (0x05)` [`RPC`]：先回复 ACK，延迟 50ms 后调用 `cpu_reboot()`。

#### 报文载荷格式
```c
/* FW_OP_PREPARE 请求载荷 (16 字节) */
struct nlk_fw_prepare_req {
    uint8_t  part_index;        /* 0=BOOTLOADER, 1=KERNEL_A, 2=KERNEL_B, 3=ASSETS */
    uint8_t  flags;             /* BIT0: 写入完成后自动校验提交 */
    uint16_t window_pkts;       /* 期望的滑动窗口包数 (1..8) */
    uint32_t total_size;        /* 固件镜像总字节数 */
    uint32_t image_crc32;       /* 完整镜像 CRC32 */
    uint32_t version_code;      /* 新固件版本号 */
} __attribute__((packed));

/* FW_OP_WRITE_CHUNK 载荷 (Bulk 模式) */
struct nlk_fw_chunk_pkt {
    uint32_t offset;            /* 镜像内字节偏移 (幂等写入，支持乱序/重传去重) */
    uint8_t  data[];            /* 实际数据，长度由 L3 payload_len - 4 决定 */
} __attribute__((packed));

/* FW_OP_WRITE_CHUNK 窗口确认响应 (8 字节) */
struct nlk_fw_chunk_ack {
    uint32_t acked_offset;      /* 已连续成功写入 Flash 的累计字节偏移 */
    uint16_t rx_window_bitmap;  /* 滑动窗口内各包接收位图 (用于选择性重传 SACK) */
    int16_t  status;            /* 0=正常, <0 为 Flash 写入错误码 (-EIO/-EINVAL) */
} __attribute__((packed));
```

---

### 6.2 内核日志能力 (`NLK_CAP_LOG`, `cap_type = 0x0002`)

与 NOS 内核日志子系统（`kernel/printk/printk.c` 与 `kernel/printk/log.c`）深度集成，支持远程实时抓取内核日志、动态调整日志级别及查询丢包水位。

#### 通信模式与操作码
- `LOG_OP_SUBSCRIBE  (0x01)` [`RPC`]：订阅或取消订阅实时日志流。
- `LOG_OP_SET_LEVEL  (0x02)` [`RPC`]：调用 `set_log_level()` / `set_log_enabled()` 动态修改运行时日志级别（`LOG_ALL` .. `LOG_DISABLE`）。
- `LOG_OP_GET_STATS  (0x03)` [`RPC`]：调用 `kernel_log_get_stats()` 获取 `dropped_records`、`truncated_records`、`high_water`。
- `LOG_STREAM_RECORD (0x10)` [`STREAM`, 设备 -> 订阅端]：异步推送结构化日志记录。

#### 报文载荷格式
```c
/* LOG_OP_SUBSCRIBE 请求 (4 字节) */
struct nlk_log_sub_req {
    uint8_t  enable;            /* 1=开启流推送, 0=停止推送 */
    uint8_t  min_level;         /* 最低推送等级 (LOG_DEBUG..LOG_FATAL) */
    uint16_t flush_interval_ms; /* 聚合发送间隔 (0=立即发送, >0 按时间片合并多条短日志) */
} __attribute__((packed));

/* LOG_STREAM_RECORD 数据帧 (直接复用 kernel_log_record 8 字节头 + 文本) */
struct nlk_log_wire_record {
    uint32_t timestamp_ms;      /* 32 位毫秒时间戳 */
    uint16_t length;            /* 日志文本长度 */
    uint8_t  level;             /* 日志等级 */
    uint8_t  flags;             /* LOG_RECORD_UNTAGGED 等标志 */
    char     msg[];             /* UTF-8 / ASCII 日志内容 (可多条连续打包在一个 Stream 包中) */
} __attribute__((packed));
```
> **架构安全约束：** `NLK_CAP_LOG` 严禁在 `pr_log()` 持锁路径或关中断上下文中直接调用阻塞式总线发送，必须由独立的 NLK 工作线程或异步排空钩子从日志环形缓冲区读取后打包发送，且 NLK 内部调试日志需标记 `NLK_LOG_NO_ECHO` 防止递归死循环。

---

### 6.3 远程终端能力 (`NLK_CAP_SHELL`, `cap_type = 0x0003`)

基于 `include/nos/tty.h` 的 `struct nos_tty` 抽象，注册一个虚拟终端后端 `nlk_tty`。上位机可通过 USB、Wi-Fi、蓝牙甚至跨级联 I2C/SPI 直接打开任意节点的 `nshell` 交互式命令行。

#### 通信模式与操作码
- `SHELL_OP_OPEN       (0x01)` [`RPC`]：打开或接管远程 Shell 会话，协商初始接收窗口信用值（`rx_credit`）与终端行列数。
- `SHELL_OP_CLOSE      (0x02)` [`RPC`]：关闭远程 Shell 会话，恢复本地 UART 控制台。
- `SHELL_OP_SIGNAL     (0x03)` [`RPC`]：发送控制信号（如 `SIGINT` 中断当前命令、清屏复位）。
- `SHELL_OP_FLOW_CRED  (0x04)` [`STREAM`]：接收端向发送端补充窗口字节信用额度（防止大段脚本粘贴导致小内存 MCU RX FIFO 溢出）。
- `SHELL_STREAM_STDIN  (0x10)` [`STREAM`, 客户端 -> 设备]：终端输入字节流（支持 ANSI 转义序列、方向键、退格）。
- `SHELL_STREAM_STDOUT (0x11)` [`STREAM`, 设备 -> 客户端]：终端输出字节流。

---

### 6.4 多形态键盘与集成灯光能力 (`NLK_CAP_KEYBOARD`, `cap_type = 0x0100` / `NLK_CAP_KEYPAD`, `0x0102`)

此能力同时服务于两个方向：
1. **下行模块互联（额外按键模块 / 独立数字小键盘 / 分体键盘副手 -> NK60 主控）**：上报物理按键矩阵按下/抬起增量事件。
2. **上行主机互联（主键盘 <-> 上位机驱动软件 / Web 配置器）**：支持全形态键盘（100%/96%/80% TKL/75%/65%/60%/40%/Split 分体/Ortholinear 直列/Alice/Numpad/Macropad）与多轴体技术（普通机械轴、磁轴/模拟霍尔轴 Rapid Trigger、光轴、静电容轴），支持在线读写多层配列（`DEF_LAYER`/`FN_LAYER`/`PN_LAYER`/`MO3`）、全键无冲 (NKRO) / 6KRO 报表，并将键盘绑定的单键 RGB 矩阵/底灯/状态指示灯以及多模块灯光相位同步信标统一内聚于键盘能力中。

#### 通信模式与操作码
- `KB_OP_GET_DESC            (0x01)` [`RPC`]：获取键盘形态 (`form_factor`)、轴体类型 (`switch_type`)、特性标志 (`kb_flags`)、矩阵行列数、按键总数、支持层数、LED 矩阵规格及级联模块掩码。
- `KB_OP_GET_KEYMAP    (0x02)` [`RPC/BULK`]：读取指定层 (`layer`) 与模块 (`module_id`) 的键值表 (`key_code`) 与动作表 (`key_action`)。
- `KB_OP_SET_KEYMAP    (0x03)` [`RPC/BULK`]：在线修改按键映射与动作（支持持久化到 Flash/VFS）。
- `KB_OP_GET_SET_PARAM (0x04)` [`RPC`]：读写扫描频率、消抖时间 (`debounce_ms`)、NKRO（全键无冲）/ 6KRO 模式、WinLock 状态。
- `KB_OP_GET_LIGHT_CFG       (0x05)` [`RPC`]：读取键盘绑定的灯效配置（开关、分区掩码、模式、亮度、速度、主色调）。
- `KB_OP_SET_LIGHT_CFG       (0x06)` [`RPC`]：设置键盘灯效模式（静态、呼吸、霓虹渐变、单点亮、涟漪、直通流）及参数。
- `KB_OP_SET_INDICATORS      (0x07)` [`RPC`]：设置键盘状态指示灯（NumLock / CapsLock / ScrollLock / Compose / Kana）。
- `KB_OP_GET_ANALOG_CFG      (0x08)` [`RPC`]：读取磁轴/模拟轴触发行程与 Rapid Trigger (RT) 灵敏度配置。
- `KB_OP_SET_ANALOG_CFG      (0x09)` [`RPC`]：设置磁轴/模拟轴触发行程 (`actuation_01mm`)、RT 触发/抬起灵敏度及死区。
- `KB_OP_GET_NKRO_REPORT     (0x0A)` [`RPC`]：获取 128 键全键无冲 (NKRO) 位图报表。
- `KB_STREAM_LIGHT_FRAME     (0x10)` [`STREAM`]：上位机实时 RGB 灯效流（如 SignalRGB / 屏幕环境光直通帧）。
- `KB_EVT_SWITCH_DELTA (0x20)` [`EVENT`, 扩展按键模块 -> NK60 主控 / 上位机]：按键状态增量上报（高实时性，CAN/I2C/UART 最高优先级）。
- `KB_EVT_HID_REPORT   (0x21)` [`EVENT`, NK60 主控 -> 无线模块 / 上位机]：8 字节标准 6KRO 报表或 NKRO 位图报表。
- `KB_EVT_LIGHT_SYNC_BEACON  (0x22)` [`EVENT`, 广播 `dst=0xFFFF`]：主键盘定期广播 8 字节灯光锁相信标 (`struct nlk_kb_light_sync_beacon`)，使级联小键盘、分体副手键盘、宏键盘实现跨设备无缝流光同步。
- `KB_EVT_NKRO_REPORT        (0x23)` [`EVENT`]：18 字节全键无冲 (NKRO) 位图报表事件。

#### 报文载荷格式
```c
/* KB_OP_GET_DESC 响应 (20 字节) */
struct nlk_kb_desc {
    uint8_t  form_factor;       /* 键盘形态: 100%/96%/TKL/75%/65%/60%/40%/Split/Ortho/Alice/Numpad/Macropad */
    uint8_t  switch_type;       /* 轴体类型: MECH / HALL_ANALOG / OPTICAL / EC / LOW_PROFILE / SCISSOR */
    uint16_t kb_flags;          /* 特性位图: 6KRO/NKRO/SPLIT/MODULAR/ANALOG_RT/LIGHT_RGB_KEY/UNDERGLOW/INDICATORS */
    uint8_t  matrix_rows;       /* 矩阵行数 (如 NK60 为 5) */
    uint8_t  matrix_cols;       /* 矩阵列数 (如 NK60 为 14) */
    uint8_t  key_count;         /* 实际按键数量 (如 NK60 为 61) */
    uint8_t  layer_count;       /* 支持的层数 (1..4 层: DEF, FN, PN, MO3) */
    uint8_t  debounce_ms;       /* 当前消抖毫秒数 */
    uint8_t  led_matrix_rows;   /* RGB 灯阵行数 */
    uint8_t  led_matrix_cols;   /* RGB 灯阵列数 */
    uint8_t  underglow_leds;    /* 底灯/氛围灯数量 */
    uint16_t led_count;         /* 可寻址 LED 总数 */
    uint16_t poll_rate_hz;      /* 矩阵扫描率 (如 1000Hz) */
    uint32_t ext_module_mask;   /* 已级联的外部按键模块位图 */
} __attribute__((packed));

/* KB_EVT_SWITCH_DELTA 增量按键事件 (适用于额外按键模块 -> NK60 主控，或矩阵监控) */
struct nlk_kb_switch_event {
    uint8_t  module_id;         /* 0=主键盘本体, 1..15=级联扩展按键模块编号 */
    uint8_t  active_layer;      /* 当前激活层 (由主控或本地判定) */
    uint8_t  event_count;       /* 本包包含的按键变化数量 (1..N) */
    uint8_t  reserved;
    struct {
        uint8_t key_index;      /* 物理按键编号 (0 .. key_count-1) */
        uint8_t state;          /* 0=KEY_UP, 1=KEY_DOWN, 2=KEY_HOLD */
        uint8_t key_code;       /* 模块本地映射键值 (KC_*)，若由主控统一映射可填 KC_UNDEFINED */
        uint8_t action;         /* 关联动作 (KA_*) */
    } items[];
} __attribute__((packed));

/* KB_EVT_HID_REPORT 报表事件 (用于 NK60 -> 蓝牙/2.4G 模块或上位机) */
struct nlk_kb_hid_report {
    uint8_t  report_id;         /* 1=标准 8B 6KRO 键盘, 2=多媒体键, 3=系统控制, 4=NKRO 位图 */
    uint8_t  modifiers;         /* E0..E7 修饰键位图 (LCTRL/LSHIFT/LALT/LGUI/...) */
    uint8_t  reserved;
    uint8_t  keys[6];           /* 6KRO 普通键值或变长位图 */
} __attribute__((packed));
```

---

### 6.5 旋钮编码器能力 (`NLK_CAP_ENCODER`, `cap_type = 0x0101`)

用于连接独立旋钮模块、带屏智能旋钮或键盘板载多路编码器：

- `ENC_OP_GET_DESC    (0x01)` [`RPC`]：获取编码器数量、每圈脉冲步数 (`steps_per_rev`)、是否带按压开关、是否具备力反馈电机（Smart Haptic Knob）。
- `ENC_OP_SET_CONFIG  (0x02)` [`RPC`]：配置各层下的顺时针 (`cw_keycode`)、逆时针 (`ccw_keycode`)、按下 (`press_keycode`)、按住旋转映射，或配置智能旋钮的阻尼/棘轮齿数（`detent_count`）。
- `ENC_EVT_CHANGE     (0x20)` [`EVENT`, 旋钮模块 -> NK60 主控]：
  ```c
  struct nlk_encoder_event {
      uint8_t  encoder_id;      /* 编码器索引 (0..N) */
      uint8_t  button_state;    /* BIT0: 按下状态, BIT1: 双击, BIT2: 长按 */
      int16_t  delta_steps;     /* 相对旋转步数 (正数=顺时针 CW, 负数=逆时针 CCW) */
      uint16_t abs_angle_deg10; /* 可选：磁编码器绝对角度 (0.1 度精度, 0..3599) */
  } __attribute__((packed));
  ```

---

### 6.6 屏幕显示能力 (`NLK_CAP_DISPLAY`, `cap_type = 0x0200`)

为同时兼容 **低速总线（I2C / UART / CAN）** 上的副屏与 **高速总线（SPI / USB / Wi-Fi）** 上的彩屏，NLK 显示能力设计了 **双模渲染协议**：

1. **模式一：语义化状态组件模式（Semantic Widget Mode，仅需几十字节/秒，I2C/UART 极速响应）**
   - NK60 主控或上位机只下发结构化状态数据，由屏幕模块本地 MCU 运行 LVGL / 点阵字库完成 UI 绘制：
   - `DISP_OP_UPDATE_STATUS (0x02)` [`RPC/EVENT`]：
     ```c
     struct nlk_disp_status_sync {
         uint8_t  active_layer;     /* 当前层: 0=DEF, 1=FN, 2=PN */
         uint8_t  lock_leds;        /* BIT0=NumLock, BIT1=CapsLock, BIT2=ScrollLock, BIT3=WinLock */
         uint8_t  conn_type;        /* 0=USB, 1=BT1, 2=BT2, 3=BT3, 4=2.4G, 5=Wi-Fi */
         uint8_t  conn_state;       /* 0=Disconnected, 1=Pairing, 2=Connected */
         uint8_t  battery_pct;      /* 电池电量百分比 (0..100, 0xFF=外部供电) */
         uint8_t  cpu_usage_pct;    /* 主控或上位机 CPU 占用率 (0..100) */
         uint16_t current_wpm;      /* 实时打字速度 WPM */
         uint32_t unix_timestamp;   /* 系统实时时间 */
         char     custom_label[16]; /* 自定义提示文本 (如当前应用/切歌标题) */
     } __attribute__((packed));
     ```
2. **模式二：脏矩形帧缓冲与资源传输模式（Framebuffer & Asset Mode，适用于 SPI / USB / Wi-Fi）**
   - `DISP_OP_GET_DESC      (0x01)` [`RPC`]：查询屏幕分辨率 (`width`, `height`)、色彩格式 (`MONO_1BPP`, `RGB565`, `RGB888`)、最大刷新率。
   - `DISP_OP_DIRTY_RECT    (0x10)` [`STREAM/BULK`]：推送局部窗口像素 `{ uint16_t x, y, w, h; uint8_t codec (0=RAW, 1=RLE, 2=LZ4); uint8_t pixels[]; }`。
   - `DISP_OP_UPLOAD_ASSET  (0x11)` [`BULK`]：上传自定义 GIF 动图、开机 Logo 或主题字库到屏幕模块本地存储。

---

### 6.7 蓝牙与无线连接能力 (`NLK_CAP_WIRELESS`, `cap_type = 0x0300`)

当 NK60 通过 UART 或 SPI 连接外部蓝牙/2.4G/Wi-Fi 模组（如 nRF52 / CH582 / ESP32）时，无线模组作为一个标准 NLK 设备挂载在 NK60 下游端口（例如 `0x0041`）：

- `WL_OP_GET_INFO         (0x01)` [`RPC`]：查询无线模组支持的制式（BLE 5.x / 2.4G / Wi-Fi）、MAC 地址、已配对宿主槽位列表。
- `WL_OP_SWITCH_CHANNEL   (0x02)` [`RPC`]：切换无线通道（`BT_SLOT_1`, `BT_SLOT_2`, `BT_SLOT_3`, `RF_24G`）、进入配对广播模式（`START_PAIRING`）、清除配对信息或控制无线休眠。
- `WL_EVT_LINK_STATE      (0x20)` [`EVENT`, 无线模块 -> NK60 主控]：上报连接状态变化、无线信号强度 `RSSI`、宿主下发的 CapsLock/NumLock 指示灯状态。
- **双通道协同（HID 快速通道 + NLK 软总线隧道）**：
  1. **HID 快速通道**：NK60 按键扫描产生的报表通过 `KB_EVT_HID_REPORT` 发给无线模块，无线模块直接通过 BLE HID Service 或 2.4G 专有端点极速发出（端到端延迟 `< 1ms`）。
  2. **NLK 软总线隧道（NLK-over-BLE / NLK-over-2.4G / NLK-over-WiFi）**：无线模块在 BLE GATT（自定义 Service UUID）或 2.4G/Wi-Fi 上同时暴露一个 **NLK Tunnel 特征值/端口**。上位机通过无线发出的任何 NLK 报文（如读取键盘电量、无线修改配列、无线打开 `nshell`、无线升级固件），无线模块直接作为 L3 路由包转发给 NK60 (`0x0001`) 及其级联子设备！

---

## 7. NOS 内核与服务层软件架构设计

### 7.1 目录结构规划（替代原 `app/server/ndk` 与 `app/server/connect`）

遵循 `docs/ARCHITECTURE_AND_BUILD_SPEC.md` 的四层头文件与模块化原则：

```text
include/
├── uapi/nos/
│   └── nlk.h                      # 【UAPI 层】NLK 协议固定报文头、标准能力 ID、核心描述符定义（跨平台与上位机共享）
└── nos/
    ├── nlk_core.h                 # NLK 核心路由、节点管理、能力注册 API
    ├── nlk_bearer.h               # 物理载体抽象层 (Bearer Ops) 与成帧状态机接口
    └── nlk_caps.h                 # 标准能力组件公共接口
app/server/nlk/                    # （或演进至 servers/nlk/）
├── CMakeLists.txt
├── Kconfig
├── core/
│   ├── nlk_core.c                 # 协议栈主循环、能力表管理、RPC/Event/Stream/Bulk 分发
│   ├── nlk_router.c               # 16-bit 树状路由转发、TTL 检查、跨 MTU 分片与重组 (SAR)
│   ├── nlk_discovery.c            # Cap 0 (NLK_CAP_CORE) 自发现状态机、地址分配与心跳保活
│   └── nlk_frame.c                # L2 成帧编解码器 (SOF+HdrCRC8+CRC16) 与主从轮询仲裁器
├── bearers/
│   ├── bearer_usb_winusb.c        # 对接 drivers/usb/device/class/winusb.c (纯 Bulk 收发)
│   ├── bearer_uart.c              # 对接 drivers/serial/ (支持 DMA/IRQ 字节流)
│   ├── bearer_i2c.c               # 对接 drivers/i2c/ (支持主从模式 + INT 引脚触发/自适应轮询)
│   ├── bearer_spi.c               # 对接 drivers/spi/
│   ├── bearer_can.c               # CAN / CAN-FD 29-bit ID 映射与 L2 微帧分片
│   └── bearer_ipc.c               # 对接微内核 nos_endpoint (本地跨进程软总线)
├── nlk_server.c                   # 平台无关、载体无关的通用 NLK 软总线守护服务
└── caps/                          # 100% 硬件无关的公共标准能力实现
    ├── cap_fw_update.c            # 0x0001: 通用滑动窗口固件升级 (分区数量/大小/擦写由板级注入)
    ├── cap_log.c                  # 0x0002: 对接 kernel_log_read_record / set_log_level
    ├── cap_shell.c                # 0x0003: 实现 nlk_tty_ops 对接 nos_tty 与 nshell
    ├── cap_keyboard.c             # 0x0100: 多形态键盘、级联按键聚合及键盘集成灯光/同步信标
    ├── cap_encoder.c              # 0x0101: 旋钮编码器能力
    ├── cap_display.c              # 0x0200: 屏幕状态同步与局部帧传输
    └── cap_wireless.c             # 0x0300: 蓝牙/2.4G/Wi-Fi 模组控制与上行网关切换
board/nk60-v2/
└── nk60_nlk.c                     # NK60-V2 板级专属 NLK 配置 (W25QXX 分区表、61键配列、WinUSB 载体挂载)
```

### 7.2 核心 C 接口契约

#### 1. 物理载体抽象接口 (`include/nos/nlk_bearer.h`)
```c
enum nlk_bearer_type {
    NLK_BEARER_USB_WINUSB = 0,
    NLK_BEARER_USB_HID,
    NLK_BEARER_UART,
    NLK_BEARER_SPI,
    NLK_BEARER_I2C,
    NLK_BEARER_CAN,
    NLK_BEARER_WIFI,
    NLK_BEARER_BLE_RF,
    NLK_BEARER_LOCAL_IPC,
};

enum nlk_bearer_role {
    NLK_ROLE_UPSTREAM_SLAVE = 0,    /* 连接上游主机或父节点 (如 USB Device, I2C Slave) */
    NLK_ROLE_DOWNSTREAM_MASTER,     /* 连接下游级联子设备 (如 I2C Master, UART Port) */
    NLK_ROLE_PEER,                  /* 对等全双工链路 (如 CAN, Wi-Fi UDP, UART Peer) */
};

struct nlk_bearer;

struct nlk_bearer_ops {
    int (*init)(struct nlk_bearer *b);
    int (*send_frame)(struct nlk_bearer *b, uint16_t next_hop_addr,
                      const void *pkt_buf, uint16_t pkt_len, uint32_t timeout_ms);
    int (*poll_slave)(struct nlk_bearer *b, uint8_t slave_slot); /* 仅 I2C/SPI Master 使用 */
    int (*reset)(struct nlk_bearer *b);
};

struct nlk_bearer {
    const char                 *name;
    enum nlk_bearer_type        type;
    enum nlk_bearer_role        role;
    uint8_t                     port_id;        /* 0=默认上游, 1..15=下游级联端口号 */
    uint8_t                     framing_profile;/* STREAM / DATAGRAM / MASTER_SLAVE / CAN */
    uint16_t                    mtu;            /* 链路单帧最大 NLK 载荷 (如 CAN=64, UART=256, USB=1024) */
    const struct nlk_bearer_ops *ops;
    void                       *priv;
    struct list_head            node;
};

int nlk_bearer_register(struct nlk_bearer *bearer);
/* 底层驱动收到完整帧或字节流解码完成后调用此入口上交 L3 路由层 */
int nlk_bearer_rx_input(struct nlk_bearer *bearer, const void *pkt_buf, uint16_t pkt_len);
```

#### 2. 能力注册与通信接口 (`include/nos/nlk_core.h`)
```c
struct nlk_capability;

struct nlk_cap_ops {
    int (*init)(struct nlk_capability *cap);
    int (*get_desc)(struct nlk_capability *cap, void *buf, uint16_t max_len);
    int (*on_rpc)(struct nlk_capability *cap, const struct nlk_pkt_hdr *hdr,
                  const void *req, uint16_t req_len,
                  void *resp, uint16_t *resp_len);
    int (*on_event)(struct nlk_capability *cap, const struct nlk_pkt_hdr *hdr,
                    const void *evt, uint16_t evt_len);
    int (*on_stream)(struct nlk_capability *cap, const struct nlk_pkt_hdr *hdr,
                     const void *data, uint16_t len);
    int (*on_bulk)(struct nlk_capability *cap, const struct nlk_pkt_hdr *hdr,
                   uint32_t offset, const void *data, uint16_t len);
};

struct nlk_capability {
    uint8_t                    cap_id;          /* 注册时分配 (1..N) */
    uint8_t                    cap_ver;         /* 版本号 */
    uint16_t                   cap_type;        /* NLK_CAP_* */
    uint8_t                    sup_modes;       /* 支持的通信模式掩码 */
    uint8_t                    sub_class;
    const char                *name;
    const struct nlk_cap_ops  *ops;
    void                      *priv;
};

/* 注册本机对外暴露的能力 (支持编译期静态表或初始化注册) */
int nlk_cap_register(struct nlk_capability *cap);

/* 主动发送接口 (目标可为 0x0000 上位机、0x0001 主控、或任意级联节点 0x0111) */
int nlk_rpc_call(uint16_t dst_addr, uint8_t dst_cap, uint8_t cmd_op,
                 const void *req, uint16_t req_len,
                 void *resp, uint16_t *resp_len, uint32_t timeout_ms);

int nlk_send_event(uint16_t dst_addr, uint8_t dst_cap, uint8_t evt_op,
                   const void *evt, uint16_t evt_len, bool ack_req);

int nlk_stream_write(uint16_t dst_addr, uint8_t dst_cap, uint8_t stream_op,
                     const void *data, uint16_t len);
```

---

## 8. 静态内存预算与可裁剪性 (Kconfig)

为确保从 **20 KiB RAM 的 `wl-001`（叶子模块/按键旋钮从机）** 到 **64 KiB RAM 的 `nk60-v2`（级联主控网关）** 均能稳定运行，NLK 采用全静态缓冲池与编译期裁剪策略：

| 配置档位 | 典型目标板 | 开启特性 | 静态 RAM 占用预算 |
| :--- | :--- | :--- | :--- |
| **Leaf Nano 档** (`CONFIG_NLK_ROLE_LEAF`) | `wl-001` (20 KiB RAM)、独立旋钮、额外按键、I2C 副屏 | 仅单上游 Bearer (UART/I2C/CAN)，关闭级联路由转发与分片重组，MTU=128B，静态双缓冲 | **~ 420 Bytes**（RX 帧缓冲 148B + TX 帧缓冲 148B + 节点与 3 个能力描述符 124B） |
| **Hub Standard 档** (`CONFIG_NLK_ROLE_HUB`) | `nk60-v2` / `zj-v3` (64 KiB RAM) | 开启上游 USB + 下游 UART/I2C/SPI 多 Bearer、树状路由转发、自发现枚举、FW_UPDATE / LOG / SHELL / KEYBOARD / LIGHTING 全能力 | **~ 2.8 KiB**（4 块 256B 路由包缓冲池 1024B + Bulk/USB 1024B 缓冲 + 子树拓扑缓存 384B + Shell/Log 管道 384B） |
| **Full Rich 档** (`CONFIG_NLK_ROLE_FULL`) | `qemu-stm32` (128 KiB) / `arm64` / `x86_64` | 开启完整 8 窗口并发 Bulk、L3 多会话分片重组、Wi-Fi UDP/TCP 与本地微内核 IPC 桥接 | **~ 8 KiB – 16 KiB** |

### 核心 Kconfig 选项设计
- `CONFIG_NLK`：启用 NLK (NOS Link Kit) 分布式软总线协议栈（替代 `CONFIG_NDK_SERVER` 与 `CONFIG_CONNECT_SERVER`）。
- `CONFIG_NLK_ROLE_HUB`：启用多端口级联路由、下游设备自发现与热插拔拓扑管理。
- `CONFIG_NLK_MAX_CHILDREN`：单节点最大直接下游子设备数（默认 `4`，最大 `15`）。
- `CONFIG_NLK_PACKET_MTU`：默认最大包载荷（Leaf 档默认 `128`，Hub 档默认 `256` 或 `1024`）。
- `CONFIG_NLK_BEARER_WINUSB` / `UART` / `I2C` / `SPI` / `CAN` / `WIFI` / `IPC`：按需勾选物理载体适配器。
- `CONFIG_NLK_CAP_FW_UPDATE` / `LOG` / `SHELL` / `KEYBOARD` / `ENCODER` / `DISPLAY` / `LIGHTING` / `WIRELESS`：按需勾选本机暴露的标准能力。

---

## 9. 分阶段落地路线图 (Implementation Plan)

1. **阶段 1：核心协议头与无硬件依赖的 L2/L3/L4 协议栈内核（含 Host 单元测试）**
   - 新增 `include/uapi/nos/nlk.h`、`include/nos/nlk_core.h`、`include/nos/nlk_bearer.h`。
   - 实现 `nlk_frame.c`（`SOF + HdrCRC8 + CRC16` 状态机）、`nlk_router.c`（16-bit 树状地址 O(1) 路由与 TTL）、`nlk_discovery.c`（`Cap 0` 自发现、地址分配与能力表枚举）及 `nlk_core.c`。
   - 在 `tests/host/` 中编写主机回归测试：模拟 `Host(0x0000) <-> NK60(0x0001) <-> Keypad(0x0011) <-> Knob(0x0111)` 四级级联虚拟拓扑，验证自动地址分配、跨跳能力枚举、半包/粘包/错包恢复与断线重连。

2. **阶段 2：系统基础能力落地（固件升级、远程日志、远程 nShell）与 WinUSB/UART 载体接入**
   - 实现 `bearer_usb_winusb.c`（将 WinUSB 改为统一 Bulk 收发）与 `bearer_uart.c`。
   - 实现 `cap_fw_update.c`、`cap_log.c`（对接 `kernel/printk/log.c`）、`cap_shell.c`（实现 `nlk_tty` 对接 `nos_tty`）。
   - 在 `qemu-stm32` 与 `nk60-v2` 上验证通过 NLK 抓取日志、交互执行 `nshell` 命令及固件升级流程，正式移除遗留的 `app/server/connect/` 与 `app/server/ndk/`。

3. **阶段 3：NK60 模块化外设能力闭环（键盘、额外按键、旋钮、屏幕、RGB 灯效、无线模块）**
   - 实现 `cap_keyboard.c`、`cap_encoder.c`、`cap_display.c`、`cap_lighting.c`、`cap_wireless.c`。
   - 实现 `bearer_i2c.c`（含 `NLK_INT` 与自适应轮询仲裁器）、`bearer_spi.c`、`bearer_can.c`。
   - 将 `drivers/key/nk60_v2_key.c` 与 `app/display/nk60_display_server.c` 接入 NLK 能力层，支持外接按键/旋钮事件聚合与跨模块灯光同步。
