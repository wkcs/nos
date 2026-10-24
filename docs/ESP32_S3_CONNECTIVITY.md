# ESP32-S3：GATT、USB Host 类与自动 OTG

<!-- Copyright (C) 2026 NOS contributors -->

本次补齐的是受 NOS 管理的 GATT **服务端**、USB Host boot HID/MSC 桥接以及经典 ID/VBUS 自动角色切换。无线、HCI、USB 枚举和硬件传输继续使用原始 ESP-IDF/NimBLE/TinyUSB；不把 SDK 句柄暴露给业务代码。

## BLE GATT

`include/nos/driver/ble.h` 是平台无关的配置与状态接口，`servers/bluetooth/gatt.c` 管理值、订阅、MTU 和事件队列。`nos_devices/ble.c` 将其接到实际 NimBLE 服务表、ATT access、GAP callback 和通知 API。

- 最多4个应用服务、16个属性（特征与自定义描述符合计），支持16位/128位 UUID、244字节值；UUID 字节使用 Bluetooth 小端顺序。
- 支持读、写、无响应写、notify、indicate、自定义描述符。CCCD 由 NimBLE 生成，禁止重复声明；GAP/GATT 系统服务也由 host 提供。
- SDK 回调把远端写入复制到 NOS 值存储和有界事件队列。队列满时有响应写入失败且不修改值；无响应写可能丢弃，丢弃计数可查询。应用须及时消费事件。
- 支持广播启动/停止、连接/断开、MTU、订阅、发送完成和 host reset 事件。通知先检查订阅与 MTU；`actual` 表示 SDK 接受的数据量，确认结果通过 TX_COMPLETE 事件传递。
- 可配置 IO capability、SC、MITM、bonding；支持输入、显示、数字比较的配对事件与响应。NimBLE 原始 NVS bond store 持有 NOS 管理的 NVS 引用。无 IO 且要求 MITM 的配置被拒绝，不支持的 OOB 请求断开连接。
- 加密/认证属性的读写权限交由 NimBLE 执行，要求16字节密钥；自定义通知另外核对 SDK 当前连接的加密/认证状态。事件 `status` 是负 NOS 错误码。
- 事件和值操作使用短临界区；调用 SDK 前释放临界区。停止先等待 host/停止任务退出和 SDK deinit，最后释放服务表及值存储。无法确认清理的失败仍保留全部资源，要求重启。

板级代码把一个不可变、长期有效的 `nos_ble_profile` 指针放入 `nos_idf_device_config.ble_profile`。省略时提供一个 NOS vendor UUID 的 READ/WRITE/NOTIFY 特征；这不是已绑定 NLP 的声明。

```c
static const uint8_t initial_battery = 100;
static const struct nos_ble_uuid services[] = {{2, {0x0f, 0x18}}};
static const struct nos_ble_attribute attributes[] = {{
    .uuid = {2, {0x19, 0x2a}}, .id = 1,
    .flags = NOS_BLE_READ | NOS_BLE_NOTIFY, .max_length = 1,
    .initial_value = &initial_battery, .initial_length = 1,
}};
static const struct nos_ble_profile profile = {
    .name = "NOS battery", .services = services, .attributes = attributes,
    .service_count = 1, .attribute_count = 1,
};
static const struct nos_idf_device_config config = {
    .name = "board.ble", .kind = NOS_SERVICE_BLE, .ble_profile = &profile,
};
```

创建、启动后，通过 `NOS_IO_BLE_EVENT` 等待 READY，再提交 `NOS_IO_BLE_ADVERTISE`（`nos_ble_advertise`）。使用 `NOS_IO_BLE_VALUE_GET/SET` 读取/更新本地值，`NOS_IO_BLE_NOTIFY` 发送通知或 indication。`NOS_IO_BLE_SECURE` 请求连接加密/配对；PAIRING 事件的 `value[0]` 是 NOS 配对动作，`value[1..4]` 是比较数字的小端编码，输入/显示动作这四字节为零；应用通过 `NOS_IO_BLE_PAIRING_RESPONSE` 响应。

连接句柄只在 CONNECTED 与 DISCONNECTED/RESET 之间有效。事件丢失后应用必须重新建立会话，不能继续使用旧连接状态。本接口没有实现 central 扫描/GATT client、OOB 或完整 BLE HID/NLP 业务绑定。

## USB Host

新增的 `nos_usb_transport` 使用已枚举的设备和完整 control/bulk/interrupt 传输契约。原 NOS DWC2 Host 与 IDF Host 都接到相同的 HID/BOT 类代码；不在 IDF 之上重复执行 SET_ADDRESS、配置选择或 DATA toggle 管理。

1. 创建并启动 `NOS_SERVICE_USB_HOST`，在服务循环轮询 `NOS_IO_POLL`。
2. 用 `nos_usb_attach` 提交 `NOS_IO_USB_ATTACH`；address=0 使用发现的根设备。返回单调递增的 generation。
3. `NOS_IO_USB_INFO` 返回复制后的 VID/PID、接口/端点及 generation，不返回 SDK 指针。
4. 用 `nos_usb_class_open` 提交 `NOS_IO_USB_HID_OPEN` 或 `NOS_IO_USB_MSC_OPEN`。
5. HID 使用 `nos_usb_report` / `NOS_IO_USB_HID_READ`，键盘 LED 使用 `NOS_IO_USB_HID_LEDS`。MSC 使用带 generation 的 `nos_usb_storage_io` / READ、WRITE，COMMIT 执行 SYNCHRONIZE CACHE，RECOVER 执行 BOT reset/clear-halt。
6. `NOS_IO_USB_DETACH` 先退休传输、释放接口，再关闭设备。服务停止后取消注册 client、释放 SDK URB、停用 Host 和 PHY。

当前支持 boot 键盘/鼠标和 SCSI BOT LUN0。沿用原有32位 LBA、512–4096字节块、最多4096字节请求限制；不支持 hub/TT、UAS、等时传输、任意 HID report parser 或文件系统自动挂载。

传输始终使用 SDK 分配的后端 bounce buffer；IN 请求按端点包长取整，但复制回调用者前检查实际长度。确认取消的空闲 HID 轮询返回 EAGAIN，允许下一次读取；不把正常 NAK/无报告当成设备损坏。MSC 超时禁止重放写入，要求显式 BOT reset。EP0 无公开 endpoint cancel API，因此通过 SDK root-port shutdown 退休控制传输；此后关闭/重建 Host 会话。不能确认退休时保留 URB、设备、接口及控制器租约，禁止销毁或交给 Device 栈。SDK 底层同步 halt/stop 仍依赖硬件 IRQ；这不是已证明所有物理故障都能有界恢复的承诺。

## 自动 OTG

`NOS_SERVICE_USB_OTG` 独占与固定 Host/Device 相同的控制器、GPIO19/20 及板级信号引脚。一个 NOS 任务自动采样、去抖并驱动现有 `nos_usb_otg` 策略；当前100Hz配置的轮询粒度至少10ms。

BSP 必须提供 `nos_idf_usb_otg_config`，明确 ID、VBUS sense、受保护且默认关闭的外部 VBUS switch、可选过流输入和极性。引脚不能与 PHY/控制台/Flash/其他设备冲突。没有已知板级接线时不会自动猜测或供电。

- `NOS_IO_USB_ROLE` 请求 NONE/DEVICE/HOST/AUTO；AUTO 根据去抖后的 ID/VBUS 自动切换。
- 原始外部 VBUS 在去抖完成前也会禁止供电；过流、VBUS 消失和 ID 撤销 Host 授权立即进入保守处理，输入上拉/下拉按配置极性选择非活动状态。
- 先停旧栈、关闭供电、等待放电，再启动新栈。Host 要经过供电授权和 power-good；始终只有一个栈拥有 PHY。
- 角色操作失败保留资源；RECOVER 在确认旧角色停止和供电关闭后重新等待放电，不自动重启角色。
- `NOS_IO_USB_OTG_STATUS` 查询请求/活动角色、阶段、generation、输入和错误。处于 ACTIVE 时其他 USB 类请求转发到当前角色。

这是经典 ID/VBUS OTG 策略，不是 USB-PD/Type-C CC、HNP 或 SRP。板级过流保护不能仅依赖串行服务域的轮询；保护开关必须在硬件上默认关闭并能独立限流。

## 官方模拟器与验证方法

```sh
python3 scripts/esp32s3.py setup
python3 tests/esp32s3.py
# 可单独执行宿主协议/SDK故障模型
python3 tests/host_esp32s3_protocols.py
python3 tests/host_esp32s3_usb.py
python3 tests/host_esp32s3_wireless.py
```

setup 安装/校验 Espressif 官方 `esp-develop-9.2.2-20260417` 的 Xtensa QEMU。专项再次核对下载包 SHA-256、实际二进制与包内文件完全相同，记录 `esp32s3` 的 QEMU device tree、命令、源码身份和镜像哈希。

模拟器默认空白 eFuse 会让 SDK 进入没有 ADC 模型支持的自校准。完整生态运行专项给虚拟 eFuse 的 BLK2 bit128 设置版本1，并用零差值表示 SDK 定义的参考出厂校准值；没有改 SDK、跳过构造函数或伪造 ADC 返回。普通固件/运行回归仍使用原空白校准 eFuse；完整生态镜像由此能够正常启动并运行相同检查及 OTG 管理任务专项。

官方 S3 machine **没有蓝牙射频和 USB OTG 控制器模型**。因此验证分为：实际 S3 CPU 上的 GATT/类协议/OTG 状态和 NOS 任务生命周期；实际 SDK 适配代码的 URB/板级信号故障模型；真实 SDK 全量链接。原有 NOS DWC2 类代码另用 stock QEMU raspi3b 的真实 USB 键盘、鼠标和存储模型验证，该项是宿主 C HCD 经 qtest 访问控制器，不是 S3 USB 枚举或 NOS Raspberry Pi 启动。

射频收发、GATT 配对互通、S3 USB 枚举与电气角色切换仍需要实板或补充对应硬件模型。SDK 和模拟器源码未打补丁，不能把软件状态测试称为无线/USB 物理测试通过。
