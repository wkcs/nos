# C 库与 POSIX 支持范围

<!-- Copyright (C) 2026 NOS contributors -->

本表描述当前端口的接口范围；是否已验证以对应 QEMU 报告为准。正在实现的兼容目标是 **POSIX.1-2017，ARM、ARM64 和 RV64 MMU 同步推进**。完整 POSIX 尚未完成。native ABI 与 Linux personality 是两条不同的调用路径，不能从 glibc 程序通过推断 Newlib/musl 已获得相同的进程、信号和取消接口。逐接口族的缺口见 [POSIX.1-2017 接口分类](POSIX_2017_INTERFACES.md)。

## 库与平台

| 组合 | 范围 |
|---|---|
| ARM Cortex-A15 / STM32F405 + Newlib | 真实 Newlib 4.5.0 动态重入、retargetable locks，NOS pthread 端口 |
| ARM64 / RV64GC + musl | 真实 musl 1.2.5 静态库、架构 syscall/clone/退出适配、真实 musl pthread |
| RV32 Sv32 / RV32 非 MMU | minimal 原生 ABI；尚未移植/验证 RV32 Newlib 或 musl |
| 库存 ARM Newlib | `NEWLIB_MODE=stock` 诊断回退；不作为共享地址空间 SMP 重入安全结论 |
| 三种 MMU 架构 + glibc | `LINUX_COMPAT=1`，真实 Debian glibc 2.41 与各架构 ld.so；ARM 为 GNU soft-float ABI |

源码下载经 Debian 签名元数据及固定 SHA256 校验，输出在忽略的构建目录。`scripts/build_newlib.py` 对目标 CPU、编译器、configure 与源码身份建立缓存；`scripts/build_musl.py` 为各架构构建实际静态库。

glibc 工具与库由 `scripts/setup_glibc_env.sh` 安装，`/workspace/.nos-env/glibc/manifest.json` 记录编译器、未修改 DSO 和精确源码身份。动态重定位、依赖库、构造器和 dlopen 由真正的用户态 ld.so 完成。内核提供虚拟内存、任务、TLS、futex 和信号机制；文件调用进入独立链接的用户态 gateway，再经 IPC 调用 VFS。文件系统业务没有移入特权内核。

## Linux personality 与真实 glibc

| 接口族 | 已实现与开发验证 | 仍需完善的边界 |
|---|---|---|
| 动态加载 | ET_DYN/PT_INTERP、auxv、真实随机 AT_RANDOM、DT_NEEDED、构造器、dlopen/dlsym/dlclose | 不是全部 Linux ABI；启动成功不能代替其他接口验收 |
| C 库与线程 | malloc/stdio/errno/C TLS，真实 NPTL create/join、detach/TSD、robust owner death、延迟/异步取消 | 其他属性、优先级协议和全部取消点需逐项验收 |
| 进程与信号 | 独立地址空间 fork、同 PID exec、共享 OFD/cwd/umask 与 CLOEXEC、waitpid 状态；真实处理器帧、sigreturn、mask/pending、altstack、RT FIFO、SIGPIPE、EINTR 与部分 SA_RESTART | 完整 wait 选项/rusage、完整会话和作业控制等仍有缺口 |
| 身份与权限（已验收子集） | 实际 real/effective/saved UID/GID、32 补充组；线程共享、fork 独立；成功 exec 保存 ID 更新为有效 ID，失败保留旧凭据，auxv 返回实际身份；可信 CALL subject，RAM DAC/access/chown、sticky 与目录 setgid；宿主 sanitizers、三 MMU 单核/四核真实 glibc 及完整 68 组验收通过 | RV64 无 errno 身份 getter 使用 psABI 规范 32 位寄存器返回，原始 syscall() 的高 UID 结果存在 errno 区间歧义；ARM UID16、Linux per-thread fsuid/fsgid、set-ID ELF、regular-file set-ID mode 未支持；持久挂载仅 root 可新解析/打开，时间仅秒级 |
| 资源限额（已验收子集） | 进程共享 RLIMIT_NOFILE、ARM ugetrlimit 与实际 libc 查询一致；VFS 以 token 绑定的实时限额限制新 FD 分配，保留已打开高 FD；三 MMU 单核/四核及完整 68 组验收通过 | 结构上限仍为 32 FD；STACK 仅查询/相同值，其他限额、64 线程/25 进程最小容量未实现 |
| 文件与目录 | stat64、pread/pwrite、getdents64、标准目录接口、常用 *at、RAM mode/umask/truncate/hardlink/symlink；兼容配置 256 字节路径及受 token 约束的双路径传输 | 纳秒时间更新、持久 inode 身份、资源最小限值和跨后端完整语义仍需完善；持久后端仍有 64 字节路径限制 |
| 进程记录锁 | Linux profile 的 F_GETLK/F_SETLK/F_SETLKW，按 inode + 进程持有；区间拆分合并、阻塞、EINTR/SA_RESTART、EDEADLK、取消与退出清理，ARM old/wide flock 转换 | 64 个有界锁区间，容量不足返回 ENOLCK；默认 native 配置明确 ENOTSUP，其他 fcntl 扩展未齐 |
| 管道与向量 I/O | Linux profile 实装 4096 字节 PIPE_BUF/缓冲与 1024 项 iovec；不超过 PIPE_BUF 的写入原子性、较大请求的非阻塞前缀、取消后不消费/注入，部分写入后仍投递 SIGPIPE；本批三架构单核/四核及完整冻结验收通过 | 有界 2 条管道及 pending 队列；默认 native 容量单列，未声明完整 FIFO/TTY |
| 多路 I/O | 真实 pipe/文件/UART readiness，poll/select、ppoll/pselect 临时信号 mask、超时/EOF/错误 FD | 使用有界 readiness 查询与等待；不是完整事件设备框架 |
| 时间 | 实际 RTC 的 REALTIME、64 位 MONOTONIC、睡眠/绝对等待、ITIMER_REAL 信号 | 100 Hz 粒度；CPU 时钟及其他 timer 类型尚不完整 |
| 虚拟内存 | 稀疏 PROT_NONE、事务式提交与回滚、MAP_FIXED/NOREPLACE、私有文件装载、fork 复制、真实 RAM 共享页与每次映射授权、W^X | 持久文件映射、按需缺页、文件截断后的 SIGBUS、交换仍未完成；私有 fork 是复制而非 COW |
| 网络和终端 | UART 与有界管道服务 | socket 协议栈、完整 TTY/PTY、termios 和控制终端尚未完成 |

`tests/qemu_glibc.py` 对三架构分别检查实际 interpreter、库与镜像摘要、严格来宾标记、RTC 宿主时间窗和 QEMU 退出状态；SMP 另检查真实 CPU 掩码。`tests/qemu_posix_suite.py` 执行固定上游提交、未修改断言的 OpenPOSIX 选定用例，分别记录失败、未解决、不支持、未执行与超时。选定用例通过不等于完整 POSIX 符合性。

当前仍有低于完整 POSIX 要求的资源和语义：任务/线程容量及所有线程/进程最小限值尚未满足，凭据/权限子集已通过冻结验收，全部资源限制尚未实现。Linux profile 已有有界进程记录锁；本批将管道原子写与 glibc 报告的 PIPE_BUF4096、IOV_MAX1024 对齐，通过新冻结验收 `1792365892261153665`。默认 native 仍为 256 字节管道、64 字节消息原子写，未达到 POSIX 最小 PIPE_BUF。已有测试成功不能消除这些缺口。glibc 还可能报告其构建时面向 Linux 的 option/limit，需要逐项与 NOS 实际执行能力对齐，不能将其常量视为已经满足的规范保证。

## native Newlib / musl 接口矩阵

| 接口 | 当前支持 | 主要限制 |
|---|---|---|
| malloc/calloc/realloc/free | 两种真实库 | 受地址空间、VMA 数量与物理页配额限制；不使用内核 malloc 代替 libc 分配器 |
| printf/浮点/stdio 文件 | 支持声明子集 | 文件容量有界；FILE 锁与分配器重入必须在目标 SMP 配置单独验证 |
| errno/TLS | Newlib 每线程 `_reent`；musl 线程指针/TLS | Newlib 库重入与通用 C11 `_Thread_local` 的编译器 TLS ABI 是不同能力；独立 ELF 的真实 C TLS 测试使用 musl |
| open/read/write/close/lseek/fstat | 常用标志，实际 stat64 inode/类型/大小/链接数，标准库宽度转换 | 非法缓冲区返回 EFAULT；零长度仍检查 FD；完整权限/用户身份模型未齐 |
| dup/dup2/fcntl 标志 | 支持 FD 共享描述与重定向 | FD 表有界；不支持完整 fcntl/文件锁集合 |
| pipe | 有界管道、读写端生命周期 | 256 字节缓冲，64 字节消息原子写；阻塞/非阻塞、超时/取消及错误回复缓冲区有专项回归；没有 SIGPIPE，断读端返回 EPIPE |
| mkdir/rmdir/unlink/rename/link/symlink/chmod/truncate/umask | 常用 *at 包装、RAM 元数据、真实链接身份和符号链接解析 | FAT/原生持久化与 RAM 具有不同边界；完整 pathname/权限/时间语义未齐 |
| 目录遍历 | opendir/fdopendir/readdir、定位 cookie、getcwd/chdir/fchdir | 每库与每架构分别验证；有界路径长度仍低于完整 POSIX 目标 |
| poll/select/ppoll/pselect | VFS/pipe/UART 实际 readiness、超时、EOF 与错误 FD | native 非空临时信号 mask 仍返回不支持；Linux glibc 的原子 mask 路径不同 |
| fsync/sync | 持久化后端和设备 FLUSH | RAM 后端不提供跨重启保证；原生双 bank 与 FAT 的事务承诺不同 |
| mmap/munmap/mprotect | MMU 匿名私有映射、范围操作、PROT_NONE | Linux profile 的扩展不自动成为 native libc mmap 支持；不允许 W+X；非 MMU mprotect 返回 ENOTSUP |
| clock_gettime/nanosleep/gettimeofday | 64 位单调快照、100 Hz 等待；MMU RTC 通过统一 CLOCK_GET64 接入，三架构库已实际通过 | STM32 无可用 RTC 时显式不支持且已验证实际 Newlib errno 转换；CPU 时钟未齐 |
| getpid/gettid | 地址空间组 ID / 任务 ID | 不冒充 Linux PID namespace；内核句柄带 generation |
| pthread_create/join/exit | Newlib NOS 端口与 musl 端口 | 上限受可用任务槽和栈/映射配额约束；musl 默认栈为嵌入式 16 KiB 配置，guard 4 KiB |
| pthread mutex/condvar | 普通/递归锁、等待/信号/广播、有限 timedwait | 受支持时钟与线程 API 范围限制；不支持完整 robust/process-shared/优先级协议组合 |
| pthread key/once | TLS key、析构、once | 使用有限表；不能由此推断支持线程取消 |
| pthread_cancel/detach/信号 | 未完整支持 | Newlib 的 cancel/detach 返回 ENOTSUP；musl detach 具有退出栈回收桥接，但未列为本次专项验证；cancel 返回 ENOTSUP，不能把未实现的信号取消当成成功；仅允许运行库启动所需的有限信号掩码路径 |
| fork/execve/共享库/网络 | native 两种库尚未完整接入 | NOS 的 native ELF spawn 与 Linux personality 的真实 fork/exec 分别描述 |

## 退出与并发契约

正常独立 C 应用从 main 返回后经 libc exit 路径处理 stdio/atexit；最小应用使用原生退出入口。进程组退出与单线程退出分开，clear_tid/futex 唤醒和地址空间引用回收由内核负责。

Newlib 使用 `TLS_GET` 获取任务本地 `_reent`，通过独立绑定槽避免把共享 `_impure_ptr` 当作跨核 TLS。分配器锁首次发布采用同步协议，stdio 使用 retargetable locks。线程退出清理自己的 `_dtoa` 等重入资源，不能误关闭进程共享标准流。持有分配器锁的任务异常退出时，分配器不能假设堆仍完整；端口记录失效并拒绝继续使用。owner-death 失效注入和 dtoa 回收前后精确堆计数尚无专项数值验收，不能由正常多线程测试推断通过。

用户缓冲区检查与复制走内核接口，避免直接解引用错误指针使调用任务退出。检查后其他线程仍可并发改变地址空间，因此传输必须处理真正复制失败，而不是只依赖早期检查。VFS 的延迟管道回复使用 token，取消或 EFAULT 不能错误消费/注入数据或永久占用 open description。

futex 比较使用对齐的 32 位原子读，保留负锁字位模式；WAIT 可读只读映射，clear_tid 仍需可写。REQUEUE/CMP_REQUEUE 在同一内核临界区处理源比较、唤醒与移动等待者，测试包含两等待者换队及三个真实 musl 条件变量等待者广播。TLS 绑定槽被解除映射或撤销写权限时会解绑；TCB/TLS 数据本身的生命周期由应用维护。

SMP、MPU、非 MMU 和不同 C 库必须分别执行对应验证。某个最小库或单核测试通过，不能代替另一种 C 库的跨核结果。

完整兼容回归使用 `python3 tests/acceptance.py --include-posix --require-frozen-source`。完整 glibc 程序组的默认宿主预算为 420 秒，单应用 focused 和原生专项为 180 秒；`--timeout` 可显式覆盖。预算是软件模拟的防挂上限，不是延迟承诺，也不免除缺失标记、非零退出或应用内断言失败。
