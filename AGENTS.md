# NOS 开发指南

默认内核是跨架构微内核，旧 RTOS 已清理。先阅读 `Makefile`、`scripts/Makefile.microkernel`、`include/nos/` 与 `docs/MICROKERNEL.md`。原始审查在 `docs/DEVELOPMENT_PLAN.md`；当前状态以 `docs/EXECUTION_PLAN.md`、`docs/VALIDATION_RESULTS.md` 为准。

## 构建与验证

- 云环境：`bash scripts/setup_microkernel_env.sh`，然后 `source /workspace/.nos-env/activate.sh`。
- 构建：`make PLATFORM=arm_virt|arm64_virt|riscv_virt|stm32|riscv32_virt|riscv32_nommu LIBC=minimal|newlib|musl -j5`。只使用文档支持的组合。
- 基础验证：`make qemu-test`，执行原有八个真实 QEMU 组合。
- 完整软件验收：`python3 tests/acceptance.py --include-posix --require-frozen-source`；开发时按平台或 section 做局部检查。
- STM32 必须使用原版 QEMU 的 `netduinoplus2`；禁止用历史 `nos_stm32` 补丁机型替代。
- 实板键盘：`make PLATFORM=nk60_v2 LIBC=minimal firmware`，仅正常启动/单核；资源、烧录和非实板验证边界见 `docs/NK60_V2.md`。`boards` 验收包含模型测试和明确跳过 F401 时钟初始化的 QEMU 用户入口专项，不能宣称键盘外设已实测。

## 模块与权限

- `kernel/{core,sched,ipc,mm,exec,compat}/`：调度、内存、IPC、能力、同步。文件系统业务和 C 库分配器留在用户态。
- `arch/{arm,arm64,riscv,riscv32}/`：启动、异常、定时器、上下文、MMU/MPU。汇编与 `arch_frame` 偏移及编译期断言保持一致。
- `servers/`：用户态 VFS、后端和设备服务；服务调用使用 IPC，设备权限经能力控制。
- `user/`：用户运行库、init、nshell 与 Linux gateway；回归源码在 `tests/guest/`；`apps/` 为独立链接 ELF。
- `libc/port/`：真实 C 库 OS 钩子；未支持操作返回错误，不使用伪成功。
- 内核数据访问必须经过用户范围/权限检查和复制接口。MMU、MPU 与无保护共享内存分别描述，不能混称隔离。
- SMP 当前串行执行内核临界区，用户任务实际并发；不要把这一实现描述成多核同时执行内核。

## 风格与报告

C 使用 4 空格、K&R 大括号、snake_case、定宽整数和明确错误码。新文件包含版权头。Make 配方使用 Tab。

修改架构或 ABI 后运行对应真实 QEMU 测试；公共核心、内存或运行库修改完成后执行完整验收。最终测试期间冻结源码、文档和构建输入，使用独立输出目录避免并发构建竞争。核对运行身份、完整/局部矩阵、必需标记与退出状态；区分通过、失败、预期失败、不适用及未执行。远端 CI、真实硬件、IOMMU 与完整 Linux/POSIX 支持不能从 QEMU 基线推断。
