# nk60_v2 硬件迁移依据

本适配恢复的是旧 NOS 的 nk60_v2 61 键、60% ANSI 键盘。硬件依据为仓库历史提交 `4c31189` 的真实驱动和启动代码，而不是历史通用 DTS。当前板级库通过 `boards/nk60_v2/hardware.h` 的 MMIO、时间和 DMA 回调运行，由具有设备能力的用户态服务调用；没有恢复旧 RTOS 的设备注册、线程、信号量或标准外设库。

## MCU、时钟与存储边界

历史 `board/nk60/Makefile.config` 明确使用 `STM32F401xx`、Cortex-M4 和 FPv4-SP。USB 代码还使用 F401 xC 的器件选择；源代码不能证明实物封装和丝印，因此烧录前仍应核对实际 MCU。历史头文件中的 `HSE_VALUE=8000000`，`system_stm32f4xx.c` 配置如下：

| 项目 | 原板配置 |
| --- | --- |
| HSE | 8 MHz |
| PLL M/N/P/Q | 8 / 336 / 4 / 7 |
| SYSCLK / HCLK | 84 MHz |
| APB1 | 42 MHz |
| APB2 | 84 MHz |
| USB OTG FS | 48 MHz |

旧链接脚本只给出 64 KiB RAM 和 224 KiB 应用 Flash，起始地址由 `CONFIG_KERNEL_ADDR` 控制。224 KiB 是旧软件链接范围，不能据此认定芯片物理 Flash 容量，也不能推断已有 bootloader 位于哪个区域。F401 xC 的保守平台上限是 256 KiB Flash、64 KiB SRAM；当前适配将应用范围进一步限定为前 128 KiB（0x08000000–0x0801ffff），保留后 128 KiB。F401 的 sector 5 覆盖 128–256 KiB；若仅按旧 224 KiB 软件边界烧录，擦除该扇区仍会破坏最后 32 KiB。因此链接器和烧录器共同使用 128 KiB 擦除对齐边界，不进行整片擦除或触及 sector 5。其他板型须显式配置并确认实物。历史 DTS 的 25 MHz HSE、96 KiB RAM 和 512 KiB Flash 与实际编译路径矛盾，不能用于这块板的默认配置。

## 已确认接线

| 功能 | 引脚/外设 | 依据与约束 |
| --- | --- | --- |
| 74HC595 锁存 | PB12 | 输出；高脉冲后回低 |
| 74HC595 时钟 | PB13 | 输出；上升沿移位 |
| 74HC595 数据 | PB15 | 输出；一位低选择键位 |
| 共享按键输入 | PB14 | 上拉输入，低电平表示按下 |
| 额外输入 | PA0 | 旧驱动配置上拉；具体用途未证实 |
| W25Q CS | PB2 | 输出，低有效 |
| W25Q SCK/MISO/MOSI | PB3/PB4/PB5，SPI1 AF5 | mode 3，84 MHz /4 = 21 MHz |
| RGB 电源 | PB0 | 高有效，初始化默认关闭 |
| RGB 串行波形 | PC3，SPI2 MOSI AF5 | 单线 TX，42 MHz /8 = 5.25 MHz |
| RGB DMA | DMA1 Stream4 Channel0 | 字节源递增、固定 SPI2 DR，不允许用户指定其他目的地址 |
| USB D-/D+ | PA11/PA12，AF10 | OTG FS，IRQ 67；旧板禁用 VBUS sensing |
| 调试 UART | PA9/PA10，USART1 AF7 | 占用 PA9，因此不能直接套用 PA9 VBUS sensing |
| 旧 USB-C 检测输入 | PC15、PD2 | 原文件命名 CC1/CC2，输入无上下拉；未据此实现供电协商 |

PB3 的复用与 JTAG 引脚有关；本板使用 SWD 的 PA13/PA14 进行调试时应保持其配置。PC13 并没有在旧 nk60_v2 LED 驱动中作为板载状态灯使用。

## 扫描链，而非行列矩阵

`drivers/key/nk60_v2_key.c` 显示硬件由 9 颗 74HC595 的 72 位选择链和一个共享返回输入组成。每次注入一位低电平，再连续移入高电平，并在每次移位后锁存和读取 PB14。只有 61 个槽连接按键，其余槽必须忽略。

当前 `nk60_scan_to_key[]` 完整保留原映射。物理键编号依次为 ANSI 五行的 14、14、13、12、8 个键，底行是左 Ctrl、左 GUI、左 Alt、Space、右 Alt、PN、右 Ctrl、FN。初始化先移入 72 个高电平，避免上电随机状态造成首帧多键误报；`nk60_matrix_scan()` 只返回原始 61 位按下集合，消抖和按键策略由用户态键盘引擎处理。

原时序每槽含数据建立 1 µs、时钟高 1 µs、锁存高 1 µs、输入稳定 10 µs，72 槽至少 936 µs。设备系统调用、调度及 USB 处理另有开销，因此实际扫描周期必须测量，不能把旧代码推断成有保证的 1 kHz。矩阵库使用 BSRR 单引脚写操作，避免改写其他同端口设备电平。

## RGB 与旧 display

旧 `app/display/nk60_display_server.c` 操作的是 14×5 逻辑 RGB 画布，不是独立 LCD/OLED。`nk60_canvas_to_led[]` 保留其 70 个逻辑坐标到 61 颗灯的映射，宽键占据多个坐标。

旧灯驱动未明确给出 LED 芯片料号。已证实的协议是 GRB 顺序、每分量 8 位、MSB 先发：每个颜色位用一个 SPI 字节表示，0=`0xc0`、1=`0xf0`。61×24=1464 字节，一帧理论发送约 2.23 ms。它与某些 WS2812 类灯协议类似，但仍须在实物上确认脉宽和复位时间。

`nk60_rgb_encode()` 生成连续波形；`nk60_rgb_write()` 要求阻塞 DMA 回调，随后确认 SPI TXE/BSY，关 SPI 并把 PC3 切回低电平 GPIO 至少 80 µs。DMA 错误会关闭灯电源。缺少 DMA 回调明确返回 `-ENOSYS`；逐字节 MMIO 系统调用不能保证波形连续性。DMA 服务必须验证源缓冲区、限定 SRAM、限定长度和目的寄存器、对所有完成/错误/超时路径停流。驱动保留原 5.25 MHz 波形，不声称已经验证未知 LED 型号。

61 颗全白灯可能超过 USB 默认供电预算。亮度限制、USB 配置前及挂起时断电应由服务策略执行；没有实测单灯电流、整板电流和 USB-C 电阻配置时，不能宣称满足某个实际功耗值。

## 外部 NOR 与 USB 进度

原 W25Q 驱动支持 W25Q80/16/32/64/128，通过旧 `0x90` ID 区分，实际板载容量需要探测。当前驱动使用 JEDEC `0x9f`，接受 Winbond `0xef`、type `0x40/0x60`、density `0x14..0x18`（1..16 MiB）；其他 ID 返回 `-ENODEV`，不假设未识别芯片容量。页为 256 字节、擦除扇区为 4096 字节，使用 24 位地址。

`nk60_flash_program()` 按页拆分并回读验证，`nk60_flash_erase_sector()` 要求扇区对齐并回读整扇区验证。写使能锁存位、SPI TXE/RXNE/BSY、NOR WIP 均检查，有时间和迭代双重超时；受保护、无法写入或无法擦除返回错误。写入不会自动擦除已有数据，存储格式及断电恢复由上层持久化协议负责。

`nk60_hw_set_progress()` 注册可选事件回调。WIP 忙等待每次轮询、普通读取每 64 字节独立事务后，在 CS 已释放时调用它，使用户态服务可继续处理 USB。回调不可重入 Flash 操作、改 SPI1 或触发持久化递归。Flash 页写本身不在 CS 有效期间让出；USB 最大延迟仍须板上实测。

## 验证范围

`tests/host_nk60_hardware.py` 用 ASan/UBSan 编译实际板级库，注入 MMIO、74HC595、SPI NOR 和 DMA 模型，验证逐键映射与释放、首帧选择链初始化、页边界、容量/地址边界、写保护、忙超时、停止的时钟、SPI 超时、USB 进度回调、RGB 编码和错误断电。此测试不能验证物理接线、HSE 频率、信号完整性、USB 枚举、DMA 波形或灯电流。

原版 QEMU `netduinoplus2` 仅适合作为通用 STM32/MPU 内核回归，既不是 nk60_v2 仿真，也不能据其通过宣称键盘硬件可用。实际验收需要 SWD 烧录、串口启动日志、USB 枚举和主机输入检查、61 键逐键与组合测试、RGB 波形/功耗检查、NOR 写入及断电恢复测试。

历史依据均可通过 `git show 4c31189:<path>` 复核：`board/nk60/{Makefile.config,nos_kernel.ld.S,system_stm32f4xx.c,include/stm32f4xx.h,arch_usb.c,board_config.c}`、`drivers/key/nk60_v2_key.c`、`drivers/spi/nk60_v2-w25qxx.c`、`drivers/misc/nk60_v2_led.c`、`include/display/led.h`、`app/display/nk60_display_server.c`。
