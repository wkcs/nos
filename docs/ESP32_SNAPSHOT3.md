# ESP32 扩展持久快照

<!-- Copyright (C) 2026 NOS contributors -->

`snapshot3` 是 C3/S3 可选的原生文件系统格式，单个持久文件上限从 512 字节扩展至 8192 字节。默认仍为 `native2`。新格式继续使用 NOS VFS、IPC 和受管块设备，底层 raw NOR 或原始 IDF wear levelling HAL 没有更换。它适合配置和小型资源快照，活动文件仍完整驻留内存；流式大文件、FAT 接入及 O_SYNC 仍待后续实现。

## 编译与刷写

两个芯片均可选择 raw 或 wl，以下为 C3 的 wl 示例；S3 将 `esp32c3` 换为 `esp32s3`：

```sh
python3 scripts/esp32c3.py build --storage wl --filesystem snapshot3 \
    --output out/esp32c3/my-wl-snapshot3
python3 scripts/esp32c3.py flash --storage wl --filesystem snapshot3 \
    --output out/esp32c3/my-wl-snapshot3 --port /dev/ttyACM0
```

Make 入口也支持 `ESP_IDF_FILESYSTEM=snapshot3`，配合 `LIBC=idf`、`ESP_IDF_STORAGE=raw|wl` 和独立的 `O` 目录。省略 filesystem 选项得到原有 native2 固件。构建目录内 `storage-mode.txt`、`filesystem-mode.txt` 记录两种选择，复用目录切换格式会报错；构建清单和 ELF/源码审计同时校验选择。nshell 的 `version` 输出 `filesystem=snapshot3` 或 `filesystem=native2`。

刷写升级时只写工具生成的固件分区，保留 storage 分区；不要执行整片擦除。必须保持相同芯片、分区表和 raw/wl 模式。raw 与 wl 的物理布局不同，不能通过更换 filesystem 选项互相转换。

```text
mount /data
cat /data/config/saved
append /data/config/saved more-data
sync
```

`mount` 只导入旧数据，第一次成功的 `sync` 或持久文件 `fsync` 才提交新版格式。升级前应自行备份重要数据。完成迁移后不支持原地降级到 native2；需要导出内容并在旧格式上重新创建。

## 容量与内存

新格式最多 16 条持久记录，挂载根及所有目录也占记录；路径包含结尾 NUL 最多 64 字节。第 17 条创建在修改命名空间前返回 ENOSPC。单文件最多 8192 字节，到达上限允许短写，后续写返回 ENOSPC；truncate 超过上限返回 EFBIG。

持久数据复用现有 512 字节独立页和 32768 字节扩展预算，与 RAM 文件共享。大于 512 字节的文件按全部数据页计费，非空小文件的首段、表、栈、SDK 堆及分配器元数据另计。因此磁盘可用空间不等于可同时加载的文件总量，物理内存不足可能先返回 ENOMEM。导入前检查节点和扩展预算，分配失败释放本次安装的页和节点，不留下半挂载树，不自动选用更旧但更小的快照。

## 磁盘格式

全部数字为当前 ESP 小端 ABI 的 32 位无符号数，逻辑扇区 512 字节。物理块设备容量为 N，双 bank 的步长为 floor(N/2)，bank 入口为 0 和该步长。要求 N 至少 136 且不超过 UINT32_MAX，保证首次新版提交位于旧版两个 bank 的 0..65 扇区之外。

| 项目 | 布局 |
| --- | --- |
| bank 扇区 0 | 32 字节头，其余填零 |
| 头字段 | magic=0x4e4f5346、version=3、generation、count、bank_sectors、data_sectors、crc、header_crc |
| bank 扇区 1..3 | 16 条紧凑记录，每条 mode、size、name[64]，共 1152 字节，尾部填零 |
| bank 扇区 4 起 | 按记录顺序串接文件内容，各文件向上取整到 512 字节，尾部填零；空文件和目录不占数据扇区 |
| CRC | 全部 3 个元数据扇区和已用数据扇区的 CRC32；header_crc 校验头部前 28 字节 |

raw 当前 N=224、步长 112；wl 当前 N=220、步长 110，分别有 108 和 106 个 bank 数据扇区。实际容量仍受记录和共享 RAM 预算限制。未使用的 bank 尾部不参与 CRC。代次溢出返回 EOVERFLOW，不回绕。

挂载校验两份 CRC、几何、类型/尺寸、规范路径、唯一根、重复路径、父目录完整性、数据扇区数及未用记录/元数据填零，再选择最高代次。数据 I/O 错误保留错误。两份均无效时，只允许整个逻辑设备全零的初始磁盘创建空树；未知非空内容返回 EIO，不自动格式化。

提交逐扇区读取内存页并写入非活动 bank，先写全部元数据和数据并 flush，再写带 CRC 的新头并 flush。成功后才更新活动 bank；fsync 仍为完整挂载的快照提交，并非单文件增量写回。

## 旧格式迁移与中断恢复

无有效新版时可以导入 v2 的两个 bank，兼容旧版稀疏记录布局。首次新版提交固定写 bank 1，不能按代次奇偶选择，因为旧代次可能为奇数。新版头持久化前，旧版 0..65 扇区保持原样。

新版头持久化后，在旧版两个头位置写入非零拒绝标记并 flush，才返回成功。标记同时防止旧固件把新建 v3 磁盘的前半区误判为全空。若标记写入或刷新失败，返回 I/O 错误；同一次运行中重试必须先完成标记，再允许复用 bank 0。重启发现 bank 1 有效而 bank 0 无效时，也在下次 sync 先完成此步骤；挂载本身不写盘。

提交确认前中断允许恢复完整的旧版或新版副本，不能承诺中断后旧固件一定拒绝挂载；开始迁移后应持续使用新版固件。成功确认后旧固件拒绝挂载，不暴露旧数据为当前数据。任一新版 bank 损坏可回退另一份完整副本，两份都坏则拒绝挂载。

## 验证入口与边界

```sh
python3 tests/host_esp32_snapshot3.py
python3 tests/esp32_snapshot3.py --target esp32c3 --storage raw
python3 tests/esp32_snapshot3.py --target esp32s3 --storage wl
```

宿主测试编译真实 VFS，使用 IPC/堆/块设备模型，覆盖首次迁移及后续提交的每个写/flush 中断点、重试顺序、损坏回退、全盘空白检查、语义校验、分配回滚、配额、几何和代次溢出，并启用 ASan/UBSan/LSan。它不能证明硬件掉电持久性。

QEMU 测试构建正常 native2 与 snapshot3 固件，保留原始串口日志、Flash/eFuse 镜像、固件身份和检查结果。每组 10 次独立启动覆盖旧格式两次提交、新版只读导入、4096/8192 字节跨启动读写、成功迁移后拒绝旧固件、记录/文件边界，以及 raw 的头/尾部损坏回退或 wl 元数据修复/拒绝。固件升级夹具只替换可执行分区，逐字节保留 storage。

最终结果另记冻结验收报告。当前没有可访问实板，不声称射频、USB 枚举、物理掉电及 Flash 寿命已经验证。
