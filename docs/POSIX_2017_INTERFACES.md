<!-- Copyright (C) 2026 NOS contributors -->
# POSIX.1-2017 接口分类与验收边界

目标版本为 [POSIX.1-2017 System Interfaces](https://pubs.opengroup.org/onlinepubs/9699919799/idx/functions.html)。本表按实现依赖组织，不是逐符号符合性认证。库中导出了函数、源码可以编译、系统调用有分派，均不能单独证明接口满足规范。glibc、musl、Newlib 必须分别记录真实程序结果；Linux syscall ABI 也不等同于 POSIX API。

| 接口族与代表 API | 需要的实际行为 | 当前接口层边界 |
|---|---|---|
| ISO C 内存、字符串、格式化、stdio、errno | 真实 C 库、每线程重入、共享 FILE/分配器同步、退出 flush | Newlib/musl 和真实动态 glibc 已有独立程序验证；Newlib 将 NOS/Linux 错误号转换为其实际 errno 编号，不把数字编码相同当作前提 |
| 文件描述符：open/openat/close/read/write/readv/writev/lseek/dup/dup2/fcntl | OFD 共享偏移、短 I/O、阻塞/非阻塞、FD flags、原子 CLOEXEC | 用户态 VFS 提供真实 OFD 和 FD flags；三 MMU 的 native 两库及 glibc 独立文件程序已有回归，Linux profile 新增进程记录锁 F_GETLK/F_SETLK/F_SETLKW；按 inode/进程归属，fork 不继承、exec 保留、任一同 inode FD 关闭释放本进程全部区间；native 默认仍 ENOTSUP，其他 fcntl 命令未齐 |
| 定位 I/O：pread/pwrite | 64 位消息偏移、不改变 OFD 偏移、错误/短 I/O | native wrapper 使用 64 位偏移；Linux profile 的大 pread 使用 512 字节 bulk 中转，成功回复后再发布当前块，取消失败不发布该块；Newlib 的 off_t 仍受其真实 ABI 宽度限制 |
| 文件信息：stat/lstat/fstat/fstatat | 实际 inode、类型/大小、链接数、所有者和时间；不能截断不可表示的数据 | 新 stat64 IPC；Newlib 的 ino_t/off_t/time_t 宽度需要溢出处理；权限和时间更新仍不完整 |
| 目录：opendir/fdopendir/readdir/readdir_r/dirfd/rewinddir/telldir/seekdir/closedir | 每 OFD cursor、点目录、EOF 与 errno、长目录项、cookie 定位 | 新 getdents64 分片、Newlib DIR 实现、musl 真实目录实现桥接；新增独立 markers 验证 |
| 路径：getcwd/chdir/fchdir、*at | 进程共享 cwd、dirfd 相对解析、绝对路径忽略 dirfd、空路径和 flags 校验 | 用户态 VFS 解析；Linux profile 输入路径含 NUL 上限 256 字节，双路径 512 字节经 token 绑定 bulk IPC 复制，默认仍 64 字节。三 MMU SMP2 的真实 glibc 已通过 guard 末尾 NUL、跨 guard EFAULT、最长双路径、280 字节目录项、getcwd 有界输出和最长 cwd 返回上级；相对执行另由进程程序验收，不宣称无限路径 |
| 文件修改：mkdir/rmdir/unlink/rename/link/symlink/readlink/chmod/chown/utimensat/truncate/umask | 链接身份、权限检查、创建 mode、时间变更和原子 rename | RAM 后端新增真实 mode/umask、截断补零、硬链接身份和符号链接解析；ARM Newlib 已通过新增回归；chown、时间更新、跨后端完整语义仍未齐 |
| 进程：fork/exec*/wait*/_exit/exit/atexit/getpid | 地址空间复制、子进程继承、CLOEXEC、生存期和 wait 状态 | Linux personality 已有真实 fork、同 PID exec 替换、wait 状态以及 OFD/cwd/umask 继承和 CLOEXEC；ARM SMP2 glibc 进程程序已通过。native Newlib/musl 不因 Linux personality 通过而自动获得这些接口 |
| 身份、会话、作业控制：getuid/setuid/getgroups/setsid/setpgid/tcsetpgrp | 权限凭据、会话和进程组、控制终端 | 需要真实内核状态和 TTY 服务，不可用常量成功补齐 |
| 信号：sigaction/sigprocmask/sigsuspend/sigwait/kill/raise | 每线程 mask、进程 pending、处理器帧/返回、默认动作、EINTR/restart | native musl port 只有线程 mask 状态；Linux personality 已新增真实投递/返回，glibc 信号及 NPTL 取消必须按各架构独立报告 |
| 时间：clock_gettime/getres/nanosleep/clock_nanosleep/gettimeofday/time、timer* | 单调时钟、墙钟、绝对等待、剩余时间、信号计时器 | native 两库通过 CLOCK_GET64 共享实际 RTC 与单调快照，三 MMU 已通过 gettimeofday/REALTIME 绝对等待；Newlib 的 clock ID 1/4 显式转换成 NOS 的 0/1，32 位 time_t 溢出返回真实 EOVERFLOW。STM32 没有 RTC 时明确 ENOTSUP，仍保留单调等待；进程/线程 CPU 时钟和完整 timer 选项需独立验证 |
| 线程：pthread_create/join/self/equal/once/key*、mutex/cond/rwlock/barrier | TLS、跨核同步、析构、属性、取消/分离、锁类型与错误恢复 | native musl/Newlib 的 create/join/基本 mutex/cond/TSD 已测；真实 glibc NPTL 默认线程、跨核 TLS/errno/malloc/共享 FILE、detach/TSD、robust owner death 已在 ARM/RV64 SMP2、A64 SMP4 通过。rwlock/barrier/once/TSD 析构三轮再入另在三 MMU SMP2 通过；全部调度/优先级属性和其他组合不能由这些程序推断 |
| 线程取消及信号取消点 | 延迟/异步取消、清理栈、阻塞调用可中断、资源释放 | native Newlib/musl 的 pthread_cancel 仍显式 ENOTSUP；真实 glibc 的条件等待取消、清理/TSD 和纯用户代码异步取消已在 ARM/RV64 SMP2、A64 SMP4 通过，不泛化为另两库支持；所有 I/O 取消点和取消竞态仍需更广覆盖 |
| 管道和终端：pipe/pipe2/isatty/termios/ttyname | 真阻塞、O_NONBLOCK、dup 后实际类型、TTY 参数与控制终端 | VFS pipe 队列已有真实回归；UART 字符输入不同于完整 POSIX TTY/PTY |
| 多路 I/O：poll/select/pselect、fd_set | 多对象等待、ready 条件、原子 signal mask、超时 | VFS 查询实际 OFD/pipe/UART readiness，native Newlib/musl 已桥接并新增超时/EOF/无效 FD 回归；native ppoll/pselect 非空 signal mask 尚不支持，Linux gateway 的原子 mask 等待单独验证 |
| 网络：socket/bind/listen/accept/connect/send/recv/getsockopt/getaddrinfo | 协议栈、socket OFD、阻塞/非阻塞、解析服务 | 尚无完整网络服务，glibc 的网络函数导出不代表可用 |
| 映射：mmap/munmap/mprotect/msync、shm_open/shm_unlink | 文件映射、共享一致性、偏移/保护、写回、生命周期 | Linux profile 的 RAM 文件采用同一真实背页，glibc 三 MMU SMP2 已验证 pwrite/pread 与 MAP_SHARED 相互可见、msync、只读 FD 映射禁止写升级、unlink 后旧映射存活和同名重建独立。不能泛化为 FatFs/磁盘 mmap 或所有持久化写回语义 |
| 同步与 IPC：sem*、mq*、shm* | 命名对象、生存期、进程共享、阻塞/优先级/超时 | glibc 三 MMU SMP2 已验证 unnamed private 信号量阻塞/超时、shm_open 上 pshared=1 信号量跨 fork 不同 VA 等待唤醒、named sem 经 exec 在不同 VA 打开及 unlink/重建身份。mq、所有 named 对象权限/资源竞态和持久化恢复仍不能由这些程序推断支持 |
| 资源/调度：getrlimit/setrlimit/getrusage/sched*/nice | 实际限制执行、统计、优先级策略和权限 | 单纯返回数值不够；需按 NOS 调度器和资源配额逐项实现 |
| 纯库扩展：regex/glob/fnmatch/iconv/locale/strftime | 库实现和 locale/时区/文件依赖可获得 | 真实 libc 可提供大量算法；glob 仍依赖目录，locale/iconv/时区数据必须打包并实际测试 |
| 系统信息：sysconf/pathconf/confstr/uname | 返回与执行的 limits/options 一致 | 有界 NOS 配额应真实报告；不能直接继承 Linux 主机配置 |

POSIX 的可选功能需要按规范的 option 标识分别选择和记录，例如 Asynchronous I/O、Message Passing、Process Scheduling、Shared Memory Objects、Memory Protection、Realtime Signals、Timers，以及 XSI 扩展。不能把所有可选组当作基础要求，也不能在宣称某组支持后跳过其语义。基础接口中的错误路径、线程取消点、异步信号安全、环境继承和资源限制同样属于验收范围。

本轮新增 libc 检查标记为 `posix_openat_positioned_io_stat`、`posix_cwd_relative_paths`、`posix_directory_iteration_seek`、`posix_directory_long_record`、`posix_poll_select_readiness`、`posix_poll_timeout_eof_invalid`、`posix_metadata_modes_truncate`、`posix_metadata_links_symlinks`。只有对应架构实际运行和严格报告通过后，才能将本表中的新增实现写入已验证能力表。详细运行身份以报告保存的源码、配置、镜像和工具摘要为准。

长路径应用的独立三架构 SMP2 报告为 `out/microkernel/glibc-reports/1792131012152301653/results.json`，ARM/A64/RV64 分别通过 103/104/105 项检查并退出 0。这是 focused 开发验证；最终统一验收必须重新执行包含全部应用的冻结源码矩阵。
