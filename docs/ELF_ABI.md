# 独立应用与启动 ABI

<!-- Copyright (C) 2026 NOS contributors -->

`apps/hello.c` 独立编译链接成 ELF，再由 `scripts/embed_initramfs.py` 放入只读启动归档。
内核通过 `NOS_SYS_EXEC_SPAWN` 加载归档中的程序；程序代码不依赖内核镜像中的函数地址。
正常启动镜像的 `nos_init` 启动 `/bin/hello`，随后提供 UART 交互 shell。

## 启动与参数

系统调用参数是 `struct nos_exec_request` 的用户地址，包含绝对路径、以空指针结束的
`argv`/`envp` 以及优先级。内核复制所有字段，最多 8 个参数、8 个环境变量，每个字符串
含结束符最多 64 字节。只有具有 `NOS_CAP_SPAWN` 的调用者可以创建独立应用。

初始栈按 16 字节对齐，布局是 `argc, argv[], NULL, envp[], NULL, auxv[]`。
入口寄存器传递 `argc`、`argv`。辅助向量提供 `AT_PAGESZ`、`AT_ENTRY`、`AT_PHDR`、
`AT_PHENT`、`AT_PHNUM`，以及 NOS 私有 `NOS_AUX_TLS` 指向的 TLS 模板描述符。
不提供虚假的随机数；应用不能把缺少 `AT_RANDOM` 当作密码学熵来源。

`apps/start.c` 调用 `nos_app_libc_init`、`main`、`nos_app_finish`。
最小库启动不需要 libc；真实库应用另外链接 `apps/libc_start.c`、用户运行库及对应
Newlib/musl OS 适配器。musl 使用实际环境变量与辅助向量初始化 ELF `PT_TLS`。

## MMU 程序

ARM、ARM64、RISC-V RV64/Sv39 与 RV32/Sv32 接受本架构、小端、原生字宽的静态
`ET_EXEC`。链接布局：

| 区域 | 地址 / 范围 |
| --- | --- |
| 程序代码 | 从 `0x00480000` 起，RX |
| 独立只读数据段 | 代码窗口内按页对齐，R/NX |
| 私有初始化数据与 BSS | `0x01000000` 至 `0x01006000`，RW |
| 初始堆 | `0x01006000` 至 `0x0100c000` |
| 栈保护页 | `0x0100c000` 至 `0x0100d000` |
| 初始用户栈 | `0x0100d000` 至 `0x01010000` |

代码与只读段合计最大 512 KiB；初始化数据与 BSS 最大 24 KiB。更多堆空间由匿名映射提供，
受页池和 VMA 配额限制。`apps/app.ld` 将 ELF 文件头及程序头映射进 RX 段，使
libc 能读取实际 `AT_PHDR`。当前不支持解释器、动态链接器或共享库装载。

加载器在分配执行资源前检查头部、程序头范围、架构、长度溢出、对齐、入口指令位于文件内容支持的可执行段、
段页范围不重叠和 W^X。单独的 R-only LOAD 保持 R/NX；回归会实际执行其中的返回指令并要求 EFAULT，不能只根据页表标志宣称 NX。失败时调用方回收创建的地址空间；回归测试检查页池与任务配额恢复。

## 非 MMU 程序

STM32 的独立最小库应用使用 `apps/stm32.ld` 生成 ARM Thumb `ET_DYN` PIE。
加载器为程序分配连续 SRAM 镜像，复制代码、数据和 BSS，按照实际装载地址处理
`R_ARM_RELATIVE`。重定位写入目标必须完整落在 RW LOAD；动态表和 REL/RELA 表须完整由 LOAD 文件内容支持，动态表必须存在 DT_NULL。未知重定位、带符号重定位、解释器和超过 64 KiB 的镜像被拒绝。
程序使用自己的栈；软件可检查 ELF 段权限，但非 MMU 镜像不提供独立进程地址空间。

`riscv32_nommu` 使用 `apps/riscv32_nommu.ld`，接受 ELF32 `ET_DYN` 和无符号引用的
`R_RISCV_RELATIVE`/`DT_RELA` 重定位。裸机 GCC 编译应用对象，支持 ELF32 PIE 的 Linux
RISC-V binutils 执行链接；裸机发行版 ld 本身不支持 PIE。实际 SRAM 装载地址位于
`0x80000000` 以上，因此指针返回值仅把 `[-4095, -1]` 识别为错误，不能按有符号负数判断。
RV32 与 STM32 共用上述重定位边界检查。装载后分别设置代码 RX、只读数据 R/NX、数据 RW 的软件权限；STM32 MPU 构建还将这些权限落实到硬件区域。

现有发行版 Newlib 静态归档不是 PIE，因此 STM32 独立 Newlib ELF 未列入此装载契约。
STM32 上真实 Newlib 的验证仍由内核镜像中的用户运行库执行。需要独立装载完整 Newlib
程序时，须先构建满足 ARM PIE 重定位契约的 Newlib，或者采用受控固定地址 XIP 应用布局。

## Shell

`help`、`ls`、`cat`、`write`、`mkdir`、`mv`、`rm`、`rmdir`、`mount`、`sync`、
`run /bin/program [args]`、`stats`、`restart`。读取 UART 无数据时睡眠；VFS 服务异常退出后由 init
显式请求重新启动。文件系统业务继续在用户态服务中执行。

`stats` 显示任务、页池、端点与 UART IRQ/RX/就绪状态。`restart` 终止旧 VFS 服务组及其
UART 工作线程，等待异核任务退出后重建服务；init 保存新服务 PID，支持连续多次重启。
RAM 文件系统内容随服务重启丢弃；需要保留的数据应先同步到持久化挂载。

自测构建可以传入 `embed_initramfs.py --negative-fixtures` 加入损坏 ELF 归档条目。
这些测试镜像不会进入正常启动归档。实际执行证据应以本次 QEMU 日志为准。
