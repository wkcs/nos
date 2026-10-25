# ESP32 磨损均衡存储

<!-- Copyright (C) 2026 NOS contributors -->

C3/S3 现在可显式构建磨损均衡格式。NOS BLOCK 服务持有控制器租约，VFS 和 Newlib 文件接口不变；底层复用仓库固定 IDF 5.4.2 的原始 WL_Flash、Partition 和 CRC 类。没有修改 vendor 源码或另写 Flash 转换算法。NOS 适配层只负责格式选择、生命周期、边界/状态检查和错误隔离。

## 构建与使用

```sh
python3 scripts/esp32c3.py build --storage wl --output out/esp32c3/runtime-wl
python3 scripts/esp32s3.py build --storage wl --output out/esp32s3/runtime-wl
# 下载串口必须明确指定，使用同一输出目录和格式：
python3 scripts/esp32c3.py flash --storage wl --output out/esp32c3/runtime-wl --port PORT
```

Make 入口为 `make PLATFORM=esp32c3 ESP_IDF_STORAGE=wl O=out/esp32c3/runtime-wl firmware`，S3替换平台。构建目录记录格式；禁止在已有raw目录中切换wl，或反向切换。默认 `--storage raw` 保留原始格式。构建manifest记录storage字段，`nshell version` 显示storage_format。

刷入正常固件后仍用 `mount /data`、write/append和sync；Newlib使用fflush后fsync。普通fclose不提交快照。每次复启都重新挂载。全局16节点、每文件512字节和64字节路径预算不变，本批没有扩展为大文件文件系统。

## 两种格式

| 项目 | raw | wl |
| --- | --- | --- |
| storage分区类型 | data subtype0x40 | data subtype0x41 |
| 默认偏移/大小 | 0x320000 / 0xe0000 | 相同 |
| 构建分区表 | partitions.csv | partitions.wl.csv |
| 逻辑扇区 | 512字节独占4KiB物理块 | 512字节独占4KiB虚拟块，由原始WL轮转物理块 |
| 默认逻辑容量 | 224扇区 | 220扇区；另有一个移动备用块及三个元数据块 |
| 擦除态映射 | 反码，0xff读为逻辑零 | 相同 |
| 初始化 | 空盘挂载不写入 | 仅当整个专用分区为0xff时允许原库建立元数据 |

wl使用原库4KiB模式，避免512字节performance模式的整块掉电风险。每16次擦除推进移动备用块，COMMIT调用原库flush完成位置记录。原生VFS仍按先数据、后CRC头的双bank快照顺序提交；各bank虚拟擦除块不共享。原始格式说明见 [ESP32_STORAGE.md](ESP32_STORAGE.md)。

格式不会自动转换或迁移。新模式只查找subtype0x41，拒绝旧表；即使手动改表，raw或SPIFFS的非空内容也会被拒绝，不会自动覆盖。切换前保存原数据并使用匹配的完整烧录布局；回到raw固件也必须使用匹配分区表。当前没有迁移/格式化工具。

## 状态与错误保护

原库默认可能在两个状态副本都无效或版本改变时重新初始化。NOS先读两份状态并校验CRC、版本2、几何和位置边界；任一CRC有效但语义不兼容的副本都会拒绝挂载。两副本都无效时扫描整个分区，只允许全擦除态初始化；非空损坏数据返回EIO且不改写。

一个有效副本允许原库修复另一个。宿主回归发现固定版本的单副本修复分支没有重放RAM中的dummy位置，因此NOS在init成功后再调用原库recoverPos，使第一次读取立即使用恢复后的映射，而不是等下次重启。这是适配层组合原始接口，没有改写或复制vendor恢复逻辑。

原库init还会忽略第一次物理读取返回值。NOS的Partition派生类锁存每次物理错误，禁止后续基于不完整状态写入或擦除。运行时任何读/擦除/写/flush失败也会锁定实例，后续I/O返回EIO；释放时丢弃不确定RAM映射，不暗中flush。当前VFS服务生命周期固定，运行时故障后需要重启以重新校验/恢复存储。分配遵循原库32字节对象对齐，分配失败和底层失败均返回错误。

私有WL类、状态格式和配置依赖固定5.4.2。升级IDF需重新审查配置、状态验证、位置重放和所有回归，不能只改版本号。

## 验证入口

```sh
python3 tests/esp32c3.py --storage raw --timeout 180
python3 tests/esp32s3.py --storage raw --timeout 180
python3 tests/esp32c3.py --storage wl --timeout 180
python3 tests/esp32s3.py --storage wl --timeout 180
python3 tests/host_esp32_wl.py
```

wl来宾增加128次连续快照提交并包裹实际SDK擦除调用计数，要求默认布局的221个数据/备用物理块全部参与轮转，计数不包含三个元数据块。只有回归镜像含计数和压力写入，正常固件不自动写入。宿主执行未经修改的原始WL/Partition/CRC实现及实际NOS保护层，注入NOR/ROM边界模型，以ASan/UBSan/LSan验证600次轮转、重新打开、单副本修复、双副本/未知内容拒绝和I/O错误锁定。

正常固件另经七次独立QEMU启动，复用实际Flash文件验证同步/未同步、单副本修复、双副本损坏和非空未知内容拒绝。离线损坏夹具不是物理断电试验；轮转计数不是Flash寿命认证。最终冻结报告另行补录。
