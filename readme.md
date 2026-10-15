# NOS

支持 ARM、ARM64、RISC-V 的微内核，包含能力控制 IPC、抢占调度、线程/锁、物理与虚拟内存、独立 ELF，以及用户态 VFS、UART 和 virtio-blk 服务。

```sh
bash scripts/setup_microkernel_env.sh
source /workspace/.nos-env/activate.sh
make qemu-test
python3 tests/acceptance.py
```

原版 QEMU 验证覆盖 ARMv7、ARM64、RV64 MMU，STM32F405 `netduinoplus2` 非 MMU/可选 MPU，以及 RV32 Sv32/非 MMU。Newlib 和 musl 使用实际源码构建；三个主架构支持 2/4 核验收。完整软件验收还包括持久化、服务恢复、故障注入和独立 Lua/LVGL 应用。

- [架构与边界](docs/MICROKERNEL.md)
- [构建与验证命令](docs/QEMU_VALIDATION.md)
- [本次验证结果](docs/VALIDATION_RESULTS.md)
- [阶段 0–5 执行清单](docs/EXECUTION_PLAN.md)
- [C 库与 POSIX 支持范围](docs/POSIX_SUPPORT.md)
- [独立应用 ABI](docs/ELF_ABI.md)

默认构建入口为微内核，旧 RTOS 命令保留在 `scripts/Makefile.legacy`。当前是有界嵌入式实现；动态链接、glibc、完整信号/fork、网络和真实板级 USB/显示/输入迁移不在已验证能力内。LVGL 的内存渲染通过不代表物理 LCD 通过。
