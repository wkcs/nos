# NuTiny-SDK-NUC472 开发板 NOS 完整支持设计文档

**日期**: 2026-11-14  
**状态**: 已确认设计，待审核并拆分实施计划  
**目标硬件**: Nuvoton NuTiny-SDK-NUC472（主控 `NUC472HI8AE`，Cortex-M4F，512 KiB APROM @ `0x00000000`，64 KiB SRAM @ `0x20000000`，142 路外部中断）

---

## 1. 背景、目标与约束

### 1.1 现状与不足
当前仓库中 [board/nutiny-nuc472/](file:///Volumes/nick-disk/projects/nos/board/nutiny-nuc472) 与 [soc/nuc472/](file:///Volumes/nick-disk/projects/nos/soc/nuc472) 仅包含最小可启动原型：
- 时钟停留在内部 22.1184 MHz HIRC，未启用板载 12 MHz HXT 与 84 MHz PLL；
- 未开启 MPU 保护与设备树，且 [arch/arm/mm/mpu.c](file:///Volumes/nick-disk/projects/nos/arch/arm/mm/mpu.c) 硬编码 STM32 Flash（`0x08000000` / 1 MiB）与 SRAM（`0x20000000` / 128 KiB），在 NUC472 上无法直接启用；
- 串口 [drivers/serial/uart_nuc472.c](file:///Volumes/nick-disk/projects/nos/drivers/serial/uart_nuc472.c) 仅为轮询收发，中断向量表未展开外设 IRQ 别名；
- 缺少 CON2 USB 复合设备、FMC 内部 Flash 块设备、LittleFS 持久化分区、`RTC SPARE` 启动标志，以及 SPI / I2C / PWM / EADC / RTC / WDT / Timer / GPIO-EXTI / CON1 PHY 探测等硬件外设支持；
- 缺少按需式 Lua 5.4 / 硬件自检交互体验。

### 1.2 设计目标与边界约束
1. **硬件全覆盖（除完整 TCP/IP 协议栈）**：支持 84 MHz 主频 PLL、完整中断/时钟/引脚复用（MFP）、中断驱动 UART0、CON2 Micro-USB 复合设备（`WinUSB` + `CDC` + `HID`）、FMC 内部 Flash、RTC/WDT/Timer、GPIO/EXTI、SPI、I2C、PWM、EADC，以及 CON1 `EMAC + IP101GR` PHY 硬件与链路探测（暂不内置完整 TCP/IP 协议栈）。
2. **Flash 分区与掉电状态**：APROM 采用 **`384 KiB` 内核区（`0x00000000 ~ 0x0005FFFF`） + `128 KiB` 内部 LittleFS 区（`0x00060000 ~ 0x0007FFFF`）**；挂载至 `/data` 并接入 `NLK_CAP_FLASH`；`boot_flag` 存入 `RTC SPARE` 寄存器实现跨复位掉电保持。
3. **软件全量默认开启（资源受控）**：默认在 [configs/nutiny-nuc472_defconfig](file:///Volumes/nick-disk/projects/nos/configs/nutiny-nuc472_defconfig) 启用 `CONFIG_MPU`、`CONFIG_MPU_STACK_GUARD`、`CONFIG_DEVICE_TREE`、`CONFIG_POSIX`、`CONFIG_KALLSYMS`、`CONFIG_PROCFS`、`CONFIG_SYSFS`、`CONFIG_RAMFS`、`CONFIG_LITTLEFS`、`CONFIG_USB_*`（Composite: WinUSB + CDC + HID）、`CONFIG_NLK_SERVER`、`CONFIG_SHELL`、`CONFIG_LUA`。明确关闭 **`CONFIG_FATFS=n`** 与 **`CONFIG_RAMDISK=n`**，动态内存分配器仅保留 **`CONFIG_MM_TLSF=y`**（`CONFIG_MM_SLUB=n`）。
4. **开机简洁与按需交互**：开机自动完成时钟、MPU、外设注册、`/data` 挂载、USB + NLK 服务与 `tty0` nShell 启动，避免后台测试任务刷屏或长期占用堆；Lua 脚本/REPL 与外设测试统一由 nShell 命令按需触发，运行结束立即释放资源。

---

## 2. 总体架构与四层模块边界

严格遵循 [ARCHITECTURE_AND_BUILD_SPEC.md](file:///Volumes/nick-disk/projects/nos/docs/ARCHITECTURE_AND_BUILD_SPEC.md) 及 [check_layer_deps.py](file:///Volumes/nick-disk/projects/nos/scripts/check_layer_deps.py) 的依赖约束，采用**方案一（`soc/nuc472` 原生模块化寄存器与 HAL 层 + 标准 NOS 驱动与设备树）**：

```text
+-------------------------------------------------------------------+
|  应用与板级集成: board/nutiny-nuc472/ , app/shell , app/lua , NLK |
|  (board.dts、LittleFS /data 自动挂载、RTC SPARE、hwtest/lua 命令) |
+-------------------------------------------------------------------+
|  通用驱动层: drivers/{serial,usb,flash,spi,i2c,...}/*_nuc472.c    |
|  (仅依赖 include/kernel, include/drivers, soc/nuc472; 禁含 board) |
+-------------------------------------------------------------------+
|  芯片 SoC 层: soc/nuc472/ (头文件 + clk / sys_mfp / fmc_ll 等)    |
|  (标准 <stdint.h> 定宽寄存器映射、解锁/时钟门控/引脚复用原语)       |
+-------------------------------------------------------------------+
|  Cortex-M4F 架构层: arch/arm/ (通用化 MPU、start.S、NVIC 分发)   |
+-------------------------------------------------------------------+
```

**类型与头文件隔离铁律**：
- `soc/nuc472/include/` 下的所有寄存器映射结构体必须全部使用 `<stdint.h>` 定宽类型（如 `volatile uint32_t`），禁止硬编码与 Newlib/Picolibc 冲突的标准库同名头文件或私有 `u32` 重定义。
- `drivers/` 下所有驱动源码禁止包含 `board.h` 或板级私有头；所有板级差异（引脚端口、引脚序号、MFP 功能编号、总线频率、通道索引）通过设备树节点属性或静态设备配置注入。

---

## 3. 核心架构：84 MHz PLL、MPU 通用化与中断向量

### 3.1 时钟系统与 84 MHz PLL（`soc/nuc472/clk.c`）
1. **启动与回退机制**：
   - `soc_early_init()` / `SystemInit()` 负责 FPU 使能（`CPACR` CP10/CP11）与早期系统保护寄存器解锁检查，保持 `HIRC` 可用以便早期串口初始化不依赖外部晶振。
   - 核对 NuTiny-SDK-NUC472 硬件晶振后，时钟模块使能 `12 MHz HXT`（设定有限超时等待 `CLK_STATUS` 稳定；若外部晶振异常则自动回退到内部 `HIRC` 并记录降级状态，保证串口与芯片不死机）。
   - 切换到高频前先配置 `FMC` 读闪存等待周期以满足 **84 MHz** 读取时序要求。
   - 锁定 PLL 至 `84 MHz` 切换为 `HCLK` 系统主频，并为 USB 控制器配置所需的稳定 `48 MHz` 时钟源；同步尝试起振 `32.768 kHz LXT` 供 `RTC` 使用（若未起振则回退 `LIRC`）。
   - 更新 `SystemCoreClock = 84000000U`（或实际降级频率），确保 [arch/arm/cpu/cortex-m4/cpu_c.c](file:///Volumes/nick-disk/projects/nos/arch/arm/cpu/cortex-m4/cpu_c.c) 中 `asm_cpu_init()` 配置的 `SysTick` 自动按精确主频产生 `CONFIG_SYS_TICK_MS` 节拍。
2. **集中式时钟与引脚复用管理（`soc/nuc472/sys_mfp.c`）**：
   - 为受写保护的 `SYS`/`CLK`/`FMC` 寄存器提供安全的临界区解锁-修改-加锁封装；
   - 提供模块时钟门控、模块复位与 `SYS->GPx_MFPL` / `SYS->GPx_MFPH` 掩码安全修改接口，防止多个外设驱动并发改写同一寄存器位域时产生竞态覆盖。

### 3.2 Cortex-M4 MPU 通用化改造（`arch/arm/mm/mpu.c`）
当前 [arch/arm/mm/mpu.c](file:///Volumes/nick-disk/projects/nos/arch/arm/mm/mpu.c#L84-L103) 将 Flash 写死在 `0x08000000` (1 MiB) 且 SRAM 写死在 `128 KiB`。为彻底支持 NUC472 及后续其它 Cortex-M4 芯片：
1. 在 [arch/arm/Kconfig](file:///Volumes/nick-disk/projects/nos/arch/arm/Kconfig) 新增板级/SoC 可覆盖的配置项（默认值兼容现有 STM32 预设，避免影响 `nk60-v2` 等既有平台）：
   - `CONFIG_MPU_FLASH_BASE`（默认 STM32F4 `0x08000000`，`SOC_NUC472` 默认 `0x00000000`）
   - `CONFIG_MPU_FLASH_SIZE`（默认 STM32F4 `0x100000`，`SOC_NUC472` 默认 `0x80000` 即 `512 KiB`）
   - `CONFIG_MPU_SRAM_BASE`（默认 `0x20000000`）
   - `CONFIG_MPU_SRAM_SIZE`（默认 STM32F4 `0x20000`，`SOC_NUC472` 默认 `0x10000` 即 `64 KiB`）
2. 在 `arch/arm/mm/mpu.c` 使用上述配置值编程 Region 1（Flash 只读 + 可执行）与 Region 2（SRAM 读写 + 不可执行 XN）。
3. 保留 `if (ARMV7M_SCB_VTOR != 0U)` 才映射 Region 0 空指针隔离区的条件：NUC472 从 `0x00000000` 直接启动（`VTOR == 0`），地址 `0x00000000` 存放初始 MSP 与中断向量表，跳过 Region 0 可避免正常取中断向量触发 MemManage Fault；同时默认开启 **`CONFIG_MPU_STACK_GUARD=y`**，在每条任务栈底部建立 32 字节不可访问硬件警戒区（Region 3），在 64 KiB 紧俏 SRAM 中第一时间拦截栈溢出。

### 3.3 中断向量表扩展（`soc/nuc472/irq_vectors.inc`）
1. **严格维持 `142` 个外部 IRQ（`IRQ 0 .. 141`）总长度不变**，确保 [tests/test_cortex_m_vectors.py](file:///Volumes/nick-disk/projects/nos/tests/test_cortex_m_vectors.py) 向量长度回归检验通过。
2. 按 NUC472 TRM 中断号依次补全常用外设的弱别名向量：`WDT_IRQHandler`、`RTC_IRQHandler`、`EINT0_IRQHandler`~`EINT7_IRQHandler`、`GPA_IRQHandler`~`GPI_IRQHandler`、`TMR0_IRQHandler`~`TMR3_IRQHandler`、`UART0_IRQHandler`~`UART5_IRQHandler`、`SPI0_IRQHandler`~`SPI3_IRQHandler`、`I2C0_IRQHandler`~`I2C4_IRQHandler`、`PWM0CH0_IRQHandler`~`PWM1CH2_IRQHandler`、`EADC00_IRQHandler`~`EADC03_IRQHandler`、`EMAC_TX_IRQHandler` / `EMAC_RX_IRQHandler`，以及已有的 `USBD_IRQHandler`、`USBH_IRQHandler`、`USB_OTG_IRQHandler`。

---

## 4. Flash 分区、FMC 块设备、LittleFS `/data` 与启动标志

### 4.1 APROM 384 KiB + 128 KiB 分区与链接脚本
修改 [board/nutiny-nuc472/nos_kernel.ld.S](file:///Volumes/nick-disk/projects/nos/board/nutiny-nuc472/nos_kernel.ld.S)：
- 将内核可用 `FLASH` 长度由 `512K` 收缩为 **`384K`**（`ORIGIN = 0x00000000, LENGTH = 0x60000`），超过 384 KiB 时链接期直接失败以形成硬性体积红线；
- 在 `0x00060000 ~ 0x0007FFFF` 定义 **`128 KiB`** LittleFS 持久化存储分区，并导出符号 `__lfs_flash_start = 0x00060000`、`__lfs_flash_size = 0x00020000` 供 FMC 块设备与 NLK 校验边界；
- 校验 `.ramfunc` 段被正确包含在 `.data`（或独立 `AT>FLASH` 段并在启动汇编/C 初始化阶段由 Flash 拷贝至 SRAM）中，确保片上擦写核心代码始终常驻 SRAM。

### 4.2 FMC 块设备驱动与单 Bank 擦写安全（`drivers/flash/fmc_nuc472.c`）
1. **单 Bank 同体取指防护**：NUC472 只有一块 APROM Bank，执行 `FMC->ISPTRG = 1` 触发 2 KiB 页擦除或 32-bit 编程时，不能同时从 APROM 读取指令。因此：
   - 底层 `fmc_ll_isp_exec()`（写 `ISPCMD`、`ISPADDR`、写/读 `ISPDAT`、触发 `ISPTRG` 与轮询忙标志直至清零）声明 `__attribute__((section(".ramfunc"), noinline))`，使其完全在 SRAM 中执行；
   - 执行单页擦除或字编程前使用 `disable_irq_save()` 关闭可屏蔽中断，防止高频 `SysTick` / USB / UART 中断在 Flash 处于擦写周期期间跳转回 APROM 取指；
   - 编程/擦除完成后清零/失效 NUC472 FMC 指令缓存相关控制位并执行 `__DSB(); __ISB();`，随后恢复中断并校验读取结果。
2. **无缝对接 `littlefs_vfs.c` 的块大小映射**：
   - [fs/littlefs/littlefs_vfs.c](file:///Volumes/nick-disk/projects/nos/fs/littlefs/littlefs_vfs.c) 固定要求 `block_size = 4096`，而 NUC472 FMC 硬件最小擦除页为 `2048 B`（2 KiB）。
   - `fmc_nuc472.c` 实现完整 [struct block_driver_api](file:///Volumes/nick-disk/projects/nos/include/drivers/block.h#L14-L22)：
     - 严格限定所有 LBA 访问必须在 `0x00060000 ~ 0x0007FFFF`（`128 KiB`）范围内，任何越界偏移直接返回 `-EINVAL`；
     - 向 VFS 报告 `sector_size = 512`，`sector_count = 256`（对应 `32` 个 4 KiB LittleFS 逻辑擦除块）；
     - `read()` 直接从 APROM 地址偏移读取（`memcpy`），高速零拷贝；
     - `write()` 检查 4 字节对齐与区域合法性，调用 `.ramfunc` 字编程逐字写入并回读比对；
     - `erase(lba, count)` 要求按 2 KiB 物理页对齐并循环擦除连续物理页，从而完美承接 LittleFS 每次 8 扇区（4 KiB）的块擦除请求（每次触发 2 次 2 KiB 页擦除）。

### 4.3 `/data` 自动挂载与 `NLK_CAP_FLASH`
- 设备注册为 `CONFIG_FLASH_DEV`（`nutiny-nuc472_defconfig` 默认设为 `"nuc472-flash"`）。
- 开机在 VFS 就绪后，[board/nutiny-nuc472/](file:///Volumes/nick-disk/projects/nos/board/nutiny-nuc472) 自动尝试 `vfs_mount("/data", CONFIG_FLASH_DEV, "littlefs")`；若首次使用片内 Flash 未格式化（`ENODEV` / corrupt superblock），自动执行一次格式化创建 `/data`（并在日志输出单行提示），后续重启自动保留已有文件与 Lua 脚本。
- 通过现有 NLK Flash 能力接口接入 `NLK_CAP_FLASH`，使上位机可通过 WinUSB 查看该 128 KiB 分区属性并对其进行读/写/擦除操作。

### 4.4 `RTC SPARE` 掉电保持启动标志
- 替换 [board/nutiny-nuc472/init.c](file:///Volumes/nick-disk/projects/nos/board/nutiny-nuc472/init.c) 当前返回 `-ENOSYS` 的 `board_write_boot_flag()` 与 `board_read_boot_flag()`；
- 在使能 RTC 时钟与 `RTC->RWEN` 访问密码解锁后，将 32-bit `boot_flag` 与魔数校验编码存入 `RTC->SPARE[0]` / `RTC->SPARE[1]`，保证 `reboot bootloader`、`reboot recovery` 及热重启期间状态可靠保留。

---

## 5. 串口与 CON2 Micro-USB 复合设备（WinUSB + CDC + HID）

### 5.1 中断驱动串口（`drivers/serial/uart_nuc472.c`）
- 硬件映射至板载 UART0（`PG.1` = `UART0_RXD`，`PG.2` = `UART0_TXD`，波特率 `115200 8N1`）。
- 延续设备注册名为 `"UART1"`，与 [drivers/serial/console_uart.c](file:///Volumes/nick-disk/projects/nos/drivers/serial/console_uart.c) 的绑定逻辑兼容。
- 启用 UART0 硬件 RX FIFO 触发中断（RDA）及接收超时中断（RXTO）与 TX FIFO 空中断（THRE）：
  - RX 中断将数据推入无锁/自旋锁保护的 RX `kfifo` 并唤醒等待任务；
  - TX 支持非阻塞环形发送 FIFO + 中断排空，避免串口日志或大量脚本输出长时间自旋阻塞调度器。

### 5.2 NUC472 USBD 控制器与复合设备（`drivers/usb/device/controller/usbd_nuc472.c`）
1. **控制器与 PHY 初始化**：
   - 使能 `SYS->USBPHY`（打开片上 USB PHY 与 LDO 3.3V 并选择 Device 角色）及 `USBD` 模块时钟；
   - 实现 [struct udcd_ops](file:///Volumes/nick-disk/projects/nos/include/drivers/usb/usb_device.h#L168-L182) 全部回调，接管 `USBD_IRQHandler` 中断处理（Bus Reset、Setup Packet、`CEP` 状态阶段、数据端点 IN/OUT 完成事件及 Suspend/Wakeup）。
2. **速度模式与端点池规划**：
   - 因 NOS 当前 [cdc.h](file:///Volumes/nick-disk/projects/nos/drivers/usb/device/class/cdc.h#L12)（`USB_CDC_BUFSIZE = 0x40`）与 [hid.c](file:///Volumes/nick-disk/projects/nos/drivers/usb/device/class/hid.c#L348) 固定采用 Full-Speed 64 字节最大包长描述符，本驱动默认配置为 **Full-Speed 12 Mbps（`dcd.device_is_hs = 0`）**，从根源上防止高速模式下主机端拒绝 64B Bulk 端点的兼容性失败，同时显著减小 64 KiB SRAM 上的缓冲需求。
   - **复合设备端点映射**：`WinUSB`（Bulk IN/OUT，2 端点）+ `CDC`（Int IN + Bulk IN/OUT，3 端点）+ `HID`（Int IN/OUT，2 端点）总计需要 **EP0 (CEP) + 7 个业务端点**。NUC472 USBD 控制器提供充分的独立可配置端点与片上专属 USB Buffer，驱动按功能需求顺序分配端点号 `1..7` 及独立不重叠片上 SRAM 地址段，在 [board/nutiny-nuc472/](file:///Volumes/nick-disk/projects/nos/board/nutiny-nuc472) 完成复合初始化并调用 `usbd_winusb_init()`、`usbd_vcom_init()`（当开启 CDC）与 `usbd_hid_init()`（当开启 HID）。
3. **上层服务接入**：
   - `WinUSB` 通过 [bearer_usb_winusb.c](file:///Volumes/nick-disk/projects/nos/app/server/nlk/bearers/bearer_usb_winusb.c) 对外呈现 `"NuTiny-NUC472"` NLK 设备（保留现有 `VID=0x4e4f`、`PID=0x0472`），无需额外工具即可用上位机连接通讯；
   - `CDC VCOM` 注册为虚拟串口设备，供按需自检测试或主机收发回环验证；
   - `HID` 提供自定义报告按需测试能力（不在开机后台盲目发送按键干扰主机）。

---

## 6. 扩展外设全覆盖、设备树与交互式 Shell 工具链

### 6.1 设备树（DTS）与 Bindings 规范
- 新增片上公共设备树 `dts/arm/nuvoton/nuc472.dtsi` 与板级 `board/nutiny-nuc472/board.dts`。
- 按 [dts/bindings/README.md](file:///Volumes/nick-disk/projects/nos/dts/bindings/README.md) 规则，在 `dts/bindings/` 新增对应 `nuvoton,nuc472-*.json` 约束文件（UART、GPIO、FMC、USBD、SPI、I2C、PWM、EADC、Timer、RTC、WDT、EMAC）。
- 所有就绪外设在初始化期执行 `nos_dt_device_bind()`，并通过 `DEVICE_DEFINE` 注册进系统设备表，可在 `lsdev` 与 `/sys` 中直接查看。

### 6.2 片上与板载外设驱动矩阵
所有外设保持按需访问、低静态 RAM 占用设计：

| 外设子系统 | 驱动 / 位置 | 接口规范 / 核心能力 | 无外部硬件下的板级验证方式 |
| :--- | :--- | :--- | :--- |
| **GPIO / EXTI** | `drivers/misc/gpio_nuc472.c` | Port A~I 方向/上下拉/翻转；`EINT`/Port 边沿中断注册与计数 | 操控 `PB.10` LED + 空闲排针输出/回读及中断统计 |
| **SPI 总线** | `drivers/spi/spi_nuc472.c` | 对接 [spi.h](file:///Volumes/nick-disk/projects/nos/include/drivers/spi.h) 全接口与 [spi_dt.c](file:///Volumes/nick-disk/projects/nos/drivers/spi/spi_dt.c)，支持 Mode 0~3 与硬件/GPIO 片选 | `hwtest spi` 短接 MISO/MOSI 实测，或使用片内环回校验 |
| **I2C 总线** | `drivers/i2c/i2c_nuc472.c` | 对接 [i2c.h](file:///Volumes/nick-disk/projects/nos/include/drivers/i2c.h)，支持 `RESTART`/`STOP`、超时与总线恢复 | `hwtest i2c` 扫描 `0x08~0x77` 7-bit 地址并返回 NACK/ACK 表 |
| **PWM** | `drivers/misc/pwm_nuc472.c` | 多通道可调周期/占空比硬件 PWM 输出 | 排针输出方波 + `hwtest pwm` 频率/占空比状态测试 |
| **EADC (12-bit)** | `drivers/misc/adc_nuc472.c` | 12 位 ADC 转换，支持外部模拟通道与片内传感器通道 | `hwtest adc` 直接采样芯片内部温度传感器与 Bandgap 参考源 |
| **Timer** | `drivers/misc/timer_nuc472.c` | `TMR0..3` 硬件计数、微秒定时与周期中断回调 | `hwtest timer` 校验硬件定时误差（对比 `SysTick`） |
| **RTC** | `drivers/misc/rtc_nuc472.c` | 年月日时分秒读写、`SPARE[0..19]`、`RTC` 秒周期/闹钟 | `hwtest rtc` 读写时钟走时 1 秒增量与 `SPARE` 寄存器检验 |
| **WDT** | `drivers/misc/wdt_nuc472.c` | 可配置超时周期看门狗、显式喂狗接口与复位标志检测 | `hwtest wdt` 以安全超时测试喂狗与状态清零（不误触发重启） |
| **EMAC PHY 探测** | `drivers/misc/emac_probe_nuc472.c` | 配置 RMII 引脚、`EMAC` 时钟与 MDIO 读写时序 | `hwtest eth` 读取 `CON1` 板载 `IP101GR` PHY ID 与 Link/双工状态 |

### 6.3 按需式 Lua 5.4 与板级 nShell 工具链
1. **Lua 5.4 按需化改造（[app/lua/](file:///Volumes/nick-disk/projects/nos/app/lua)）**：
   - 将原 [lua_test.c](file:///Volumes/nick-disk/projects/nos/app/lua/lua_test.c) 开机自动跑 demo 的固定任务改为可配置/按需入口（通过单独的 `CONFIG_LUA_BOOT_TEST` 默认关闭，不再在开机后台静默占用 4 KiB 栈与日志输出）；
   - 通过 `shell_register_cmd_ext` 注册 **`lua`** 命令：
     - `lua -e "<code>"`：直接求值内联脚本片段；
     - `lua <file>`：从 VFS（如 `/data/demo.lua`）读取并执行脚本；
     - `lua`（无参）：进入交互式多行/单行 REPL，`Ctrl-D` 或 `.exit` 退出；
     - 每次启动按需创建受控大小的临时运行环境/任务（配置充足安全栈，受 `CONFIG_MPU_STACK_GUARD` 硬件保护），执行结束立即 `luaL_close` 归还全部 TLSF 动态堆内存。
2. **板级交互诊断命令**：
   - **`hwinfo`**：一行式汇总展示核心主频（84 MHz / 时钟源）、MPU 区域与栈警戒状态、`RTC SPARE` 启动标志、Flash 384 KiB + 128 KiB 划分信息、TLSF 堆余量与最大连续块大小。
   - **`hwtest <subcmd>`**：支持 `all`、`clk`、`fmc`、`fs`、`rtc`、`adc`、`timer`、`gpio`、`spi`、`i2c`、`pwm`、`wdt`、`usb`、`eth` 分项或综合自检。
   - **`led <on|off|toggle|blink|status>`**：交互式控制或切换板载 `PB.10` LED 模式（默认上电仅提供简洁心跳，随时可用命令接管检验）。

---

## 7. 资源预算控制（384 KiB Flash / 64 KiB SRAM）与 Kconfig 策略

### 7.1 核心 Kconfig 策略（[configs/nutiny-nuc472_defconfig](file:///Volumes/nick-disk/projects/nos/configs/nutiny-nuc472_defconfig)）
- **分配器**：仅启用 **`CONFIG_MM_TLSF=y`**，明确 **`CONFIG_MM_SLUB=n`**、`CONFIG_MM_DEBUG=n`，消除双分配器结构头与页管理元数据浪费；
- **系统保护与核心**：`CONFIG_ARM_CORTEX_M4_FPU=y`、`CONFIG_MPU=y`、`CONFIG_MPU_STACK_GUARD=y`、`CONFIG_DEVICE_TREE=y`、`CONFIG_POSIX=y`、`CONFIG_KALLSYMS=y`；
- **文件系统**：`CONFIG_VFS=y`、`CONFIG_LITTLEFS=y`、`CONFIG_ASSETS_FS_LITTLEFS=y`（或板级自动挂载触发）、`CONFIG_RAMFS=y`、`CONFIG_PROCFS=y`、`CONFIG_SYSFS=y`；明确 **`CONFIG_FATFS=n`**、`CONFIG_RAMDISK=n`；
- **USB / NLK / Shell / Lua**：`CONFIG_USB=y`、`CONFIG_USB_DEVICE=y`、`CONFIG_USB_DEVICE_COMPOSITE=y`、`CONFIG_USB_WINUSB=y`、`CONFIG_USB_CDC=y`、`CONFIG_USB_HID=y`、`CONFIG_NLK_SERVER=y`、`CONFIG_TTY=y`、`CONFIG_SHELL=y`、`CONFIG_LUA=y`。

### 7.2 内存与代码体积约束红线
1. **Flash 预算红线（严格 `<= 384 KiB`）**：
   - 基线镜像当前为 ~194 KiB；预计新增 USB 栈、LittleFS、全套 NUC472 外设驱动、Lua 5.4 核心后目标控制在 **`330 ~ 360 KiB`** 之间，留出 **`>= 24 KiB`** Flash 余量。
   - 控制手段：保持 `-Os` 优化；如果启用 `CONFIG_KALLSYMS` + Lua 标准库后接近 370 KiB，优先裁剪不必要的 Lua 重量级标准模块（如冗余调试或重复 IO 封装，保留 `base/table/string/math/io/os` 核心可用）及精简外设冗余常量表。
2. **SRAM 预算红线（总计 `64 KiB`）**：
   - 静态 `.data + .bss` 目标控制在 **`<= 20 KiB`**（谨慎控制 UART RX/TX FIFO、USB 类驱动端点缓冲与 `CONFIG_LOG_FIFO_BUF_SIZE`，避免各模块盲目申请数 KiB 静态缓冲）；
   - 常驻任务栈总和（Idle 1 KiB + Shell ~3–4 KiB + USB/NLK 等服务任务）控制在 **`<= 12 KiB`**；
   - **可用 TLSF 堆裕度目标 `>= 30 KiB`**：日常待机仅承担 LittleFS 挂载元数据（约 4–8 KiB），执行 `lua` 命令时动态申请虚拟机内存（约 12–18 KiB）并在退出时完整释放，配合 `MPU_STACK_GUARD` 实现零踩踏运行。

---

## 8. 测试与验收计划

1. **主机自动化测试与静态检查**：
   - `python3 tests/run_host_regressions.py`
   - `python3 -m unittest discover -s tests -p 'test_*.py'`（包含 `test_cortex_m_vectors.py` 对 `nutiny-nuc472` `16 + 142` 中断向量的严格检验）
   - `cmake -P tests/test_arch_flags.cmake` 与 `cmake -P tests/test_soc_headers.cmake`
   - `python3 scripts/check_layer_deps.py`（验证无跨层头文件违规）
   - **新增专项主机/构建测试**：验证 `arch/arm/mm/mpu.c` 参数化 Flash/SRAM 区域计算、`fmc_nuc472` 逻辑 LBA 到 2 KiB 物理页擦写映射与越界拒止、`RTC SPARE` 标志编解码逻辑、`nutiny-nuc472` 设备树 binding 验证与 `384 KiB / 64 KiB` 容量门限。
2. **全预设编译与仿真回归**：
   - 全部 5 个 Cortex-M 预设（`nutiny-nuc472`、`nk60-v2`、`qemu-stm32`、`zj-v3`、`wl-001`）执行配置+构建，达到零错误、零新增编译警告；
   - 执行 `python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf` 确保共用代码改动零回归；
   - 检查 `arm-none-eabi-size build/nutiny-nuc472/nos.elf` 实测值完全符合 `<= 384 KiB` APROM 与 `64 KiB` SRAM 约束。
3. **实板烧录手工验收流程（NuTiny-SDK-NUC472）**：
   - 通过 OpenOCD（[board/nutiny-nuc472/openocd.cfg](file:///Volumes/nick-disk/projects/nos/board/nutiny-nuc472/openocd.cfg)）烧录，串口 `tty0` 查看启动日志无异常直接落入 `nsh>` 提示符；
   - 主机连接 `CON2` Micro-USB 确认 `WinUSB` + `CDC` + `HID` 复合设备正常枚举，`nlk` 上位机可读取设备与 Flash 能力；
   - 在 `tty0` 依次执行 `hwinfo`、`lsdev`、`df`、`hwtest all`、`lua -e "f=io.open('/data/test.txt','w') f:write('ok') f:close()"` 后复位板卡再读取 `/data/test.txt` 与 `reboot` 检查 `RTC SPARE` 标志，全部通过。
