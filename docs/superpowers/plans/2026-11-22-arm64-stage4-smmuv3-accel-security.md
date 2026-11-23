# NOS ARM64 Stage 4：`SMMUv3` 硬件隔离、`AccelQueue` (`virtio-accel` / `rknpu`) 与 ARMv9 安全硬化实施计划

> **Architecture Spec:** `docs/superpowers/specs/2026-11-22-arm64-ai-native-kernel-design.md`

---

## Stage 4 核心目标与任务拆解

### Task 1: ARM64 `SMMUv3` 硬件驱动与统一 IOMMU/SVA 隔离层

- **Files:**
  - Create: `include/nos/iommu.h`, `arch/arm64/include/asm/smmuv3.h`, `arch/arm64/mm/smmuv3.c`
  - Modify: `arch/arm64/CMakeLists.txt`, `board/qemu-virt/init.c`
  - Create: `tests/host/test_smmuv3.c`, `tests/host/run_smmuv3_test.py`
  - Modify: `tests/run_host_regressions.py`
- **Targets:**
  - [x] **Step 1:** 实现可移植 IOMMU 契约 `include/nos/iommu.h` 与 ARM IHI 0070 标准 `SMMUv3` 驱动（`IDR0` 硬件探测 + 无 IOMMU 平台的 Shadow 同步隔离自动降级、Stream Table / CMDQ / EVTQ 初始化、`CR0`/`CR0ACK` 握手、64B Context Descriptor 完整 64-bit `MAIR` 与 `EPD1=1` 禁用内核空间 DMA）。
  - [x] **Step 2:** 实现 `nos_iommu_bind_space()` / `unbind_space()` / `translate()` / `tlbi_asid()` / `tlbi_va()`，打通 SVA/PASID 绑定及权限/边界翻译复核。
  - [x] **Step 3:** 在 `qemu-virt` 初始化路径探测默认 `smmuv3@0x09050000`，并通过主机单测 `run_smmuv3_test.py`。

---

### Task 2: ARMv9 PAC / BTI / MTE 安全硬化与 CCA/RMM 预留

- **Files:**
  - Modify: `include/nos/hal_arch.h`, `arch/arm64/include/asm/mmu.h`, `arch/arm64/mm/mmu.c`, `arch/arm64/include/asm/sysreg.h`, `arch/arm64/include/asm/cpufeature.h`, `arch/arm64/cpu/cpufeature.c`, `arch/arm64/include/asm/task.h`, `include/kernel/task.h`, `arch/arm64/boot/start.S`, `arch/arm64/cpu/cpu.c`, `arch/arm64/cpu/smp.c`, `arch/arm64/kernel/task.c`, `arch/arm64/CMakeLists.txt`
  - Create: `arch/arm64/include/asm/security.h`, `arch/arm64/cpu/security.c`
  - Create: `tests/host/test_arm64_security.c`, `tests/host/run_arm64_security_test.py`
  - Modify: `tests/run_host_regressions.py`
- **Targets:**
  - [x] **Step 1:** 扩展 `HAL_VM_PROT_BTI` / `HAL_VM_PROT_MTE`、`MT_NORMAL_TAGGED` (`0xF0`)、`ARM64_PTE_GP` 与 PAC/BTI/MTE/EL2 控制位定义，保证 `-march=armv8-a` 零警告兼容及 EL2 降级放行。
  - [x] **Step 2:** 实现每 CPU 安全策略激活、每任务独立 128-bit `APIAKey` 生成与切换加载、MTE2 同步检查配置以及 EL2 CCA/RMM 探测预留 `arm64_cca_probe()`。
  - [x] **Step 3:** 通过主机单测 `run_arm64_security_test.py`。

---

### Task 3: EL1 混合内核 `kernel/accel/`、`SYS_NOS_ACCEL_SUBMIT (22)`、`virtio-accel` 与 `soc/rk3588/` (`rknpu`)

- **Files:**
  - Modify: `include/uapi/nos/ai.h`, `kernel/cap/cap_table.c`, `kernel/ipc/ring_ipc.c`, `kernel/syscall/syscall.c`, `arch/arm64/kernel/trap.c`, `kernel/CMakeLists.txt`, `drivers/CMakeLists.txt`, `soc/Kconfig`
  - Create: `include/nos/accel.h`, `kernel/accel/accel_core.c`, `drivers/accel/sources.cmake`, `drivers/accel/virtio_accel.c`, `drivers/accel/rknpu.c`
  - Create: `soc/rk3588/include/soc/rk3588.h`, `soc/rk3588/headers.cmake`, `soc/rk3588/sources.cmake`, `soc/rk3588/rk3588_soc.c`
  - Create: `tests/host/test_accel_queue.c`, `tests/host/run_accel_queue_test.py`
  - Modify: `tests/run_host_regressions.py`
- **Targets:**
  - [x] **Step 1:** 扩展 `ai.h` 定长结构（64B `nos_accel_caps` / `nos_accel_job_desc`、32B `nos_accel_job_result`）与审计事件，新增 `NOS_CAP_ACCEL` 支持与 `kernel/accel/accel_core.c` 门禁（VMO 读写/Sealed 检查、Cell 5D 硬配额扣费、SMMUv3 SVA 绑定、审计、`sched_infer` 截止期继承）。
  - [x] **Step 2:** 打通 `SYS_NOS_ACCEL_SUBMIT` (22) 及 `RingIPC` `NOS_RING_OP_ACCEL_JOB` 联合直通路径（严格防重复扣费）。
  - [x] **Step 3:** 实现纯整数无浮点 `virtio-accel` EL1 异步算子驱动（`GEMV_I32`/`MATMUL_I8`/`RELU_I32`）与 `soc/rk3588/` + `rknpu` 驱动骨架，并通过 `run_accel_queue_test.py`。

---

### Task 4: QEMU 真实 `SMMUv3` & ARMv9 `-cpu max` 端到端硬件自检与全矩阵验收

- **Files:**
  - Modify: `arch/arm64/kernel/el0_smoke.S`, `arch/arm64/kernel/stage2_selftest.c`, `tests/qemu_arm64_smoke.py`
- **Targets:**
  - [x] **Step 1:** 实现 `arm64_el0_stage4_code`（EL0 `SYS_NOS_ACCEL_SUBMIT` GEMV 精确计算 + Sealed 输出 VMO `-EACCES` 拦截 + 真实 `pacia`/`autia` 硬件指令校验）与 `run_stage4_selftest()`（SMMUv3 跨 Cell 隔离、`RingIPC` 联合驱动加速器、超限 `-EDQUOT` 与 MTE/BTI 页表验证）。
  - [x] **Step 2:** 通过全 5 个 CMake Preset 零警告构建、6 组 QEMU 实机仿真矩阵（含 `--cpu max --iommu smmuv3`）、14 项主机 C 回归、`test_arch_flags`/`test_soc_headers`/`check_layer_deps` 及全部 24 项 Python unittest。
