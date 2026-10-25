# ESP32 原生 Flash 持久化

<!-- Copyright (C) 2026 NOS contributors -->

C3/S3 复用公共 NOS VFS 的双 bank 快照文件系统；新增 ESP 专用块适配器，不修改公共文件系统实现。普通应用和 Newlib 文件调用仍进入原生 VFS IPC，只有 VFS 的块后端调用 NOS 设备服务；设备服务内使用原始 IDF `esp_partition_read/write/erase_range`，不绕过设备生命周期和资源仲裁。

## 使用

默认正常固件不会自动挂载或格式化存储。nshell 中执行：

```text
mount /data
mkdir /data/config
write /data/config/note "persistent NOS file"
sync
```

重启后再次 `mount /data`，然后 `cat /data/config/note`。RAM 的 `/tmp` 仍在重启后丢失。快照记录绝对路径，应以相同路径重新挂载。一个实例只允许一个持久挂载，没有卸载命令；挂载失败保留正常 shell 和 RAM 文件系统。

Newlib 可用 `fopen/fwrite/fflush` 后再 `fsync(fileno(file))` 提交；`fflush` 只把 stdio 缓冲提交给 VFS，`fclose`、普通写和改名不会隐式同步 NOR。`nos_sync()`/shell `sync` 提交整个已挂载快照，当前 fsync 也使用这一全局提交语义；RAM 路径不会因此持久化。没有持久挂载时 sync/fsync 保留 ENOTSUP，O_SYNC 仍未实现。

## 分区与映射

| 项目 | 当前值 |
| --- | --- |
| NOS 设备类型 | 追加的 `NOS_SERVICE_BLOCK`，实例0 |
| 设备名称 | VFS 首次挂载时创建 `vfs.storage`，持有独占控制器租约 |
| 默认分区 | storage，data，自定义 subtype 0x40，偏移0x320000，大小0xe0000（896 KiB） |
| 逻辑扇区 | 512字节，每扇区独占一个4096字节NOR擦除块 |
| 逻辑容量 | 默认224扇区，即112 KiB |
| 当前快照使用 | 两个bank，各33逻辑扇区，共66个擦除块，即264 KiB物理Flash |

映射地址为 `partition_offset + logical_sector * 4096`。逻辑字节按位取反存入NOR，因此擦除态0xff自然对应VFS识别的全零空盘，不需要首次挂载时擦除或写入。读写为同步SDK调用，完成后才增加实际扇区计数；SDK错误和部分完成向上返回。块服务不提供任意擦除命令。

每个逻辑扇区独立擦除尤其重要：现有bank长33扇区，并非4KiB对齐。如果将相邻逻辑扇区紧密放入同一NOR擦除块，更新一个bank会连带擦除另一个bank。本实现以空间换取擦除隔离；保留原VFS先写新bank数据、同步、最后写CRC头并同步的顺序。最新bank损坏时回退另一个有效bank，两者均无效且非空时拒绝挂载，不自动清空。

启动只接受指定标签和subtype的分区，拒绝只读、加密、擦除未对齐及容量不足的分区。默认布局与先前固件相比只改变storage的subtype；升级时需要烧录匹配的新分区表。旧SPIFFS分区不会被当成NOS存储自动擦除或迁移。升级前应保存已有板级数据；当前没有迁移/格式化工具，不能将旧SPIFFS直接改标签当成可导入的NOS快照。

## 预算与边界

保留MCU VFS的全局16节点（包含初始系统节点）、每文件512字节、路径64字节及每任务11个FD；空间预算不会因为块设备大而自动扩大。可选 [原始IDF磨损均衡格式](ESP32_WEAR_LEVELING.md) 已接入；FAT、加密、容量扩展、掉电实板试验及Flash寿命尚未验收。本后端适合低频配置保存；每次sync重写一个完整bank，不能用于频繁日志或高吞吐存储。频繁保存应使用显式wl格式；大文件文件系统后端仍待接入。

VFS槽0及init槽1在SDK构造器创建工作任务前均保留，避免工作任务抢占具有挂载权限的init身份。默认仍是单核可信共享固件，没有新增MMU隔离、独立ELF或不可信设备调用边界。

## 验证

`python3 tests/esp32c3.py --timeout 90` 与 `python3 tests/esp32s3.py --timeout 90` 包含：

- 实际NOR设备租约冲突、二进制512字节stdio读写、fflush/fsync和VFS栈余量。
- 七次独立正常固件QEMU启动，共用实际Flash文件：重启导入、rename/append持久化、RAM丢失、未同步写丢弃。
- 离线破坏最新头/数据后回退旧快照，破坏两个bank后拒绝挂载且存储字节不变；检查未使用存储区未改变。
- 宿主实际NOR适配器的ASan/UBSan/LSan故障模型：分区与范围拒绝、擦除/读/部分写失败和独立擦除边界。

这些是实际QEMU NOR操作和明确的离线损坏夹具，不冒称实板断电或Flash耐久验收。冻结报告、正常固件及原始日志见 [ESP32_STORAGE_VALIDATION.md](ESP32_STORAGE_VALIDATION.md)。
