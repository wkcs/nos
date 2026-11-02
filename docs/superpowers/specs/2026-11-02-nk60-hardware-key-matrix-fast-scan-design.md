# NK60-V2 键盘矩阵硬件扫描加速设计规范

## 1. 背景与目标

NK60-V2 键盘主控采用 STM32F401，键盘硬件按键矩阵由 9 片级联的 74HC595 移位寄存器构成（提供 72 位输出通道），通过单一汇聚输入引脚采集 61 个按键的状态。

当前驱动位于 `board/nk60-v2/key_matrix.c`，使用 GPIO 软件位带模拟时序（Bit-banging）。由于每次时钟脉冲和锁存过程均插入了基于 SysTick 轮询的 `usleep(1)` 与 `usleep(10)`，单次扫描 72 位按键耗时高达 **936 µs**（接近 1 ms）。在以 1000 Hz 刷新率轮询时，CPU 忙等耗尽率接近 100%，对系统多任务实时性造成严重拖累。

本项目旨在：
1. 评估硬件 SPI 对 74HC595 矩阵驱动的可行性。
2. 重构 `key_matrix.c` 扫描时序，消除冗余软件等待，将单次扫描耗时压缩至 **20 ~ 50 µs**（性能提升 20 ~ 45 倍）。
3. 保持现有字符设备与上层 `keyboard_server`、NLK 软总线协议的 100% 接口兼容。

---

## 2. 硬件 SPI 评估结论与外设分析

### 2.1 引脚布局与硬件 SPI2 关系
NK60-V2 板级连接引脚与 STM32F401 的 SPI2 映射完全重合：
* `PB12` $\leftrightarrow$ `SPI2_NSS`（连接 74HC595 的 `ST_CP / RCLK` 锁存引脚）
* `PB13` $\leftrightarrow$ `SPI2_SCK`（连接 74HC595 的 `SH_CP / SRCLK` 移位时钟）
* `PB14` $\leftrightarrow$ `SPI2_MISO`（连接 61 键汇聚上拉输入线 `KEY_DATA`）
* `PB15` $\leftrightarrow$ `SPI2_MOSI`（连接 74HC595 的 `SER / DS` 串行数据输入）

### 2.2 硬件 SPI 不适用的技术原因
1. **74HC595 双级寄存器时序矛盾**：
   * 74HC595 内部具有“移位寄存器”与“存储寄存器（输出锁存）”两级结构。
   * 硬件 SPI 的 SCK 仅驱动移位寄存器。若使用单次 SPI 连续发送 72 位数据，在整个传输期间片选 NSS 保持不变，74HC595 的外部并行引脚 $Q_0 \dots Q_{71}$ 不会逐位更新输出，导致 MISO（PB14）无法在每个时钟边沿采集到对应的单个按键按下状态。
   * STM32 硬件 SPI 无法在每个 bit 之后自动生成纳秒级的 NSS 锁存上升沿脉冲。
2. **外设资源冲突**：
   * `board/nk60-v2/led.c` 已占用 `SPI2`（通过 `PC3` 复用为 `SPI2_MOSI`）配合 `DMA1_Stream4` 驱动键盘全键 RGB 背光（WS2812）。
   * 若键盘扫描强制占用 SPI2，将导致 SPI2 寄存器状态与 DMA 通道引发硬件冲突。

**结论**：放弃硬件 SPI 方案，采用“纳秒级直接寄存器操作 + 精确 RC 建立时间”的极速 GPIO 优化方案。

---

## 3. 详细设计

### 3.1 寄存器原子操作替换位带访问
* 将原有的 `PBout(12)`、`PBout(13)`、`PBout(15)` 和 `PBin(14)` 宏替换为 CMSIS 标准的 `GPIOB->BSRR` 与 `GPIOB->IDR`。
* `GPIOB->BSRR` 单次写操作为 32 位原子访问，无读-改-写竞态，且可在 2~3 个总线时钟周期内完成（约 24~35 ns）。

### 3.2 纳秒级脉冲生成机制
根据 74HC595 官方数据手册，在 3.3V 供电下的最小时序参数要求：
* 移位时钟脉冲高电平宽度 $t_{w(CLK)} \ge 20\,\text{ns}$
* 锁存时钟脉冲高电平宽度 $t_{w(EN)} \ge 20\,\text{ns}$
* 串行数据建立时间 $t_{su} \ge 20\,\text{ns}$
* 串行数据保持时间 $t_h = 0\,\text{ns}$

在 STM32F401（84 MHz 主频，每周期约 11.9 ns）下：
* 触发高电平：`GPIOB->BSRR = (1UL << pin);`
* 插入 2~4 个 `__NOP()`（24~48 ns）
* 触发低电平：`GPIOB->BSRR = (1UL << (pin + 16));`
* 此脉冲总持续时间为 35~60 ns，在所有工作温度与供电波动下均能稳定满足 74HC595 规格要求，无需调用开销极大的 `usleep()`。

### 3.3 回路 RC 稳定延时设计
* 矩阵中 61 颗按键二极管及走线具有寄生电容（$C \approx 30 \sim 50\,\text{pF}$）。当扫描位从“被按下的键”（拉低到 0V）切换到“未被按下的键”时，上拉电阻需对回路电容充电至高电平逻辑门限（$V_{IH} \ge 0.7 V_{DD}$）。
* STM32F4 内部上拉电阻 $R_{pu} \approx 40\,\text{k}\Omega$，充放电时间常数 $\tau = R \times C \approx 1.2 \sim 2.0\,\mu\text{s}$。若 PCB 上已配置外部弱上拉（如 4.7 k$\Omega$），建立时间常数将缩短至约 $200\,\text{ns}$。
* 设计专用内联稳定延时函数 `key_settle_delay()`：
  * 提供宏 `CONFIG_KEY_SETTLE_CYCLES`（默认根据内部上拉设定为 60~80 个周期，约 700~950 ns）。
  * 彻底移除原 `usleep(10)`（10,000 ns）的长延时。

### 3.4 扫描主流程重构
```c
static inline void hc595_fast_pulse_clk(void)
{
    GPIOB->BSRR = (1UL << 13);
    __NOP(); __NOP();
    GPIOB->BSRR = (1UL << (13 + 16));
}

static inline void hc595_fast_pulse_latch(void)
{
    GPIOB->BSRR = (1UL << 12);
    __NOP(); __NOP();
    GPIOB->BSRR = (1UL << (12 + 16));
}

static inline void key_matrix_settle_delay(void)
{
    /* 约 60 周期（在 84MHz 下约为 714ns） */
    for (volatile int i = 0; i < 15; i++) {
        __NOP();
    }
}

static void nk60_v2_key_scan_once(uint32_t *raw_data)
{
    int i;
    memset(raw_data, 0, HC595_RAW_DATA_NUM * sizeof(uint32_t));

    for (i = 0; i < HC595_BIT_NUM; i++) {
        if (i == 0) {
            GPIOB->BSRR = (1UL << (15 + 16)); /* 送 0 */
        } else {
            GPIOB->BSRR = (1UL << 15);        /* 送 1 */
        }
        __NOP();
        hc595_fast_pulse_clk();
        hc595_fast_pulse_latch();
        key_matrix_settle_delay();

        if ((GPIOB->IDR & (1UL << 14)) == 0) {
            raw_data[KEY_RAW_GROUP(i)] |= (1UL << KEY_RAW_BIT(i));
        }
    }
}
```

### 3.5 接口与 ABI 契约保持
* 注册设备名称保持 `"nk60_v2-key"`。
* 控制命令 `KEY_CTRL_ENABLE`、`KEY_CTRL_DISABLE`、`KEY_CTRL_GET_STATS` 等完整保留。
* `dev->ops.read` 输出的逻辑键位图格式保持不变，驱动内部映射表 `s_nk60_position_index` 逻辑保持不变。

---

## 4. 性能指标预期

| 指标 | 改造前 | 改造后（预期） | 提升倍数 |
| :--- | :--- | :--- | :--- |
| **单 bit 移位与回读延时** | 13 µs | ~0.8 µs | ~16x |
| **72 位单次全扫描耗时** | 936 µs | 55 ~ 65 µs | **14 ~ 17x** |
| **1000 Hz 轮询时 CPU 占用** | ~93.6% (死等) | ~5.8% | 释放 87% CPU 算力 |
| **100 Hz 轮询时 CPU 占用** | ~9.4% | ~0.6% | 几乎可忽略 |

---

## 5. 验证与回归计划

1. **主机回归测试**：运行 `python3 tests/run_host_regressions.py`，确保上层 keyboard profile 与系统核心无破坏。
2. **多板级固件构建验证**：
   * `cmake --preset nk60-v2 && cmake --build --preset nk60-v2`
   * `cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32`
   * `cmake --preset zj-v3 && cmake --build --preset zj-v3`
   * `cmake --preset wl-001 && cmake --build --preset wl-001`
3. **QEMU STM32 冒烟测试**：`python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf`。
