# 驱动模型与设备树

## 分层

`arch/` 实现处理器启动、异常与上下文切换；Cortex-M 复位代码调用
`soc_early_init()`，外部中断表由选定的 SoC 提供。`soc/` 提供芯片级
初始化和厂商外设代码，`board/` 提供板卡引脚、时钟和设备装配。
`drivers/` 包含控制器、设备协议和设备类别接口。

`struct device` 提供设备注册、就绪状态和类型化驱动 API。启用设备树后，
`nos_dt_device_bind()` 根据 `compatible` 匹配已初始化的设备并调用驱动
`probe`；`probe` 失败时不会绑定，调用方可在 `-EAGAIN` 时再次尝试。
目前重试由调用方负责，不提供自动设备枚举、热插拔或自动解除绑定。
设备必须在绑定期间保持有效；`probe` 失败时由驱动清理其资源。

## 设备树构建

开启 `CONFIG_DEVICE_TREE` 时，CMake 使用交叉编译器预处理
`board/<board>/board.dts`，然后通过仓库固定版本的 Zephyr `dtlib` 生成
`build/<board>/generated/devicetree.c`。SoC 可复用的描述放在 `dts/`，
板级 DTS 选择启用的设备。设备树数据以只读节点和原始属性字节表示；
`include/nos/devicetree.h` 提供路径、phandle、兼容项、状态和 32 位单元
查询。节点数据未用于描述内核与用户态之间的 ABI。

`dts/bindings/` 中的 NOS JSON binding 校验已声明的属性。未找到 binding
的节点目前不会被校验；格式不是 Linux dt-schema 或 Zephyr YAML 的替代品。
QEMU STM32 控制台、NK60 闪存和可选的 ZJ-V3 SD 卡已接入设备树，其余旧驱动
尚未全部迁移。运行时传入 DTB、自动识别硬件和完整 binding 覆盖尚未实现。

## SPI 与 I²C

`include/drivers/spi.h` 使用控制器设备、频率、模式、片选和缓冲段描述
事务。一次 `spi_transceive()` 保持片选直到所有缓冲段完成。显式会话
`spi_session_begin()` / `spi_session_transfer()` / `spi_session_end()`
允许多次调用之间保持片选；会话必须在同一任务中结束，期间不可从该任务
再次访问同一控制器。传输失败时会自动结束会话。
`spi_session_end_with_idle()` 在释放总线锁之前撤销片选并发送空闲时钟，
供 SD 卡等设备释放 MISO；控制器需显式实现该操作。

STM32F1/F4 SPI 控制器支持 APB1/APB2、多个 GPIO 片选、8 位帧、四种模式和
不超过设备要求的时钟频率。板卡负责实例化控制器并提供引脚及总线时钟。
W25Q 协议和扇区存储逻辑位于 `drivers/flash/`，NK60 板级装配位于
`board/nk60-v2/`。部分扇区更新保留其余数据，写入前分配扇区缓冲区；
写入失败或掉电后不能保证事务原子性。当前路径已通过模拟设备测试，
仍需在 NK60 实物上验证。

通用 SD SPI 层提供卡初始化、容量读取和 512 字节扇区读写，块设备
适配层支持多扇区请求。ZJ-V3 可选启用 `CONFIG_ZJ_V3_SD_BLOCK`，在调度器
启动后根据设备树初始化 SPI2 并注册 `sd0`。此路径尚未经过
实板验证，也不会自动改变 FatFs 的块设备或在启动时挂载 SD 卡。

`include/drivers/i2c.h` 使用消息序列描述 7 位地址的 I²C 事务，支持
repeated-start 和 stop。GPIO 控制器由 `CONFIG_I2C_GPIO` 选择，使用板级
开漏 GPIO 和延时回调，支持单主机及有限时钟延伸；目前没有板级 GPIO
实例接入，也没有 STM32 硬件 I²C 控制器实现。

## 验证

```text
python3 tests/run_host_regressions.py
python3 -m unittest discover -s tests -p test_devicetree_gen.py
python3 -m unittest discover -s tests -p test_cortex_m_vectors.py
cmake --preset qemu-stm32
cmake --build --preset qemu-stm32
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```

`qemu-run` 首次配置时从 `assets/qemu/disk.img` 创建构建目录中的可写
`disk.img`，之后不自动覆盖；QEMU 工作目录为对应构建目录。当前默认
FatFs 块设备是 `ramdisk0`，并不会自动使用该镜像。
