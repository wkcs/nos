# ESP32 Newlib 文件与标准流适配

<!-- Copyright (C) 2026 NOS contributors -->

C3 与 S3 使用固定 ESP-IDF 5.4.2 工具链的原始 Newlib。NOS 在 `libc/port/esp32/` 实现 OS 钩子，`ports/esp32/components/nos_libc/` 只负责组件集成与链接包装。调度、文件表、cwd 与文件操作归 NOS；没有修改 vendor Newlib 或新增 SDK 调度器。

## 文件路径与描述符

设备服务和控制台初始化完成后激活适配，普通任务的 `open/read/write/lseek/stat` 通过 NOS VFS IPC。`fopen/fprintf/fscanf/fseek/fflush/freopen/fclose` 使用原始 Newlib 的解析、格式化和缓冲实现，底层进入相同 NOS 文件表。`fileno()` 返回原生描述符，可与 `nos_read()` 等接口互操作。

| 描述符 | 所属与行为 |
| --- | --- |
| 0–2 | 当前 NOS 任务的标准输入、输出和错误，默认连接受管理的 UART0 控制台 |
| 3–10 | 当前 NOS 任务的普通文件、目录或管道 |
| 11–63 | SDK 全局设备/网络描述符；已有描述符的 I/O 交还原始 SDK，实际 socket 区间由 lwIP 配置决定 |

NOS 在 SDK stdio 初始化之前永久保留 3–10，防止 SDK 分配重复编号；编译期检查 socket 区间不重叠。普通应用的路径打开只查询 NOS，不在 ENOENT 时偷偷退回 SDK 路径。SDK 自身的原始 VFS 注册/打开接口仍可供兼容层使用，回归用一个真实注册的 SDK 文件系统验证与 NOS FD3 并存。SDK 全局描述符不获得 NOS 每任务归属或自动回收语义。

支持 `mkdir/rmdir/chdir/getcwd`、`opendir/readdir/readdir_r/closedir/telldir/seekdir/rewinddir`、`unlink/rename/link`、`dup/dup2/pipe`、`pread/pwrite/ftruncate/truncate`、`isatty`，以及 `fcntl` 的复制、FD_CLOEXEC、访问/append/nonblock 标志。底层不支持的功能保留错误返回；没有持久挂载时 `fsync` 返回 ENOTSUP；挂载 [原生Flash快照](ESP32_STORAGE.md) 后 fsync 提交整个快照，不会持久化RAM路径。O_SYNC 与记录锁也不伪造成功。Newlib 与 NOS 的 open 标志、errno、poll 位布局分别转换。

`poll` 支持至多 FD_SETSIZE 项，合并 NOS 就绪快照和 SDK select 回调；SDK 设备必须自身实现 select。`select` 支持 NOS 与 SDK 混合集合，纯 SDK 集合交还原实现。等待粒度为一个 100Hz tick。当前不是完整 POSIX.1-2017 实现或认证。

## 标准流与退出

激活后进入的兼容任务分配私有 stdin/stdout/stderr FILE 对象，避免原始全局 FILE 缓冲、关闭与重定向互相影响。每任务最多 14 个受跟踪 FILE 对象（含三个标准流和 fdopen 别名），底层仍只有 11 个 FD；目录对象另计但消耗同一 FD 表。NOS PID 包含复用代次，跨任务 fclose/freopen/目录操作拒绝 EBADF。FILE 注册表在短临界区内发布和移除，不在临界区内分配内存或调用 VFS。

任务正常返回和自我 vTaskDelete 在原任务身份有效时刷新并关闭其原生 FILE 与目录。原生 EXIT、外部强制删除走 orphan 回收：丢弃尚未提交的缓冲，释放 FILE/DIR 元数据，由 VFS 的进程退出通知关闭原 FD，避免 idle 或删除者误操作自己的同编号文件。回收后重置 Newlib reent 的标准流指针，再执行 SDK reent 清理。通过反复复用任务槽和精确 heap 比较验证该路径。

启动早期、VFS 自身及 idle 的内部 I/O 保留 SDK 路径，不能向 VFS 自身发 RPC。激活前已经运行的内部任务可保留原始标准流；SDK 范围的 FILE 对象保留 SDK 生命周期。此适配不提供 pthread 进程组、完整 exit/atexit 或强制取消任意库临界区的保证。

## 驱动日志与锁顺序

设备后端持有 NOS 设备域的递归锁时，若其日志再经 VFS 等待同一设备域，会形成跨任务死锁。因此该上下文的 stdout/stderr 写入在当前所有者任务中直接调用受管理的 system.console，保留短写/EAGAIN 和用户缓冲校验。这类诊断输出固定进入控制台；普通任务的重定向与 I/O 仍走 VFS。该上下文的其他 NOS 文件/目录操作返回 EDEADLK。不得在设备域锁内新增阻塞的文件 RPC。

## 验证与剩余工作

运行 `python3 tests/esp32c3.py --timeout 90` 与 `python3 tests/esp32s3.py --timeout 90`。每个平台构建普通、运行时回归、完整生态回归镜像；检查真实编译/ELF 符号、Newlib 文件/stdio、poll/select ABI、SDK FD 并存、任务所有权与退出回收，以及私有标准流启用后的 16/24 个实际任务槽容量。普通固件启动输出 `NOS: Newlib filesystem online`，执行 32 项真实 UART nshell 交互。

RAM 文件之外已接入 [原生Flash持久化](ESP32_STORAGE.md)，仍待磨损均衡/FAT、分配器统一、独立 ELF 与 NLP 服务绑定；ESP32 仍是单核共享固件地址空间。本适配不扩大 RF、USB 或实板验证范围。冻结验证结果、原始日志和正常固件见 [ESP32_NEWLIB_VALIDATION.md](ESP32_NEWLIB_VALIDATION.md)；历史公共 89 组基线见 [ESP32_VFS_VALIDATION.md](ESP32_VFS_VALIDATION.md)。
