# RV32 平台

`riscv32_virt` 在原版 `qemu-system-riscv32 -M virt -bios none` 上运行 RV32GC/ILP32D、
Sv32、U/S/M 三种权限模式。`riscv32_nommu` 使用同一 CPU，保持 `satp=0`，使用实际 RAM
地址及共享地址空间。两者均支持独立 ELF32 程序、抢占、IPC、锁、线程与 UART IRQ 服务。
非 MMU 模式不提供进程内存隔离；PMP 配置允许访问平台地址空间。

```sh
source /workspace/.nos-env/activate.sh
make PLATFORM=riscv32_virt LIBC=minimal
python3 tests/qemu_matrix.py --platform riscv32_virt --libc minimal
python3 tests/qemu_matrix.py --platform riscv32_nommu --libc minimal
python3 tests/qemu_matrix.py --platform riscv32_virt --libc minimal --cpus 2
python3 tests/qemu_kernel_fault.py --platform riscv32_virt
python3 tests/qemu_kernel_fault.py --platform riscv32_nommu
```

正常系统：`make PLATFORM=riscv32_virt LIBC=minimal BUILD_MODE=normal qemu-run`。
可在 shell 执行 `run /bin/hello qemu`、`stats`、`restart`，重启后再测试文件读写。

实现要点：

- 400 字节上下文保存 32 位 GPR、32 个 64 位浮点寄存器和 FCSR；异常栈按 16 字节对齐。
- RV32 使用 high-low-high 读取 MTIME，比较寄存器采用 low=全1/high/low 顺序安全更新。
- M 模式定时器转发 SSIP；PLIC 向用户态工作线程交付 UART/virtio IRQ。
- 特权异常使用安全栈直接报告 `privileged_exception`，避免再次获取已持有的内核锁。
- 内核页表、保护页、PLIC 与 CLINT 别名由 supervisor 独占。CLINT 的 supervisor 别名
  为 `0x04000000`，避免与 `0x02000000` 的用户匿名映射区域重叠。
- SMP 使用每核内核/机器栈、M 软件中断进行 TLB/I-cache 同步。
- RV32 浮点回归通过栈传递双 GPR 的 64 位值，使用合法的 `fld/fsd` 指令；不使用 RV64
  专有的 `fmv.d.x` 或 `fmv.x.d`。

当前库矩阵仅声明 RV32 最小运行库；RV32 Newlib/musl 尚未加入构建矩阵，不能沿用 RV64
库测试结果。独立程序参数与非 MMU 重定位契约见 [ELF_ABI.md](ELF_ABI.md)。

本轮已实际执行 Sv32 自测、非 MMU 自测、Sv32 双核自测及两模式特权异常负向镜像。
正常 Sv32 双核系统连续重启两次 VFS/UART 服务后，任务/端点/空闲页回到相同基线，
UART 再次收到中断，文件读写与独立应用继续成功。最终计数以带 run ID 的 QEMU 报告为准。
