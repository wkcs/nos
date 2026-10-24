# 设备驱动框架验证记录

<!-- Copyright (C) 2026 NOS contributors -->

日期：2026-10-24。实现与使用方式见 [设备驱动模型](DEVICE_DRIVERS.md)。

被测实现提交：`bc5a3792dffb84935fbdb16286248ef790848c70`；测试前后均为干净工作区，源码身份无变化。本报告是验收完成后单独添加的文档，不属于该被测提交。

源码身份对象（排序 JSON）SHA-256：`47e95fbbe4a24808e2f004c6cbec5bc6bbee72db47b6042aa9f3bd6a1b200eb7`。

执行命令：

```sh
source /workspace/.nos-env/activate.sh
python3 tests/acceptance.py --section boards --section baseline --section rv32 --require-frozen-source
```

结果：**14/14 分组通过**，所有分组退出码 0。记录：`out/microkernel/acceptance/1792810656798501082/results.json`。以下输出路径只保存在本工作区的忽略目录，不随 Git 提交上传。

| 分组 | 结果 |
|---|---|
| eight_platform_libc_combinations | PASS |
| portable_device_driver_framework | PASS |
| nlp_reference_wire_registry | PASS |
| nlp_actual_c_python_interoperability | PASS |
| keyboard_host_sanitizers | PASS |
| usb_keyboard_host_sanitizers | PASS |
| nk60_hardware_host_sanitizers | PASS |
| nk60_device_host_sanitizers | PASS |
| nk60_service_host_sanitizers | PASS |
| nk60_flash_image_validation | PASS |
| nk60_firmware_cross_builds | PASS |
| nk60_cortex_m_startup_stubbed_clock | PASS |
| riscv32_virt | PASS |
| riscv32_nommu | PASS |

原有 ARM/ARM64/RV64/STM32 的 8 个平台/C 库组合，以及 RV32 MMU、无 MMU 组合均经过真实 QEMU 启动。新增 `driver_framework_userspace_lifecycle` 是每次来宾验收的必需标记，验证实际用户态框架的资源失败回滚、64 位 token、GPIO 极性和使用中拒绝卸载。

新增宿主框架测试在 ASan/UBSan/LSan 正常退出下通过，包含独立 SPI NOR 控制器模型和实际 16550 寄存器驱动。4 个公共驱动源文件也通过 GCC `-Wall -Wextra -Werror -fanalyzer` 编译检查。NK60 服务的长驻 loopback 夹具仍按原有方式终止，不能将该进程的终止日志当作正常退出的泄漏扫描。

## NK60 固件预算

固件记录：`out/microkernel/nk60-firmware/1792810796264203078/results.json`。

| MPU | bin 字节 | 用户静态区 / 16384 | 静态区末尾 | MSP 预留字节 | 已知核心 load/save 栈帧合计 |
|---|---:|---:|---|---:|---|
| 0 | 96636 | 16112 | 0x2000bef0 | 16656 | 2528 / 2584 |
| 1 | 97620 | 16112 | 0x2000bef0 | 16656 | 2528 / 2584 |

MPU=0 ELF SHA-256：`e06543b0ef936211b961443962176fcd70940c43dceedfb8779bcad9164196e9`。

MPU=1 ELF SHA-256：`6f81296e24ab7dc6a415b72b50b2a3c52d4471b358bffe4ff66f16f62d18729d`。

两种配置均满足 128 KiB 应用 Flash、64 KiB SRAM 和 4 KiB 用户栈预留约束。用户静态区余量 **272 字节**，继续增加 NK60 常驻状态时必须复查预算。核心栈帧合计不包含所有间接驱动/progress 回调，也不是实测高水位。

## 范围与未验证事项

- 框架和外设协议位于用户态；没有扩大 NK60 MMIO/DMA 权限或改变 syscall ABI。
- NK60 GPIO/SPI NOR/USB IN 已接入实际公共实现；I²C 控制器、16550 的验证是模型，不是新平台硬件验收。
- USB 当前是 device 数据通路；host/hub/xHCI、通用端点/类注册、异步 URB 尚未提供。
- ESP32/RK3588 提供接入边界和顺序，没有宣称已经完成板级固件或启动。
- NK60 QEMU 用户入口专项使用原版 netduinoplus2，显式跳过 F401 时钟初始化；真实时钟、USB 枚举、电气、DMA 时序与扫描延迟尚无实板验证。
- 本轮是驱动相关验收及原有基线/RV32 回归，没有执行完整 POSIX/glibc/SMP/存储验收矩阵。

首轮记录 `out/microkernel/acceptance/1792810401426332176/results.json` 保留：13/14 分组通过，键盘宿主构建遗漏公共头文件 include 路径导致 1 项编译失败。补齐路径和依赖哈希后，从上述干净实现提交重跑全部 14 分组通过；首轮不能作为通过证据。
