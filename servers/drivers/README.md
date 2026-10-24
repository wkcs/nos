# 用户态设备与存储服务

<!-- Copyright (C) 2026 NOS contributors -->

通用设备生命周期、资源 lease、GPIO/SPI/I²C/UART/USB device 接口及 ESP32/RK3588 移植入口见 [设备驱动模型](../../docs/DEVICE_DRIVERS.md)。NK60 已接入 GPIO、SPI NOR 和 USB 发送；本页继续记录原 VFS 设备与存储服务。

`servers/vfs/server.c` 是独立调度的非特权服务。文件业务、描述符表、管道、FatFs 和 virtio 队列均在用户态执行。内核检查 DEVICE 能力、建立设备页映射、查询调用方可写页的 DMA 物理地址、提供 IRQ 对象，以及访问平台串口控制寄存器。只有 VFS 服务获得初始 DEVICE 能力，接收线程继承其地址空间组权限。

## 接口与文件语义

VFS 消息由内核填入 `sender`、地址空间组 `process` 和回复 `token`。服务按 `process` 共享描述符，保留原 `sender/token` 回复；退出通知只有最后一个线程结束才关闭整个文件表。

- 标准 FD 0 是串口输入，1/2 是串口输出，均经过 VFS。默认标准 FD 打开时，新 `open/dup/pipe` 从 FD 3 分配；关闭标准 FD 后复用最低可用号码。
- 独立打开有独立 offset；`dup/dup2` 引用同一 open-file description，共享 offset。`unlink` 移除名字，已打开的文件保留到最后一个引用关闭。
- 路径支持重复 `/`、`.`、`..` 和合法的 `file..txt`；越过 `/dev` 或 `/proc` 的词法路径仍被拒绝。
- `rmdir` 只删除空目录。`rename` 支持 RAM 目录树改名及目标替换，保留打开引用。FatFs 改名目标已存在时返回 `EEXIST`，不承诺 RAM 后端的替换事务语义。
- `READDIR` 的 `arg` 是目录 FD、`value` 是零起始索引，返回 1 和带 NUL 的名字，末尾返回 0。没有排序保证。
- 管道容量 256 字节，写入消息最多 64 字节，单条消息保持原子性。默认空读/满写保存请求并继续处理其他 IPC，在另一端 I/O/关闭时回复；只有 `O_NONBLOCK` 或已完成部分 I/O 的续包返回 `EAGAIN`。没有写端返回 EOF，没有读端返回 `EPIPE`，不伪造尚未实现的 SIGPIPE。当前消息原子性上限仍是 64 字节，不能宣称完整 POSIX PIPE_BUF 支持。
- 目录/文件共 16 个节点，默认 RAM 文件和持久化文件最多 512 字节，默认每地址空间最多 11 个 FD（Linux 兼容配置为 32 个），管道共 2 个。这些是当前明确的 MCU 配置上限，并不是无限容量的 POSIX 文件系统。

## 只读启动归档与 POSIX 元数据

启动归档通过受权限检查的 BOOTFILE 读取接口访问；路径解析、目录枚举、FD 和 offset 仍归用户态 VFS 管理。独立归档 OFD 可读取大于 RAM 节点容量的 ELF、动态加载器与共享库。`pread/pwrite` 不改变 OFD offset，归档写操作返回 `EROFS`。目录枚举使用标准 Linux dirent64 记录，超过单次 IPC 的记录按 cookie 分片，不截断长名字。

RAM 后端支持 create mode、umask、chmod、ftruncate/truncate、硬链接和符号链接。硬链接共享 inode 和文件数据；删除最后一个名字后，已打开引用继续存活，stat 的 nlink 为零。符号链接读出原始目标，普通 open/stat 跟随目标，NOFOLLOW/lstat 保留链接身份，循环返回 ELOOP。FatFs 不支持的权限或链接语义返回 ENOTSUP。默认配置的单路径缓冲区为 64 字节；Linux 兼容配置为 256 字节（含 NUL），两个路径合计可用 512 字节。持久化格式仍使用 63 字节路径，超出时返回 ENAMETOOLONG。

可信 fork 通知复制 FD 表、cwd、umask 和 FD flags，并增加同一 OFD 的引用，所以父子共享 offset。成功 exec 通知只关闭 FD_CLOEXEC 描述符。通知由内核排队，用户不能伪造。POLL 查询不会消费字节，管道按实际数据/空间/端点报告，串口写就绪读取硬件状态；POLLIN/OUT 和 RDNORM/WRNORM 都可查询。

`virtio_rng.c` 从 QEMU virtio-rng 的真实 DMA 队列获取熵，支持 legacy/modern 传输。该驱动当前轮询队列并抑制 IRQ，不宣称 RNG IRQ 已验证。未提供设备返回 ENODEV，不产生固定或时间派生的假随机数。

长路径通过 profile 专用 IPC bulk span 传输，消息内联数据上限仍为 64 字节。调用时内核保存输入/输出边界，VFS 使用 sender/token 申请 FETCH/STORE；内核只允许服务访问仍有效且已送达的当前调用，并重新检查地址空间和范围。VFS 一次取回完整路径副本，不保存跨调用的路径片段。取消、过期 token 和其他线程的请求不能访问旧 span。GETCWD、READLINK 和 RESOLVE_PATH 使用受控输出 span；getdents64 的最长记录为 280 字节，通过现有 cookie 协议分片。

Linux 兼容配置的 RAM 文件容量为 8192 字节，文件增长时才分配共享物理页；默认配置的 512 字节缓存和 24 KiB 模板上限保持不变。`/dev/shm` 是该配置下的可写 RAM 命名空间，命名信号量的临时文件、硬链接发布和 unlink 使用同一套 inode 引用规则。

共享文件映射经 VFS 验证 FD，再由内核创建、授权和映射共享页对象。VFS 文件读写与客户端映射访问同一 backing；客户端只拿一次映射授权 lease，不能指定物理地址。权限上限来自原 FD，内核同时限制 MAP 与 MPROTECT；只读描述符不能升级成写映射。服务释放已 unlink 且无 FD 的节点后，已有客户端映射仍持有 backing 引用。完整的文件截断后页失效及越 EOF 页 SIGBUS 语义尚未实现，不能据此声称完整 POSIX mmap 一致性。

## 延迟 I/O 与串口 IRQ

`pipe_wait.h` 为每个活动任务保留一个有界等待槽，保存完整 sender/token 与 open-file-description 引用。完成时先准备结果，经内核确认 token 和回复复制成功后才消耗/写入管道数据；取消、过期 token 或坏回复地址不会提交数据。内核保留的 `PEER_CANCEL` 通知按任务槽与递增 token 撤销旧请求；退出时撤销该线程请求，最后一个线程退出再关闭文件表。`dup` 共享 `APPEND/NONBLOCK` 状态，`F_GETFL/F_SETFL` 读取和修改实际 OFD。

`uart.c` 在正常启动创建共享地址空间的接收线程，绑定 UART IRQ 并等待中断。接收线程将输入放入 256 字节 SPSC 环形缓冲区，主 VFS 保持响应其他文件请求。原生读在缓冲区为空时返回 `EAGAIN`；C 库依据实际 NONBLOCK 标志决定是否等待重试。IRQ/RX/丢弃计数及 ready 状态可由 DEVICE_STATS(arg=1) 查询。正常串口验证必须同时证明 IRQ 与 RX 计数为正、ready 为 1；降级轮询不算 IRQ 验证通过。自测配置不创建长期驻留接收线程，保留 STM32 任务槽用于能力测试。

## virtio-blk

`virtio_blk.c` 枚举原版 QEMU `virt` MMIO 控制器，检查 offset 8 的 DeviceID，支持 virtio-mmio legacy 1 和 modern 2 协商。描述符、avail、used 和请求数据使用已映射、连续的三个物理页；通过真实 DMA 请求执行读、写和 FLUSH，并校验 used id 和设备状态。设备队列在用户态构造，内核不接收文件或块业务命令。

完成通知使用有生命周期的内核 IRQ 对象：绑定硬件源，等待通知，用户态清设备中断状态后确认并重新使能。每个请求统计真实 IRQ 数；超时后 DMA 已完成的降级轮询另计，验证要求 IRQ 数不少于请求数且降级次数为零。设备错误会返回错误；块设备热插拔/自动恢复尚未实现。只读设备被拒绝；缺少 FLUSH 能力的设备不能提供持久化提交保证。STM32 `netduinoplus2` 没有 virtio 控制器，块操作明确返回 `ENOTSUP`。

设备地址空间组的最后一个线程退出时，内核先重置控制器并确认 DMA 已停止，再释放地址空间及其物理页；单个串口接收线程退出不会重置同组仍使用的块设备。

QEMU 需附加 raw 磁盘：

```sh
-drive if=none,id=nosdisk,file=disk.raw,format=raw \
-device virtio-blk-device,drive=nosdisk
```

## 原生持久化后端

`MOUNT(arg=1, data="/persist\\0")` 加载原生后端。仅 init 地址空间组 1 能挂载/格式化磁盘。原生后端只接受全零空盘或通过校验的原生镜像；损坏、未知文件系统不会被静默当成空盘覆盖。

磁盘最少 66 个 512 字节扇区。两个 bank 各占 33 个扇区：头部 1 个，16 对元数据/文件数据扇区。格式版本 2 有数据 CRC32 和头 CRC32，`SYNC` 先写非活动 bank、FLUSH，再写最后的提交头并再次 FLUSH。加载验证两个 bank 并选择最新有效 generation；最新 bank 损坏时回退前一有效提交。generation 耗尽返回 `EOVERFLOW`。

这提供整个有界文件树的双副本提交；不是日志文件系统，也没有密码学完整性保证。只有显式 `SYNC` 完成后的内容承诺跨启动保留。打开后已 unlink 的文件不纳入快照。

## FatFs 迁移

`servers/fatfs/` 保留仓库 FatFs R0.11a 的许可证和实际实现，使用 ASCII 8.3、单卷、512 字节扇区、tiny 配置。其 diskio 适配调用同一个真实用户态 virtio 后端，旧特权 RTOS 的 diskio 不参与新镜像。

- `MOUNT(arg=2)` 加载已有 FAT 盘。
- `MOUNT(arg=3)` 是显式格式化请求，仅在没有 FAT 文件系统时初始化空白盘，再挂载。
- 文件树导入有界 VFS 缓存。超出 16 节点、63 字节路径或 512 字节文件限制会返回错误，不能静默截断。
- 写入、创建、删除、改名调用真实 FatFs 操作，`SYNC` 执行设备 FLUSH。FatFs 后端遵从 FAT 本身的故障恢复边界，不继承原生双 bank 的整树事务承诺。
- 当前原生/FAT 两个磁盘后端互斥；没有多磁盘或跨文件系统 rename。

## 验证与硬件边界

`servers/vfs/regression.c` 提供正常和错误路径、1000 次资源循环、设备能力检查，以及独立跨启动读回验证。运行：

```sh
source /workspace/.nos-env/activate.sh
python3 tests/qemu_persistence.py --platform arm64_virt --filesystem native
python3 tests/qemu_persistence.py --platform arm64_virt --filesystem fat
python3 tests/qemu_shared_files.py
python3 tests/qemu_long_paths.py
```

`tests/guest/libc_regression.c` 的 `vfs_pipe_blocking_read`、`vfs_pipe_blocking_write`、`vfs_pipe_nonblocking` 和 `vfs_pipe_timeout_cancel_reclaims` 验证线程阻塞唤醒、状态标志与取消后的 OFD 引用回收。完整 QEMU 矩阵将这些标记以及内核取消通知、坏回复缓冲区的检查列为必需项。存储开发验收记录在 `out/microkernel/storage-irq-results.json`，包含 13 项、26 次真实启动；最终集成状态以完整验收报告为准。

UART 输入输出和存储必须通过真实 QEMU 启动验证；宿主替换传输的回归不能替代设备验证。USB、真实 STM32 SD/LCD 控制器、真实开发板的时钟/引脚/电气行为尚未验证；不能将无模型的外设称为已迁移硬件。Lua/LVGL 的独立应用与软件显示迁移由 `apps/` 承载。
