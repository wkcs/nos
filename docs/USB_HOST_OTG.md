# USB host 与双角色框架

<!-- Copyright (C) 2026 NOS contributors -->

本实现新增 `include/nos/driver/usb_{host,dwc2,class,otg}.h` 和 `servers/usb/host/`。host 枚举、类协议、DMA 控制器和角色策略均在用户态，不增加 syscall 或绕过设备能力检查。原 device/HID/NLP 通路继续工作。

## 本轮能力

| 部分 | 已实现 | 当前边界 |
|---|---|---|
| Host core | root 连接检测/复位、地址分配、设备/配置描述符、SET_CONFIGURATION、拔插失效句柄 | 每控制器一个直连设备，选择第一个配置、alternate 0；多控制器可分别实例化 |
| 传输 | 控制 SETUP/data/status、批量、周期中断轮询、DATA toggle、短包、STALL/clear halt、整笔 timeout | 同步有界操作；无 ISO、异步 URB 或多队列调度 |
| DWC2 HCD | 内部 DMA 模式、一个 host channel、LS/FS/HS root、cache/barrier 回调、超时停止与隔离 | 无 PIO、hub/TT split 或高带宽多事务；单包最大 512 字节（小 FIFO 配置为 64） |
| HID | boot 键盘/鼠标、原始按键/相对移动报告、键盘锁灯 | 无任意 HID report descriptor 解析；不自动注入本地键盘/NLP |
| U 盘 | SCSI transparent + BOT、LUN0、就绪/REQUEST SENSE、容量、READ/WRITE(10)、SYNCHRONIZE CACHE | 不提供 UAS、文件系统自动挂载或完整 SCSI；LBA32、512–4096 字节块 |
| OTG role | ID/VBUS 自动选择、手动 NONE/device/host、停旧角色、放电等待、VBUS 上电确认、过流与失败恢复 | DRD 角色策略；无 HNP/SRP/ADP、USB-PD、Type-C CC/PD 协商 |

Hub 设备、ISO 配置、USB3/SuperSpeed 和超出资源预算的配置明确报错。RK3588 后续需要 DWC3/xHCI HCD；本轮没有把 DWC2 实现标成 xHCI 支持。

## Host 使用与生命周期

BSP 先注册/绑定一个 `nos_device`，授权 MMIO、DMA、时钟、PHY 和引脚。调用方零初始化并固定保存 `nos_usb_host`，填入 controller、HCD ops 和 `power_budget_ma`，再依次调用：

```c
int error = nos_usb_host_start(host);
uint32_t handle;
if (!error)
    error = nos_usb_host_enumerate(host, &handle);
/* 按 host->interfaces[] 选择 HID/MSC 或用公共 transfer/control 接口。 */
```

`start` 持有设备引用，直到 `stop` 确认控制器停止。运行中或 stop 失败时，设备不能被 unbind。一次失败的 start 也可能保留资源；若随后的 stop 失败，框架保持引用并进入 faulted 状态，只允许重试 stop。引用计数饱和时仍保留已经获得的引用，不依赖第二次引用申请。

所有操作由服务事件循环串行化；`controller.busy` 只检测重入，不代替线程锁。同步调用期间不能通过同一服务执行角色切换；其它线程必须先取得外层锁。DMA buffer 和对象不得在 stop 成功前移动/释放。

枚举先等待 100 ms 稳定连接，port reset 由 HCD 保持至少 50 ms 并等待 10 ms 恢复；SET_ADDRESS 的 status 阶段结束后才更新地址，并等待 2 ms。配置最多 512 字节、8 个 interface、16 个 active endpoint；只启用 alternate 0。解析先验证再提交元数据，拒绝截断、零长描述符、重复端点/接口、非法 max packet/interval、无对应 alternate 0 和超出电流声明预算的配置。`bMaxPower` 检查不等于实测供电能力，BSP 还必须限制实际电流。

设备 handle 是非零 generation。重新枚举递增，stop、断开、连接变化或过流使当前配置失效；旧 handle 返回 ENODEV。generation 到 UINT32_MAX 后拒绝继续分配，不回绕复用。每次 packet 前复查连接状态，服务也应周期调用 `nos_usb_host_poll()`；HCD 必须提供并确认 connection-change，才能识别两次调用之间拔出又插入的设备。

控制传输自动生成 SETUP/DATA1/status 和后续 toggle。公共 control 拒绝直接改变地址、配置、alternate 或 endpoint halt/toggle 的标准请求；这些状态由 core 管理，class/vendor 控制请求可以使用。USB3 和 hub/TT 暂不进入该地址域。

批量传输遇到 NAK 在同一总 deadline 内重试；中断传输一次 token 后返回，NAK 保持 toggle，并根据 endpoint interval 安排下一次轮询。已 ACK 包才翻转 toggle。成功短 IN 包结束传输。错误和已经完成的字节数分别返回，调用方不能自动重发可能已执行的 OUT 数据。STALL 锁存到 endpoint，`nos_usb_host_clear_halt()` 完成 CLEAR_FEATURE 后复位 DATA0。

## DWC2 平台后端

`nos_usb_dwc2_io` 接收相对寄存器偏移的 read32/write32、单调回绕微秒 time/delay，以及 dma_map/unmap/sync。后端不将客户端物理地址写入寄存器；BSP 必须经已有能力/映射机制，授权一个受限的 32 位 DMA bus 地址。

驱动识别 Synopsys ID、host 能力、内部 DMA 架构和 FIFO 深度。深度至少 608 words 时分配 RX 288、non-periodic TX 160、periodic TX 160 words，单包最大 512 字节；96–607 words 分配 48/24/24，单包最大 64 字节。RX 包含单通道状态和余量；1024 字节高带宽 interrupt/ISO 不宣称可用。只支持 PIO 的控制器返回 ENOTSUP，不能据此启用 STM32F401 OTG FS host。

BSP 在 start 前配置 PHY。`phy_clock_hz` 可明确指定 30/48/60 MHz，0 按 PHYSEL 推断内部 FS 48 MHz 或 HS 60 MHz；30 MHz UTMI 必须显式填写。HCD 按 root speed 配置 HCFG/HFIR；并不替代平台 USB PLL、PHY reset 或外部 ULPI 初始化。

每次 DMA 使用自身 1024 字节、64 字节对齐的 bounce 区域，映射在会话内保留。IN 缓冲覆盖完整 packet 空间，收到超过调用方长度的数据返回错误，不复制越界。`dma_sync(for_device)` 和 `dma_sync(for_cpu)` 必须提供目标架构需要的 cache 操作与内存屏障；coherent 平台也需显式回调。QEMU 桥接使用 qtest RAM 读写，这不是硬件 cache 一致性验证。

完成、NAK、STALL、错误或 deadline 后都确认 channel 停止；无法 halt 时 `faulted` 隔离，拒绝复用 bounce buffer。stop 只有确认 core reset/AHB idle 后才解除 DMA 映射；reset 失败继续保留对象、映射和设备引用，不能假装 DMA 已停止。循环同时有限次迭代和时间检查，时钟停滞也退出。

此后端不驱动外部 VBUS；`HPRT.PWR` 不足以给接口供电。电源开关、过流关断、接线和 Type-C source 授权都由平台 OTG/BSP 负责。

## HID 与存储

HID 驱动选择 boot interface，发送 SET_PROTOCOL(boot)，设置 IDLE，并使用 interrupt IN。键盘返回 8 字节报告；鼠标返回 3 字节 boot 报告。某些设备（含 QEMU）在 boot 模式仍发送第 4 个滚轮字节，驱动先安全接收再归一化，不将该字节写出三字节调用方缓冲区。锁灯使用 class SET_REPORT。

MSC 检查 interface class/subclass/protocol 和双向 bulk endpoint，使用 BOT CBW/CSW 签名、递增 tag、长度、residue/status 验证。初始 TEST UNIT READY/REQUEST SENSE 处理一次上电 unit attention；没有无限等待介质或自动重试写命令。只查询 LUN0，READ CAPACITY(10) 的超范围容量明确拒绝。每笔数据受 controller.max_transfer（最多 65536 字节）约束。

传输、CSW tag/签名或 phase 状态不可信时锁住 BOT 通道，调用方必须执行 `nos_usb_msc_recover()`（BOT reset、clear 两个 halt）后自行决定后续操作。失败的 WRITE 不自动重放；已经写入哪些字节可能不确定。sync 发送真实 SCSI flush 请求，但持久化还取决于设备实现、电源与介质。当前没有接入 VFS 自动挂载、USB 磁盘热拔出的文件系统恢复或板级 USB 更新启动链。

## OTG 角色和供电顺序

`nos_usb_otg` 初始化前，BSP 必须保证 VBUS source/pull-up 关闭、控制器没有遗留 DMA。它是独立策略对象，平台提供 stop/start(role) 与真实外部 `set_vbus`，并提交经过消抖的 ID、VBUS、source_allowed、overcurrent。角色 stop 应连接 `nos_usb_host_stop()` 或相应 gadget 停止/断开函数；start 应连接 host start 或 gadget 初始化。成功 stop 必须已经取消传输、清旧 completion、停止 DMA/IRQ、去掉 device pull-up；失败保留资源等待重试。

- AUTO：有效且接地的 ID 请求 host；否则有 VBUS 请求 device；否则 NONE。USB-C 端口应由 TCPC/PD 层决定请求和 source_allowed，不能用这个策略取代 CC/PD。
- manual host 仍要求 source_allowed；发现已有外部 VBUS 时等待，不反向供电。
- 切换先 stop 旧角色，再关闭自己供电，等待默认 100 ms 放电；host 再开电，等默认 100 ms 内 VBUS 有效后 start。update 返回 EAGAIN 时服务继续轮询；等待不阻塞事件循环。
- 过流、VBUS 掉电、source 授权撤销、start/stop/供电失败进入锁存 FAULT；立即尝试停角色和关闭自身 VBUS。控制器 stop 失败时不启动另一个角色。
- `recover()` 显式重试停止与关电，成功后进入放电等待，request 重置为 NONE；调用方排除外部故障后重新请求。generation 饱和不复用旧会话。

硬件必须具有独立过流保护。软件轮询不能替代短路/过流开关的及时关断。所有时间是 wrapping uint32 毫秒；服务应连续轮询，不能跨越计数周期后继续使用过期输入。

**NK60 保持 device-only**：已知 PA9 用于 UART TX，现有代码无 VBUS sense，板载 source 开关、ID/Type-C CC 接法也没有确认。不新增源供电 syscall、不扩展 MMIO 白名单、不强行把键盘接口切到 host。ESP32/RK3588 接入时须先确认具体芯片、PHY、供电和角色硬件，再填写能力。

## 验证方式

```sh
source /workspace/.nos-env/activate.sh
python3 tests/host_usb_host.py
python3 tests/qemu_usb_host.py
python3 tests/acceptance.py --section boards --section baseline --section rv32 --require-frozen-source
```

宿主 sanitizer 测试运行实际 core/HID/BOT/DWC2/OTG C 代码，覆盖 50000 个恶意配置、控制 PID、短包/部分成功、NAK 停钟退出、坏 CSW/写不重放、stale handle、DMA halt/reset 失败隔离、引用饱和、ID/VBUS/放电/过流/失败恢复。

`qemu_usb_host.py` 将实际 C HCD 的 MMIO/DMA 接到原版 QEMU raspi3b 的 DWC2，显式直连 root port 1，用四个 profile 分别枚举真实 QEMU USB keyboard（FS/HS）、mouse、storage；用 QMP 注入按键/移动、热拔插，用临时 raw 磁盘验证容量、读、写、sync 和退出后的扇区内容。C 进程正常退出执行 ASan/UBSan/LSan。qtest 中 ARM64 只运行两条 WFI/循环指令的测试载荷；**这不是 NOS 已支持 Raspberry Pi 启动**。物理 OTG、供电、电气时序和真实硬件 DMA/cache 仍未验证。

原有 ARM、ARM64、RV64、STM32 和 RV32 来宾另运行实际 descriptor/OTG 代码，以必需标记 `usb_host_descriptor_and_otg_userspace_roles` 验证目标编译与用户态执行；它也不是实板 HCD 验证。NK60 双 MPU 固件继续检查 128 KiB Flash 和 16 KiB 用户静态区限制。
