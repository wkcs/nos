# ESP32 后续开发顺序

<!-- Copyright (C) 2026 NOS contributors -->

NOS已接管C3/S3启动、单核任务调度、设备生命周期、原生VFS及Newlib文件接口；Flash双bank快照和可选磨损均衡格式也已接入。下面仍需继续，不能将现有回归等同于完整POSIX或完整ESP-IDF生态。

| 优先级 | 待完善内容 | 可验收目标 |
| --- | --- | --- |
| 0 | 时序可靠性与WL压力诊断 | 7812b03修正暂停tick/等待链表语义、异步清理同步及UART待处理中断卸载；四组与四轮并行共48次启动通过。旧随机失败子项不能追溯，开发态WL宿主预算超时保留；3ab51e7的S3 WL首轮压力提交180秒超时，同固件同预算单独复验通过但原因未定；优先补逐提交耗时及宿主CPU/I/O采样，继续持续负载诊断，见[本轮验收](ESP32_SNAPSHOT3_VALIDATION.md)及[时序验收](ESP32_TIMING_VALIDATION.md) |
| 1 | 持久文件系统容量与接口 | addc280完成RAM32节点/8KiB文件、按需分段/32KiB扩展预算及按FD选择fsync，公共89/89与ESP40次启动通过，见[验收](ESP32_RAMFS_VALIDATION.md)。可选[snapshot3](ESP32_SNAPSHOT3.md)实现16记录/8KiB持久文件、v2只读导入/首次sync迁移和降级拒绝；3ab51e7冻结公共89/89与八组80次QEMU启动通过，见[验收](ESP32_SNAPSHOT3_VALIDATION.md)。默认native2保持512字节。继续实现FAT或其他流式大文件后端及O_SYNC |
| 2 | 分配器与内存统计 | NOS页池与IDF capability heap统一分配入口/预算和故障回滚，保留DMA/内部RAM/IRAM约束，避免两个分配器重复管理物理内存 |
| 3 | NLP业务绑定 | 把UART、USB及网络传输接入NOS能力发现、日志、nshell和OTA，验证帧边界、断连恢复、权限与升级激活策略 |
| 4 | 驱动异步能力 | GPIO IRQ订阅、UART/SPI/I2C DMA、事件与取消、每控制器并行队列；继续复用原始HAL并经NOS设备模型管理 |
| 5 | 网络服务 | 地址获取/事件、TCP/UDP与NOS服务/描述符生命周期、超时与错误路径；实板Wi-Fi/BLE/GATT互通 |
| 6 | 独立ELF | 先完成分配器与资源所有权，再接入Xtensa/RV32重定位、TLS、装载失败回滚和进程退出资源回收；明确无MMU共享地址空间边界 |
| 7 | 低功耗/可靠性 | NOS调度器配合tickless、看门狗、复位原因与崩溃转储，实测Flash掉电恢复和寿命、USB/OTG/外接总线错误恢复 |
| 8 | S3多核 | 双核上下文、跨核唤醒、SDK ABI和锁顺序；C3物理单核，不扩大其硬件能力 |

磨损均衡当前为显式选项，不迁移旧数据。固件只能证明软件逻辑和模拟器能力；目前没有可访问实板，板级功耗、DMA、射频、USB枚举和真正掉电测试仍需硬件验证。实现与边界见 [ESP32_WEAR_LEVELING.md](ESP32_WEAR_LEVELING.md)、[ESP32_ARCHITECTURE.md](ESP32_ARCHITECTURE.md)。

下一项功能开发优先处理不完整驻留RAM的流式大文件后端；时序诊断与负载观察作为持续回归保留。
