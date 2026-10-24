# USB host / OTG 验证记录

<!-- Copyright (C) 2026 NOS contributors -->

日期：2026-10-24。设计、API 和硬件边界见 [USB host 与双角色框架](USB_HOST_OTG.md)。

被测实现提交：`95b4fb934ee5b7c868d00b9a67d047b778f2e635`。测试前后工作区干净，源码身份无变化；本报告在验收结束后单独添加，不属于被测实现提交。

源码身份对象（排序 JSON）SHA-256：`9a382ca70cee48cf3621ed3da3a0290a00a1231d4cb9272818d9cdcb282000e1`。

```sh
source /workspace/.nos-env/activate.sh
python3 tests/acceptance.py --section boards --section baseline --section rv32 --require-frozen-source
```

**16/16 分组通过，全部退出码 0。** 记录：`out/microkernel/acceptance/1792812761279060050/results.json`。输出目录保存在当前工作区，不随 Git 上传。

| 分组 | 结果 |
|---|---|
| eight_platform_libc_combinations | PASS |
| usb_host_otg_sanitizers | PASS |
| usb_host_actual_dwc2_qemu | PASS |
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

原有 ARM/ARM64/RV64/STM32 的 8 个平台/C 库组合以及 RV32 MMU、无 MMU 组合均在真实 QEMU 来宾中执行；新增 `usb_host_descriptor_and_otg_userspace_roles` 是必需通过标记。它覆盖实际目标用户态的配置描述符验证、ID/VBUS 双角色与故障回滚，不替代平台 USB 控制器硬件测试。

## 实际 C HCD 与 QEMU USB 外设

记录：`out/microkernel/qemu-usb-host/1792812846446792177/results.json`。

QEMU：`/workspace/.nos-env/root/usr/bin/qemu-system-aarch64`；SHA-256：`2e7fe3008a900980493a85e606036793fd162153edac0228477579988c53e1f5`。

被测 C 可执行文件 SHA-256：`f9db68e7b9289fa33c9760a23ee04822029fc49500918ece1817d3f5fe66e01a`。

| Profile | 行为 | 结果 |
|---|---|---|
| keyboard | HID 枚举、输入注入、旧句柄失效、锁灯 | PASS |
| keyboard_fs | HID 枚举、输入注入、旧句柄失效、锁灯 | PASS |
| mouse | HID 枚举、输入注入、旧句柄失效 | PASS |
| storage | BOT/SCSI 容量、读、写、sync、raw 扇区核对和热拔插 | PASS |

实际 NOS C host/core、HID/BOT 与 DWC2 DMA 后端运行在宿主 sanitizer 进程，通过 qtest 访问原版 QEMU raspi3b 的真实 DWC2 寄存器模型和 RAM DMA；root port 1 显式直连，未插入 hub。QMP 注入输入并卸载 USB device，存储测试只写临时 1 MiB raw 镜像，退出后确认第 4 扇区内容。四个 C 进程均正常退出，执行 ASan/UBSan/LSan 检查。

QEMU CPU 仅运行 A64 WFI/循环测试载荷；这不是 NOS 已支持 Raspberry Pi 启动或实板 OTG 时序验证。qtest DMA 读写也不验证真实 cache/PHY/信号完整性。

## 错误路径与资源

记录：`out/microkernel/host-usb-host/1792812843840771823/results.json`。

- 实际 core 枚举/控制 PID、短包、部分 OUT 成功、interrupt interval、坏 CSW 与 BOT reset；失败写命令不会自动重放。
- 50000 个恶意配置验证无 sanitizer 错误，错误解析不提交元数据；拔插/重新枚举后旧 generation 被拒绝。
- 引用计数饱和及 partial start + stop 失败时仍保持设备所有权。
- DWC2 NAK/STALL/停钟、低速根端口/PHY 时钟、小 FIFO 限额、无法 halt/reset 后隔离；确认停止前不释放 DMA 映射。
- OTG 不反灌 VBUS、放电等待、VBUS 上电超时、过流、start/stop 失败、时间回绕与显式恢复。
- 4 个生产 C 源文件通过 GCC `-Wall -Wextra -Werror -fanalyzer` 编译检查。

## NK60 兼容与内存预算

记录：`out/microkernel/nk60-firmware/1792812964247138982/results.json`。

| MPU | bin 字节 | 用户静态区 / 16384 | 静态区末尾 | MSP 预留字节 | 核心 load/save 栈帧合计 |
|---|---:|---:|---|---:|---|
| 0 | 96636 | 16112 | 0x2000bef0 | 16656 | 2528 / 2584 |
| 1 | 97620 | 16112 | 0x2000bef0 | 16656 | 2528 / 2584 |

MPU=0 ELF SHA-256：`fd0dfb819a0f6048f3c833414540d6125dc03361f3920efac9d3c3e8d6f6b065`。

MPU=1 ELF SHA-256：`9301c2cfca5a1a69fbca0bbfa90e5a518e413c5b6fecc9064d132e191284c644`。

未实例化的 host/OTG 模块由链接器回收，NK60 device-only 固件的 bin/用户静态内存预算与上一阶段一致，用户静态区仍余 272 字节。已知核心帧合计不覆盖所有间接回调或高水位。现有 HID/NLP、键盘灯效、SPI NOR 和明确跳过 F401 时钟初始化的 Cortex-M QEMU 入口专项全部通过。

## 尚未验证或未实现

- NK60 没有可访问实板且 VBUS source/sense/ID/CC 未确认，保持 device-only；没有扩展 MMIO 白名单或 syscall 权限。
- 物理 host/OTG、VBUS 电源和过流电路、cache/真实 DMA、PHY 和 USB 电气均待实板。软件 role policy 不替代硬件过流保护。
- 本轮 DWC2 只提供内部 DMA 模式，PIO、hub/TT、多设备拓扑、ISO、高带宽多事务、USB3/DWC3/xHCI、异步 URB 未实现。
- 双角色是 ID/VBUS/manual DRD policy；HNP/SRP/ADP、USB-PD/Type-C CC 协商未实现。
- HID 限 boot 键鼠；MSC 限 BOT/SCSI LUN0 和 READ/WRITE(10)，没有自动挂载 VFS 或完整 SCSI/UAS。
- 本轮执行驱动相关/基线/RV32 验收，没有执行完整 POSIX/glibc/SMP/存储验收矩阵。
