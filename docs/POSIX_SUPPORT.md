# C 库与 POSIX 支持范围

<!-- Copyright (C) 2026 NOS contributors -->

本表描述当前端口的接口范围；是否已验证以本次 QEMU 报告为准。NOS 不提供完整 Linux syscall ABI 或完整 POSIX，musl 的系统调用号在适配层转换成 NOS ABI。不能把链接到真实 libc 等同于其全部功能可用。

## 库与平台

| 组合 | 范围 |
|---|---|
| ARM Cortex-A15 / STM32F405 + Newlib | 真实 Newlib 4.5.0 动态重入、retargetable locks，NOS pthread 端口 |
| ARM64 / RV64GC + musl | 真实 musl 1.2.5 静态库、架构 syscall/clone/退出适配、真实 musl pthread |
| RV32 Sv32 / RV32 非 MMU | minimal 原生 ABI；尚未移植/验证 RV32 Newlib 或 musl |
| 库存 ARM Newlib | `NEWLIB_MODE=stock` 诊断回退；不作为共享地址空间 SMP 重入安全结论 |
| glibc / 动态加载 | 未移植；没有 Linux 进程/信号/动态链接接口的完整兼容承诺 |

源码下载经 Debian 签名元数据及固定 SHA256 校验，输出在忽略的构建目录。`scripts/build_newlib.py` 对目标 CPU、编译器、configure 与源码身份建立缓存；`scripts/build_musl.py` 为各架构构建实际静态库。

## 接口矩阵

| 接口 | 当前支持 | 主要限制 |
|---|---|---|
| malloc/calloc/realloc/free | 两种真实库 | 受地址空间、VMA 数量与物理页配额限制；不使用内核 malloc 代替 libc 分配器 |
| printf/浮点/stdio 文件 | 支持声明子集 | 文件容量有界；FILE 锁与分配器重入必须在目标 SMP 配置单独验证 |
| errno/TLS | Newlib 每线程 `_reent`；musl 线程指针/TLS | Newlib 库重入与通用 C11 `_Thread_local` 的编译器 TLS ABI 是不同能力；独立 ELF 的真实 C TLS 测试使用 musl |
| open/read/write/close/lseek/fstat | 支持常用标志与字段 | 非法缓冲区返回 EFAULT；零长度仍检查 FD；stat 提供 mode/size 等有限字段，不提供完整权限/用户身份模型 |
| dup/dup2/fcntl 标志 | 支持 FD 共享描述与重定向 | FD 表有界；不支持完整 fcntl/文件锁集合 |
| pipe | 有界管道、读写端生命周期 | 256 字节缓冲，64 字节消息原子写；阻塞/非阻塞、超时/取消及错误回复缓冲区有专项回归；没有 SIGPIPE，断读端返回 EPIPE |
| mkdir/rmdir/unlink/rename | 支持端口包装与 NOS VFS | RAM 与 FAT 的 rename 替换语义不同；没有 hardlink/symlink/chmod/完整 pathname API |
| 目录遍历 | NOS `nos_readdir` | 不代表两种 libc 的完整 opendir/readdir/dirent 接口已适配 |
| fsync/sync | 持久化后端和设备 FLUSH | RAM 后端不提供跨重启保证；原生双 bank 与 FAT 的事务承诺不同 |
| mmap/munmap/mprotect | MMU 匿名映射、范围操作、PROT_NONE | 无文件映射、MAP_SHARED、MAP_FIXED 或任意地址布局；不允许 W+X；非 MMU mprotect 返回 ENOTSUP |
| clock_gettime/nanosleep | CLOCK_MONOTONIC、100 Hz 时间粒度 | 使用 64 位 tick 快照，支持相对及单调绝对睡眠；没有初始化的日历时钟；CLOCK_REALTIME 等未实现操作返回 ENOTSUP |
| getpid/gettid | 地址空间组 ID / 任务 ID | 不冒充 Linux PID namespace；内核句柄带 generation |
| pthread_create/join/exit | Newlib NOS 端口与 musl 端口 | 上限受可用任务槽和栈/映射配额约束；musl 默认栈为嵌入式 16 KiB 配置，guard 4 KiB |
| pthread mutex/condvar | 普通/递归锁、等待/信号/广播、有限 timedwait | 受支持时钟与线程 API 范围限制；不支持完整 robust/process-shared/优先级协议组合 |
| pthread key/once | TLS key、析构、once | 使用有限表；不能由此推断支持线程取消 |
| pthread_cancel/detach/信号 | 未完整支持 | Newlib 的 cancel/detach 返回 ENOTSUP；musl detach 具有退出栈回收桥接，但未列为本次专项验证；cancel 返回 ENOTSUP，不能把未实现的信号取消当成成功；仅允许运行库启动所需的有限信号掩码路径 |
| fork/execve/共享库/网络 | 未支持 | NOS 提供受权的 initramfs ELF spawn；不是 POSIX execve 或 fork |

## 退出与并发契约

正常独立 C 应用从 main 返回后经 libc exit 路径处理 stdio/atexit；最小应用使用原生退出入口。进程组退出与单线程退出分开，clear_tid/futex 唤醒和地址空间引用回收由内核负责。

Newlib 使用 `TLS_GET` 获取任务本地 `_reent`，通过独立绑定槽避免把共享 `_impure_ptr` 当作跨核 TLS。分配器锁首次发布采用同步协议，stdio 使用 retargetable locks。线程退出清理自己的 `_dtoa` 等重入资源，不能误关闭进程共享标准流。持有分配器锁的任务异常退出时，分配器不能假设堆仍完整；端口记录失效并拒绝继续使用。owner-death 失效注入和 dtoa 回收前后精确堆计数尚无专项数值验收，不能由正常多线程测试推断通过。

用户缓冲区检查与复制走内核接口，避免直接解引用错误指针使调用任务退出。检查后其他线程仍可并发改变地址空间，因此传输必须处理真正复制失败，而不是只依赖早期检查。VFS 的延迟管道回复使用 token，取消或 EFAULT 不能错误消费/注入数据或永久占用 open description。

futex 比较使用对齐的 32 位原子读，保留负锁字位模式；WAIT 可读只读映射，clear_tid 仍需可写。REQUEUE/CMP_REQUEUE 在同一内核临界区处理源比较、唤醒与移动等待者，测试包含两等待者换队及三个真实 musl 条件变量等待者广播。TLS 绑定槽被解除映射或撤销写权限时会解绑；TCB/TLS 数据本身的生命周期由应用维护。

SMP、MPU、非 MMU 和不同 C 库必须分别执行对应验证。某个最小库或单核测试通过，不能代替另一种 C 库的跨核结果。
