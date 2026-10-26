# ESP32 原生 NOS VFS

<!-- Copyright (C) 2026 NOS contributors -->

C3/S3 固件现在创建独立的 `vfs` 任务，运行现有 `servers/vfs/server.c`。`init`、nshell 和兼容任务通过 NOS 的端点0、系统调用及用户复制接口访问文件服务。不是将 SDK 文件系统重命名为 NOS，也没有复制另一份 RAM 文件系统实现。

## 启动与任务

任务槽0在硬件库构造器创建任务之前就保留给VFS。内核启动钩子创建VFS及init，随后启用保留IPC端点；后续兼容任务获得CALL权限，不能RECV该端点。VFS拥有启动归档访问所需的DEVICE能力，普通兼容任务不因此获得该能力。

各任务使用其原生进程身份对应的文件表和cwd。原生EXIT会通知VFS回收文件表，NOS idle随后回收兼容任务的栈/TLS；这些任务仍处于无硬件隔离的共享固件地址空间。不能把文件表独立理解为内存隔离。VFS栈为12288字节，回归检查实际剩余栈至少512字节。服务重启和ELF进程监督尚未接入该profile。

ESP版VFS的节点表、打开文件描述和进程文件表在服务启动时，从平台提供的内部capability heap分配并清零；任一分配失败回滚并退出服务，不报告就绪。这样可使用SoC分散的内部RAM，不把全部表压进链接器单一BSS区域。内存生命周期为固件服务，不提供卸载/重启。其他平台继续使用原有静态表。这是VFS存储后端选择，尚不代表内核页池和硬件heap已统一。

## 文件与终端

初始命名空间包含 `/`、`/tmp`、`/dev`、`/proc`、`/dev/console` 和只读的 `/proc/version`。支持RAM文件、目录、读写/定位、dup共享文件偏移、rename、unlink后继续访问已打开文件、管道及cwd。ESP现为32节点、RAM文件最多8192字节、额外文件缓冲共享32KiB预算；每任务11个描述符和64字节路径保持不变。持久版本2仍为16记录/512字节文件，详见 [容量与按文件同步](ESP32_RAMFS.md)。资源耗尽返回实际错误，RAM内容复位即丢失。

nshell命令为 `pwd cd ls cat write append touch mkdir rm rmdir mv stat`，另保留诊断及编辑命令。例如：

```text
mkdir /tmp/demo
cd /tmp/demo
write note "NOS file"
append note " through IPC"
cat note
mv note renamed
ls .
rm renamed
cd /
rmdir /tmp/demo
```

shell的标准输入/输出也经过VFS的READ/WRITE RPC；后端再调用NOS受管理的 `system.console`，复用原始UART驱动。没有额外的RX驱动或第二个接收任务。`NOS_IO_POLL` 从原始驱动RX长度和HAL TX FIFO空间读取就绪快照，不消费输入；传输仍处理实际短写/EAGAIN。

原生 `nos_open/nos_read/nos_write` 与 [Newlib 文件/stdio 适配](ESP32_NEWLIB.md) 共用每任务文件表。`fopen()`、目录与 poll/select 已接入；SDK 全局设备/网络描述符保留独立区间和原始生命周期，不代表任意 SDK 文件系统或完整 POSIX 已接入。[专用Flash块设备和原生双bank持久化](ESP32_STORAGE.md) 已接入，`mount /data` 和 `sync` 可用；FAT挂载和熵服务仍返回ENOTSUP，`run` 仍明确报不支持。已有启动归档读取协议可用，但当前归档为空，没有独立应用ELF。

## 验证入口

`python3 tests/esp32c3.py`、`python3 tests/esp32s3.py`各构建正常/运行时/生态镜像，检查原生VFS实际编译和链接，并在官方QEMU运行。正常镜像执行32项UART交互，包含文件创建、相对cwd、读写/追加、目录、rename、unlink、只读拒绝和错误恢复。回归镜像还验证dup、已删除文件、管道EOF/EPIPE、FD耗尽、8KiB RAM边界和32KiB预算回收、端点接收拒绝及超过进程槽数的任务退出回收。

公共IPC/VFS改动还需 `python3 tests/acceptance.py --include-posix --require-frozen-source`。宿主模型与模拟器测试不替代实板无线、USB或物理存储验证。

最终冻结实现 `bfd67ab` 已通过两平台八个实际镜像和公共89/89组验收，原始报告、日志和正常固件见 [ESP32_VFS_VALIDATION.md](ESP32_VFS_VALIDATION.md)。
