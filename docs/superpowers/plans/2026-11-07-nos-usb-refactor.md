# NOS USB 协议栈重构与安全加固实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 重构 NOS USB 协议栈核心与类驱动，修复 7 大安全与时序缺陷（控制请求越界、异步栈逃逸、尾包内存踩踏、EP0 状态机与 ZLP 异常、配置收尾不严、Bulk OUT 背压缺失、端点资源超界），规范化公共 Chapter 9 协议层，为后续 USB Host 扩展奠定基础。

**Architecture:** 
1. 净化 `include/drivers/usb/usb_common.h`，仅保留纯净的标准 USB 2.0 Chapter 9 常量与描述符（配置描述符严格 9 字节），私有结构移至 `usb_device.h` 或类头文件；
2. 重写 STM32F4 与 F1 底层驱动的尾包复制逻辑（整字/半字后尾部用安全字节搬移），规整 F401 1.25KB FIFO 预算及 F1 PMA 偏移（0x40 开始避免与 BDT 表重叠）；
3. 规范 Device Core 控制传输状态机，新 SETUP 到达时冲刷在途状态，按照 USB 2.0 规范（比对 host `wLength`）重构 ZLP 逻辑，消除描述符组装越界与空指针解引用；
4. 类驱动层消除栈逃逸（字符串与 HID 报文绑定实例生命周期），在 WinUSB/CDC 引入硬件背压流控（FIFO 满时暂停下发端点接收产生硬件 NAK，读者消费后恢复），完善断开唤醒广播。

**Tech Stack:** C99/GNU11, NOS 微内核 IPC/调度原语 (`nos_completion`, `nos_msgq`, `kfifo`), STM32 USB HAL/LL, CMake/Ninja.

## Global Constraints
- 遵循 NOS 代码规范：4 个空格缩进，K&R 大括号，`snake_case` 命名；
- 注释必须使用中文；
- 每次修改后确保四大板级预设（`nk60-v2`、`qemu-stm32`、`zj-v3`、`wl-001`）编译零错误零新增警告，且主机回归测试全绿；
- 严禁引入任何阻断编译的破坏性变动。

---

### Task 1: 净化与规范化 `include/drivers/usb/usb_common.h`

**Files:**
- Modify: `include/drivers/usb/usb_common.h:1-200`
- Modify: `include/drivers/usb/usb_device.h:1-60`
- Modify: `drivers/usb/device/class/hid.h:1-50`
- Modify: `drivers/usb/device/core/core.c:1-50`

**Interfaces:**
- Consumes: Standard USB 2.0 Chapter 9 definitions.
- Produces: Clean, host-independent `struct usb_config_descriptor` (strictly 9 bytes, without `uint8_t data[256]`), portable descriptor types and standard request definitions.

- [ ] **Step 1: 检查现有 `usb_common.h` 中破坏标准 9 字节的定义与私有结构**

确认 `struct uconfig_descriptor` 中的 `data[256]`，以及混杂的 `uep0_stage_t`、`func_callback`、`struct usb_os_function_comp_id_descriptor` 等。

- [ ] **Step 2: 重构 `usb_common.h` 并迁移从机私有结构到 `usb_device.h`**

在 `include/drivers/usb/usb_common.h` 中：
1. 将 `struct uconfig_descriptor` 的定义修正为标准 9 字节：
```c
struct uconfig_descriptor
{
    uint8_t bLength;
    uint8_t type;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t MaxPower;
};
typedef struct uconfig_descriptor* ucfg_desc_t;
```
2. 移除 `uep0_stage_t`、`func_callback`、`udevice_state_t`、`struct hid_report`、`struct ustorage_cbw`、`struct ustorage_csw`；
3. 将上述私有结构迁移至 `include/drivers/usb/usb_device.h`（对于设备状态机）及各自类驱动头文件中；
4. 替换 `USB_TIMEOUT_BASIC` 中残留的 `RT_TICK_PER_SECOND` 为 `msec_to_tick(1000)`。

- [ ] **Step 3: 适配 `core.c` 对配置描述符缓冲区的访问**

在 `core.c` 中：将原先假定 `cfg->cfg_desc.data[...]` 的拼接逻辑修改为在 `struct uconfig` 结构体中分配专用描述符缓冲区（如 `uint8_t raw_desc[512];`）或动态分配缓冲区，增加长度越界检查。

- [ ] **Step 4: 编译验证四大预设**

Run:
```bash
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
cmake --preset zj-v3 && cmake --build --preset zj-v3
cmake --preset wl-001 && cmake --build --preset wl-001
```
Expected: 全部构建通过，无新增编译器警告。

- [ ] **Step 5: 提交更改**

```bash
git add include/drivers/usb/ drivers/usb/
git commit -m "USB: Clean up usb_common.h and standardize config descriptor"
```

---

### Task 2: 底层硬件驱动尾包复制越界修复与端点资源规划 (STM32F4 & STM32F1)

**Files:**
- Modify: `board/nk60-v2/usb/stm32f4xx_ll_usb.c:790-840`
- Modify: `board/nk60-v2/arch_usb.c:230-245`
- Modify: `board/qemu-stm32/usb/stm32f1xx_ll_usb.c:590-635`
- Modify: `board/qemu-stm32/arch_usb.c:215-230`
- Modify: `board/wl-001/usb/stm32f1xx_ll_usb.c:590-635`
- Modify: `board/wl-001/arch_usb.c:215-230`
- Modify: `board/zj-v3/usb/stm32f1xx_ll_usb.c:590-635`
- Modify: `board/zj-v3/arch_usb.c:215-230`

**Interfaces:**
- Produces: Safe `USB_WritePacket` / `USB_ReadPacket` (STM32F4) avoiding OOB read/write; safe `USB_WritePMA` / `USB_ReadPMA` (STM32F1) protecting odd bytes; non-overlapping PMA allocation; F401 FIFO capped at 1.25KB.

- [ ] **Step 1: 修复 STM32F4 OTG FS `USB_WritePacket` 与 `USB_ReadPacket`**

在 `board/nk60-v2/usb/stm32f4xx_ll_usb.c` 中：
```c
HAL_StatusTypeDef USB_WritePacket(USB_OTG_GlobalTypeDef *USBx, uint8_t *src, uint8_t ch_ep_num, uint16_t len, uint8_t dma)
{
  uint32_t words = 0U, rem = 0U, i = 0U;

  if (dma == 0U)
  {
    words = len / 4U;
    rem = len % 4U;
    for (i = 0U; i < words; i++, src += 4U)
    {
      USBx_DFIFO(ch_ep_num) = *((uint32_t *)src);
    }
    if (rem > 0U)
    {
      uint32_t tail_word = 0U;
      memcpy(&tail_word, src, rem);
      USBx_DFIFO(ch_ep_num) = tail_word;
    }
  }
  return HAL_OK;
}

void *USB_ReadPacket(USB_OTG_GlobalTypeDef *USBx, uint8_t *dest, uint16_t len)
{
  uint32_t words = len / 4U;
  uint32_t rem = len % 4U;
  uint32_t i = 0U;

  for (i = 0U; i < words; i++, dest += 4U)
  {
    *(uint32_t *)dest = USBx_DFIFO(0U);
  }
  if (rem > 0U)
  {
    uint32_t tail_word = USBx_DFIFO(0U);
    memcpy(dest, &tail_word, rem);
    dest += rem;
  }
  return ((void *)dest);
}
```

- [ ] **Step 2: 规整 STM32F401 硬件 FIFO 预算 (上限 1.25KB / 320 字)**

在 `board/nk60-v2/arch_usb.c` 的 `_init` 中：
```c
HAL_PCDEx_SetRxFiFo(pcd, 0x80);    /* 128 words = 512B */
HAL_PCDEx_SetTxFiFo(pcd, 0, 0x20); /* 32 words = 128B (EP0) */
HAL_PCDEx_SetTxFiFo(pcd, 1, 0x40); /* 64 words = 256B (EP1 Bulk IN) */
HAL_PCDEx_SetTxFiFo(pcd, 2, 0x20); /* 32 words = 128B (EP2 Interrupt IN) */
HAL_PCDEx_SetTxFiFo(pcd, 3, 0x40); /* 64 words = 256B (EP3 Bulk IN 备用) */
/* 0x80 + 0x20 + 0x40 + 0x20 + 0x40 = 0x140 = 320 words，严格不超出 1.25KB 物理边界 */
```

- [ ] **Step 3: 修复 STM32F1 PMA 奇数字节越界与 BDT 重叠**

1. 修改 `stm32f1xx_ll_usb.c` 中的 `USB_WritePMA` 与 `USB_ReadPMA`：
```c
void USB_WritePMA(USB_TypeDef *USBx, uint8_t *pbUsrBuf, uint16_t wPMABufAddr, uint16_t wNBytes)
{
  uint32_t halfwords = wNBytes >> 1;
  uint32_t index = 0, temp1 = 0, temp2 = 0;
  uint16_t *pdwVal = (uint16_t *)(wPMABufAddr * 2 + (uint32_t)USBx + 0x400);

  for (index = halfwords; index != 0; index--)
  {
    temp1 = (uint16_t)*pbUsrBuf++;
    temp2 = temp1 | ((uint16_t)*pbUsrBuf++ << 8);
    *pdwVal = temp2;
    pdwVal += 2;
  }
  if (wNBytes & 1)
  {
    *pdwVal = (uint16_t)*pbUsrBuf;
  }
}

void USB_ReadPMA(USB_TypeDef *USBx, uint8_t *pbUsrBuf, uint16_t wPMABufAddr, uint16_t wNBytes)
{
  uint32_t halfwords = wNBytes >> 1;
  uint32_t index = 0;
  uint32_t *pdwVal = (uint32_t *)(wPMABufAddr * 2 + (uint32_t)USBx + 0x400);

  for (index = halfwords; index != 0; index--)
  {
    uint32_t val = *pdwVal++;
    *pbUsrBuf++ = (uint8_t)(val & 0xFF);
    *pbUsrBuf++ = (uint8_t)((val >> 8) & 0xFF);
  }
  if (wNBytes & 1)
  {
    *pbUsrBuf = (uint8_t)(*pdwVal & 0xFF);
  }
}
```
2. 在 `board/qemu-stm32/arch_usb.c`、`board/wl-001/arch_usb.c`、`board/zj-v3/arch_usb.c` 中，将 PMA 起始偏移从 `0x18` 调整为 `0x40` 之后（避开 0x00..0x40 BDT 表区）：
```c
HAL_PCDEx_PMAConfig(pcd, 0x00, PCD_SNG_BUF, 0x40);
HAL_PCDEx_PMAConfig(pcd, 0x80, PCD_SNG_BUF, 0x80);
HAL_PCDEx_PMAConfig(pcd, 0x81, PCD_SNG_BUF, 0xC0);
HAL_PCDEx_PMAConfig(pcd, 0x01, PCD_SNG_BUF, 0x100);
HAL_PCDEx_PMAConfig(pcd, 0x82, PCD_SNG_BUF, 0x140);
HAL_PCDEx_PMAConfig(pcd, 0x02, PCD_SNG_BUF, 0x180);
HAL_PCDEx_PMAConfig(pcd, 0x83, PCD_SNG_BUF, 0x1C0);
```

- [ ] **Step 4: 编译并验证冒烟测试**

Run:
```bash
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32 && python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
```
Expected: 编译通过且 QEMU 启动冒烟测试通过。

- [ ] **Step 5: 提交更改**

```bash
git add board/
git commit -m "USB: Fix tail-packet OOB copy in F4/F1 and rebalance FIFO/PMA layouts"
```

---

### Task 3: Device Core 控制传输状态机、ZLP 规范与边界安全加固

**Files:**
- Modify: `drivers/usb/device/core/core.c`
- Modify: `drivers/usb/device/core/usbdevice.c`
- Modify: `include/drivers/usb/usb_device.h`

**Interfaces:**
- Consumes: Clean `usb_common.h` standard descriptors.
- Produces: Safe control transfer handling, no stack escaping string descriptors, USB 2.0 compliant ZLP logic, safe de-configuration (`SET_CONFIG(0)`), endpoint assign validation.

- [ ] **Step 1: 消除字符串描述符异步栈逃逸**

在 `include/drivers/usb/usb_device.h` 的 `struct udevice` 中加入：
```c
uint8_t str_desc_buf[128];
```
在 `core.c` 的 `_get_string_descriptor` 中：移除 `struct ustring_descriptor str_desc;`，改为使用 `device->str_desc_buf` 格式化并传给 `usbd_ep0_write`。

- [ ] **Step 2: 修复控制请求边界与判空**

1. 在 `_get_interface` 和 `_set_interface` 中增加判空防护：
```c
intf = usbd_find_interface(device, setup->wIndex & 0xFF, NULL);
if (intf == NULL || intf->curr_setting == NULL) {
    usbd_ep0_set_stall(device);
    return -EINVAL;
}
```
2. 在 `_vendor_request` 与 `usbd_os_proerty_descriptor_send` 中，加入：
```c
size_t send_len = min_t(size_t, setup->wLength, actual_len);
usbd_ep0_write(device, data, send_len);
```

- [ ] **Step 3: 重构 EP0 状态机与 ZLP 判定**

在 `include/drivers/usb/usb_device.h` 的 `struct uendpoint` 中记录原始请求长度 `size_t req_wlength;`。
在 `core.c` 中：
1. `usbd_ep0_setup_handler` 收到新 SETUP 时，先中止并清理旧传输：
```c
dcd->ep0.request.remain_size = 0;
dcd->ep0.request.size = 0;
dcd->ep0.request.buffer = NULL;
dcd->ep0.request.req_length = setup->wLength;
```
2. `usbd_ep0_in_handler` 中更正 ZLP 逻辑：
```c
if (remain == 0)
{
    /* 仅当发送长度是 MPS 整数倍且严格小于主机要求的 wLength 时才发 ZLP */
    if ((dcd->ep0.request.size > 0) &&
        (dcd->ep0.request.size < dcd->ep0.request.req_length) &&
        ((dcd->ep0.request.size % mps) == 0))
    {
        dcd->ep0.request.req_length = 0;
        dcd_ep_write(dcd, EP0_IN_ADDR, NULL, 0);
    }
    else
    {
        dcd->stage = STAGE_STATUS_OUT;
        dcd_ep_read_prepare(dcd, EP0_OUT_ADDR, NULL, 0);
    }
}
```

- [ ] **Step 4: 修复配置生命周期与端点分配拦截**

1. 在 `_set_config` 中，当 `setup->wValue == 0` 时：
```c
if (setup->wValue == 0)
{
    if (device->curr_cfg != NULL) {
        for (i = device->curr_cfg->func_list.next; i != &device->curr_cfg->func_list; i = i->next) {
            func = (ufunction_t)list_entry(i, struct ufunction, list);
            FUNC_DISABLE(func);
        }
    }
    device->curr_cfg = NULL;
    device->state = USB_STATE_ADDRESS;
    goto _exit;
}
```
2. 在 `usbd_device_add_config` 中，检查端点分配返回值：
```c
if (usbd_ep_assign(device, ep) != 0) {
    pr_err("endpoint assign error for ep %p\n", ep);
    return -ENOSPC;
}
```
3. 在 `usbd_ep_assign` 中，先清除端点号再赋值：
```c
EP_ADDRESS(ep) = (EP_ADDRESS(ep) & ~0x0FU) | (device->dcd->ep_pool[i].addr & 0x0FU);
```

- [ ] **Step 5: 编译测试与主机回归**

Run:
```bash
python3 tests/run_host_regressions.py
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
```
Expected: 回归全通过，固件编译零错误。

- [ ] **Step 6: 提交更改**

```bash
git add drivers/usb/device/core/ include/drivers/usb/
git commit -m "USB: Refactor device core EP0 state machine, ZLP logic, and bounds validation"
```

---

### Task 4: 类驱动 (WinUSB / HID / CDC) 安全加固与 Bulk OUT 背压流控

**Files:**
- Modify: `drivers/usb/device/class/winusb.c`
- Modify: `drivers/usb/device/class/hid.c`
- Modify: `drivers/usb/device/class/cdc.c`

**Interfaces:**
- Produces: Safe WinUSB command buffer parsing, disconnection broadcast wake-up, persistent HID report buffer, hardware backpressure in Bulk OUT for WinUSB and CDC.

- [ ] **Step 1: WinUSB 控制缓冲截断与断开唤醒**

在 `winusb.c` 中：
1. 移除全局 `static ufunction_t cmd_func;`，将其移入 `struct winusb_device`；
2. 在 `_ep0_cmd_read` 中检查 `setup->wLength`：
```c
if (setup->wLength > sizeof(winusb_device->cmd_buff)) {
    usbd_ep0_set_stall(func->device);
    return -EINVAL;
}
```
3. 增加 `bool connected;` 字段；在 `_function_disable` 中设置 `connected = false` 并调用 `nos_completion_broadcast(&win_dev->write_sem)` 与 `nos_completion_broadcast(&win_dev->read_sem)` 唤醒阻塞任务。读写接口检测到 `!connected` 返回 `-ENOTCONN`。

- [ ] **Step 2: WinUSB Bulk OUT 背压流控实现**

在 `winusb.c` 中：
1. 结构体增加 `bool rx_paused;`；
2. 在 `_ep_out_handler` 中：
```c
if (size > 0) {
    kfifo_in(&winusb_device->rx_fifo, winusb_device->ep_rx_buf, (unsigned int)size);
    nos_complete(&winusb_device->read_sem);
}

if (kfifo_avail(&winusb_device->rx_fifo) >= EP_MAXPACKET(winusb_device->ep_out)) {
    winusb_device->ep_out->buffer = winusb_device->ep_rx_buf;
    winusb_device->ep_out->request.buffer = winusb_device->ep_rx_buf;
    winusb_device->ep_out->request.size = EP_MAXPACKET(winusb_device->ep_out);
    winusb_device->ep_out->request.req_type = UIO_REQUEST_READ_BEST;
    winusb_device->rx_paused = false;
    usbd_io_request(func->device, winusb_device->ep_out, &winusb_device->ep_out->request);
} else {
    /* FIFO 空间不足，暂停挂载接收，底层硬件对主机自动回复 NAK 形成背压 */
    winusb_device->rx_paused = true;
}
```
3. 在 `winusb_read_timeout` 消费数据后恢复挂载：
```c
read_size = kfifo_out(&win_dev->rx_fifo, buffer, (unsigned int)size);
if (win_dev->rx_paused && kfifo_avail(&win_dev->rx_fifo) >= EP_MAXPACKET(win_dev->ep_out)) {
    win_dev->rx_paused = false;
    win_dev->ep_out->buffer = win_dev->ep_rx_buf;
    win_dev->ep_out->request.buffer = win_dev->ep_rx_buf;
    win_dev->ep_out->request.size = EP_MAXPACKET(win_dev->ep_out);
    win_dev->ep_out->request.req_type = UIO_REQUEST_READ_BEST;
    usbd_io_request(win_dev->func->device, win_dev->ep_out, &win_dev->ep_out->request);
}
```

- [ ] **Step 3: 消除 HID 上报局部栈逃逸**

在 `drivers/usb/device/class/hid.c` 中：
1. 在 `struct hid_s` 中定义 `struct hid_report tx_report;`；
2. 修改 `_hid_write`，将数据填入 `hid_dev->tx_report`，不再使用局部变量 `struct hid_report report;`。

- [ ] **Step 4: CDC 驱动背压流控与设备注册补全**

在 `drivers/usb/device/class/cdc.c` 中：
1. 引入与 WinUSB 相同的 Bulk OUT 背压机制；
2. 注册 `"vcom"` 设备：`data->dev.name = "vcom"; device_register(&data->dev);`；
3. 在上层写数据触发时显式触发 `nos_complete(&data->tx_ready)` 唤醒 `vcom_tx_task_entry`。

- [ ] **Step 5: 全量构建与回归**

Run:
```bash
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
python3 tests/run_host_regressions.py
```
Expected: 编译通过且测试全部通过。

- [ ] **Step 6: 提交更改**

```bash
git add drivers/usb/device/class/
git commit -m "USB: Fix class driver stack escaping, add Bulk OUT backpressure, and harden WinUSB/CDC"
```

---

### Task 5: 单元测试补全与系统级全架构回归验证

**Files:**
- Create: `tests/host/test_usb_safety.c`
- Modify: `tests/run_host_regressions.py`

**Interfaces:**
- Tests ZLP logic bounds, tail-packet copy safety, and descriptor memory clamp.

- [ ] **Step 1: 编写 USB 安全逻辑宿主机单元测试**

在 `tests/host/test_usb_safety.c` 中编写测试：
1. 验证尾包读写算法：分别测试长度为 0, 1, 2, 3, 4, 5, 7, 8 字节时的缓冲区拷贝，确认不发生越界读或越界写；
2. 验证 ZLP 判定逻辑：比对 `remain == 0`, `size == MPS`, `size < wLength`，确认各种组合下的 ZLP 发送决策符合 USB 2.0 规范。

- [ ] **Step 2: 将新测试接入 `run_host_regressions.py`**

在 `tests/run_host_regressions.py` 中编译并执行 `test_usb_safety`。

- [ ] **Step 3: 运行完整宿主机回归与 QEMU 冒烟**

Run:
```bash
python3 tests/run_host_regressions.py
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```
Expected: 全部测试 PASS。

- [ ] **Step 4: 四大板级预设零告警构建验证**

Run:
```bash
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
cmake --preset zj-v3 && cmake --build --preset zj-v3
cmake --preset wl-001 && cmake --build --preset wl-001
```
Expected: 四个板级全部零错误、零警告编译成功。

- [ ] **Step 5: 提交更改**

```bash
git add tests/
git commit -m "test: add USB safety regression tests and verify all board presets"
```
