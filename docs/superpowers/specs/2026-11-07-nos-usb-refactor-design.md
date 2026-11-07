# NOS USB 协议栈重构与安全加固设计规范

- **文档状态:** 待评审 (Draft)
- **创建日期:** 2026-11-07
- **目标版本:** NOS v2.0
- **涵盖范围:** USB 公共规范层 (`usb_common.h`)、Device Core 核心状态机、类驱动 (WinUSB/HID/CDC) 流控安全、底层硬件驱动 (STM32F4/F1) 内存访问与端点资源规整、未来 USB Host 模式扩展接口。

---

## 1. 背景与目标

### 1.1 现状与技术背景
NOS 当前的 USB 协议栈基于早期 RT-Thread USB Device 栈移植改造，整体设计仅针对单一 USB 从机（Device）模式。在长期的演进中累积了较多历史包袱，且与 NOS 当前微内核/模块化架构存在明显分层冲突：
1. **公共层职责混淆**：`include/drivers/usb/usb_common.h` 包含了大量 Device 私有状态机枚举、内嵌内核链表的专属描述符以及 Mass Storage 专有报文，破坏了 USB 基础规范的纯粹性，阻碍了未来接入 USB Host（主机）栈。
2. **硬件驱动重复拷贝**：STM32F1 的 USB 驱动被原样拷贝在 `board/qemu-stm32/`、`board/wl-001/` 和 `board/zj-v3/` 三个板级目录下，违反了 SoC 外设归 `soc/`、板级仅管理引脚与供电的架构规范。
3. **单例模式硬编码**：全局静态单一变量导致系统无法支持多控制器并发或双角色（Dual-Role）切换。
4. **存在 7 大高危实现缺陷**：
   - 控制请求边界检查不足（WinUSB 缓冲区溢出、OS 描述符越界读取、非法接口号空指针访问）；
   - 异步发送引用栈内存（字符串描述符、HID 上报在函数返回后仍被中断异步读取）；
   - 底层尾包复制越界（F4 DFIFO 整字读写越界、F1 PMA 奇数字节越界读写）；
   - EP0 状态机缺少完整事务边界（ISR 与任务并发竞态、新 SETUP 未重置在途传输、ZLP 判定违背 USB 2.0 规范）；
   - 复位、取消配置和断线收尾不一致（`SET_CONFIG(0)` 漏收尾、重复配置丢接收、WinUSB 阻塞读写挂起）；
   - Bulk OUT 缺少背压（FIFO 满载时静默丢包仍向主机回复 ACK）；
   - 端点资源规划缺失（F401 FIFO 物理超界、F103 PMA 与 BDT 表重叠、端点分配失败盲目继续）。

### 1.2 阶段目标
本设计规范聚焦于**阶段一：Device 栈与底层硬件驱动的系统化重构与 7 大缺陷彻底修复**，同时确立纯净的公共 Chapter 9 规范契约，为**阶段二（USB Host 双模支持）**打下坚实基础。

---

## 2. 总体架构与分层设计

重构后的 USB 子系统采用严格分层架构：

```
+-------------------------------------------------------------+
|              Application / Server / VFS Layer               |
|      NLK WinUSB Bearer, Keyboard Server (HID), TTY / VCOM    |
+-------------------------------------------------------------+
|                     USB Class Drivers                       |
|   drivers/usb/device/class/ (winusb.c, hid.c, cdc.c)        |
|   - 引入硬件背压流控 (Backpressure)                            |
|   - 消除栈逃逸，缓冲区生命周期绑定至设备实例                      |
|   - 完善断开/复位唤醒广播机制                                  |
+-------------------------------------------------------------+
|                     USB Device Core                         |
|   drivers/usb/device/core/ (core.c, usbdevice.c)            |
|   - EP0 状态机原子同步与在途事务冲刷                          |
|   - 规范 USB 2.0 ZLP 判定 (比对 host wLength)                |
|   - 严格边界校验与描述符动态装配 (移除 256B 固定数组)           |
+-------------------------------------------------------------+
|                 USB Common Specification Layer              |
|   include/drivers/usb/usb_common.h                          |
|   - 纯净的 USB 2.0 / Chapter 9 标准定义 (Device 与 Host 共享) |
|   - 标准描述符头、配置描述符 (严格 9 字节)、请求码、PID 等       |
+-------------------------------------------------------------+
|                 Hardware Controller Drivers                 |
|   - DCD (Device Controller Driver)                          |
|     * STM32F4 OTG FS (DWC2): 安全整字/尾字节 FIFO 搬移        |
|     * STM32F1 FSDEV: 安全半字/奇数字节 PMA 搬移                |
|   - 端点硬件资源隔离 (F401 1.25KB 限额，F1 PMA 避开 BDT 表)   |
+-------------------------------------------------------------+
```

---

## 3. 详细设计与缺陷根治方案

### 3.1 公共层重构 (`usb_common.h` 规范化)
1. **剥离私有结构**：
   - 将 `uep0_stage_t`、`func_callback` 移至 `include/drivers/usb/usb_device.h`；
   - 将内嵌 `struct list_head` 的 `struct usb_os_function_comp_id_descriptor` 移出公共规范层；
   - 移除 `hid_report`、`ustorage_cbw` / `csw` 等特定类结构，归位到各自类头文件；
   - 清除 `RT_TICK_PER_SECOND`，规范使用 NOS 标准内核时钟原语 `msec_to_tick(...)`。
2. **规范配置描述符结构体**：
   - `struct uconfig_descriptor` 剔除末尾的 `uint8_t data[256];`，严格固定为 USB 2.0 标准的 9 字节长度；
   - 描述符拼接缓冲区在设备创建时统一管理，并在追加接口和端点时增加严格的缓冲区边界校验。

### 3.2 控制请求边界检查与判空安全
1. **WinUSB 控制命令缓冲区截断**：
   - 在 `winusb.c` 的 `_ep0_cmd_read` 中，比较主机下发的 `setup->wLength` 与 `winusb_device->cmd_buff`（512 字节）：
     - 若 `wLength > sizeof(cmd_buff)`，直接发送 STALL 并返回 `-EINVAL`，阻断堆溢出；
     - 否则读取 `wLength` 字节进缓冲区。
   - 移除静态全局变量 `cmd_func`，将上下文记录在设备结构体内。
2. **描述符发送长度钳位**：
   - 在 `core.c` 的 `_vendor_request` 与 `usbdevice.c` 的 `usbd_os_proerty_descriptor_send` 中，发送长度必须执行 `min_t(size_t, setup->wLength, actual_desc_len)` 钳位，禁止直接透传未校验的 `setup->wLength`，杜绝堆越界读取。
3. **接口查询与设置判空**：
   - 在 `_get_interface`、`_set_interface` 中增加检查：若 `usbd_find_interface` 返回 `NULL` 或其 `curr_setting == NULL`，立即回复 `usbd_ep0_set_stall(device)` 并退出，杜绝空指针解引用。

### 3.3 消除异步发送引用栈内存 (Stack-Use-After-Return)
1. **字符串描述符缓冲区持久化**：
   - 废除 `_get_string_descriptor` 内部的局部栈变量 `struct ustring_descriptor str_desc;`；
   - 在 `struct udevice` 中开辟生命周期与设备实例绑定的 `uint8_t ep0_str_buf[128];`；
   - 字符串转换为 Unicode 后直接写入该持久缓冲区，再提交 `usbd_ep0_write`，确保后续硬件中断/DMA 异步读取时内存始终合法。
2. **HID 上报缓冲区持久化**：
   - 在 `struct hid_s` 中定义常驻发送缓冲区 `struct hid_report tx_report;`；
   - `_hid_write` 将数据填入 `tx_report` 后提交底层端点，彻底根除局部变量栈逃逸。

### 3.4 底层硬件驱动尾包读写安全与端点资源规划
1. **STM32F4 OTG FS (DWC2) 尾包复制重构**：
   - 在 `USB_WritePacket` 中：前 `len / 4` 个字按 32 位正常写入；剩余 `len % 4`（1~3 字节）使用局部临时变量配合 `memcpy(&tail_word, src, len % 4)`，只将有效字节读入，再写入 `DFIFO`，杜绝源缓冲区越界读；
   - 在 `USB_ReadPacket` 中：前 `len / 4` 个字按 32 位正常读取；剩余 `len % 4` 字节从 `DFIFO` 读取一个 32 位字后，使用 `memcpy(dest, &tail_word, len % 4)` 写入目标缓冲区，杜绝踩踏后方内存。
2. **STM32F401 硬件 FIFO 预算控制**：
   - 严格遵守 F401 OTG FS 专用 SRAM **1.25 KB (320 words = 0x140)** 硬件上限：
     - `RxFiFo = 0x80` (128 words, 512B)
     - `TxFiFo 0 = 0x20` (32 words, 128B, EP0)
     - `TxFiFo 1 = 0x40` (64 words, 256B, EP1 Bulk IN)
     - `TxFiFo 2 = 0x20` (32 words, 128B, EP2 Interrupt IN)
     - `TxFiFo 3 = 0x40` (64 words, 256B, EP3 备用)
     - 合计 `0x80 + 0x20 + 0x40 + 0x20 + 0x40 = 0x140 (320 words)`，彻底消除超界隐患。
3. **STM32F1 PMA 读写与 BDT 表隔离**：
   - 修复 `USB_WritePMA` / `USB_ReadPMA`：对奇数字节执行单字节读写保护，禁止 16 位越界踩踏；
   - 将板级 PMA 缓冲区起始地址统一调整至 `0x40` 之后（如 EP0 OUT 从 `0x40` 开始），彻底解除与 `0x00 ~ 0x40` 8 端点 BDT 描述符表区的重叠。
4. **端点分配失败拦截**：
   - `usbd_ep_assign` 分配前先清除旧端点地址掩码；
   - 若端点池耗尽分配失败，`usbd_device_add_config` 立即返回 `-ENOSPC` 阻断加载，严禁空指针端点上线。

### 3.5 EP0 状态机事务边界与 ZLP 规范逻辑
1. **原子状态机与新 SETUP 中断复位**：
   - 显式划分控制传输状态：`STAGE_IDLE`、`STAGE_SETUP`、`STAGE_DIN`、`STAGE_DOUT`、`STAGE_STATUS_IN`、`STAGE_STATUS_OUT`、`STAGE_STALL`；
   - 新 SETUP 到达时，立即清除未完成旧传输的状态与缓冲区，防止在途状态污染新请求。
2. **规范化 ZLP 判定**：
   - 仅当传输字节为端点 MPS 的整数倍，**且累计发送总长度严格小于主机请求的 `setup->wLength`** 时，才加发 ZLP；若已达到 `wLength`，直接切入 STATUS 阶段。

### 3.6 复位、取消配置与断线唤醒
1. **完善 `SET_CONFIGURATION(0)`**：
   - 收到配置 0 时，遍历所有已注册 Function 调用 `FUNC_DISABLE`，关闭非控制端点，状态切回 `ADDRESS`。
2. **重复配置重置**：
   - 修复 `FUNC_ENABLE` 守卫，确保重复配置时端点依然能可靠重新挂载接收请求。
3. **断线与复位唤醒**：
   - 当 USB 拔出或复位触发 `_function_disable` 时，设置 `connected = false`，并通过 `nos_completion_broadcast` 唤醒所有阻塞在读写信号量上的任务，线程被唤醒后感知到断开立即返回 `-ENOTCONN`，彻底消除挂起死等。

### 3.7 Bulk OUT 硬件背压流控 (Flow Control)
1. **接收端判定**：
   - 在 `_ep_out_handler` 中，写入 `kfifo` 后检查剩余可用空间；
   - 若 `kfifo_avail >= EP_MAXPACKET`，继续挂载 `usbd_io_request` 预读；
   - 若 `kfifo_avail < EP_MAXPACKET`，**暂停挂载接收**，标记 `rx_paused = true`。此时底层端点自动向主机回复硬件 **NAK**，迫使主机暂停发送，实现硬件背压。
2. **消费端唤醒**：
   - 当上层调用 `read` 消费出足够空间（`>= EP_MAXPACKET`）且 `rx_paused == true` 时，重新调用 `usbd_io_request` 挂载接收并恢复传输，彻底消除静默丢包。

### 3.8 CDC 驱动完善
- 为 CDC 驱动补全标准的 `device_register`，命名为 `"vcom"`；
- 在用户向 `tx_fifo` 写入数据时显式触发 `nos_complete(&data->tx_ready)`，激活并保持发送线程的连续传输。

---

## 4. 验证与回归测试计划

### 4.1 编译与无告警验证
四个金标预设必须保证 100% 编译通过且无新增警告：
```bash
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
cmake --preset zj-v3 && cmake --build --preset zj-v3
cmake --preset wl-001 && cmake --build --preset wl-001
```

### 4.2 自动化回归验证
1. **宿主机测试**：
   - `python3 tests/run_host_regressions.py`
   - `python3 -m unittest discover -s tests -p 'test_*.py'`
   - 针对 ZLP 计算、尾包复制安全边界编写专门的单元测试。
2. **QEMU 固件冒烟测试**：
   - `python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf`

### 4.3 功能与协议测试
1. **WinUSB / NLK 吞吐测试**：
   - 持续高吞吐写入数据，验证 Bulk OUT 背压生效，不发生静默丢字节与通信挂死；
2. **HID 键盘测试**：
   - 验证连续按键上报无野栈破坏、无乱码；
3. **断开/复位测试**：
   - 在数据传输过程中模拟断开或复位，验证阻塞线程能立即返回错误退出，无任务悬挂。
