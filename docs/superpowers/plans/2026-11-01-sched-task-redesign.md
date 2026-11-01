# NOS Scheduler & Task Management Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Redesign the NOS scheduler and task management subsystem to support Linux-style multi-policy scheduling classes (`rt_sched_class` FIFO/RR + `fair_sched_class` CFS/EEVDF `vruntime` + `idle_sched_class`), Per-CPU SMP runqueues (`struct sched_rq`) with zero-overhead single-core MCU folding, fixed-point PELT CPU load tracking & `loadavg` (1m/5m/15m), multi-lock priority inheritance, `wchan` tracking, zero-heap dual-mode cross-task stack backtrace (FP + Thumb-2 `BL`/`BLX` scanner safe for 20KB RAM `wl-001`), crash dumps, and multi-view `ps` / `procfs`.

**Architecture:** A four-layer architecture comprising unified public headers (`include/nos/sched.h`, `include/kernel/sched.h`, `include/kernel/task.h`, `include/kernel/unwind.h`), a Per-CPU runqueue core (`kernel/sched/sched.c`, `kernel/sched/sched_load.c`), modular scheduling class engines (`kernel/sched/sched_rt.c`, `kernel/sched/sched_fair.c`, `kernel/sched/sched_idle.c`), and decoupled task lifecycle & observability modules (`kernel/task/task.c`, `kernel/task/task_dump.c`, `kernel/kallsyms.c`).

**Tech Stack:** C99/GNU11, ARM Cortex-M3/M4 / ARM64 / RISC-V / x86_64 HAL, CMake + Ninja + Kconfiglib, Python host regression tests & QEMU STM32 smoke tests.

---

## File Structure Map

- **Configuration & Headers:**
  - Modify `Kconfig`: Add `CONFIG_NR_CPUS`, `CONFIG_SCHED_FAIR`, `CONFIG_KALLSYMS`, `CONFIG_STACK_BACKTRACE`, `CONFIG_BACKTRACE_MAX_DEPTH`.
  - Create `include/nos/sched.h`: Declare `enum sched_policy`, `struct sched_entity`, `struct sched_class`, `struct rt_rq`, `struct fair_rq`, `struct sched_rq`, PELT constants, and internal scheduler class hooks.
  - Modify `include/kernel/sched.h`: Declare public scheduler APIs (`sch_init`, `sch_start`, `switch_task`, `sch_lock`, `sch_unlock`, `sched_get_loadavg`, `sched_get_cpu_util`, `sched_rq_of`, `this_rq`, `dump_sched_rq`).
  - Modify `include/kernel/task.h`: Define 6-state `enum task_status` (plus backward-compatible aliases), enhanced `struct task_struct` (`se`, `sched_class`, `ppid`, `exit_code`, `nice`, `policy`, `cpu`, `on_cpu`, `cpus_allowed`, `pi_boost_prio`, `wchan_addr`, `wchan_name`), and task control/dump APIs (`task_create_ex`, `task_set_policy`, `task_set_nice`, `task_set_affinity`, `task_pi_boost`, `task_pi_unboost`, `task_set_wchan`, `dump_all_task_ex`, `dump_task_detail`).
  - Create `include/kernel/unwind.h`: Declare `struct unwind_frame`, `dump_task_stack`, `dump_trap_backtrace`, `unwind_task_stack`, and `kallsyms_sprint_symbol`.
- **Scheduler Core & Classes (`kernel/sched/`):**
  - Create `kernel/sched/sched_rt.c`: Implement `rt_sched_class` (`SCHED_POLICY_RR` & `SCHED_POLICY_FIFO`, O(1) priority bitmap, RR time-slicing, preemption check, SMP RT push/pull migration).
  - Create `kernel/sched/sched_fair.c`: Implement `fair_sched_class` (`SCHED_POLICY_NORMAL`, 40-entry `nice` weight table, `vruntime` accounting, `min_vruntime` sleeper compensation, sorted intrusive timeline queue, preemption check, SMP work stealing).
  - Create `kernel/sched/sched_idle.c`: Implement `idle_sched_class` (`SCHED_POLICY_IDLE`, Per-CPU idle task fallback).
  - Create `kernel/sched/sched_load.c`: Implement fixed-point PELT (`0..1024` EWMA window accounting for tasks and CPUs), `0..10000` compatible `task_get_cpu_usage` / `get_cpu_usage`, and Linux-style `FIXED_1=2048` 1m/5m/15m `g_avenrun[3]`.
  - Modify `kernel/sched/sched.c`: Implement Per-CPU `g_sched_rqs[CONFIG_NR_CPUS]`, deferred `need_resched` under `scheduler_lock_nest`, `select_task_rq`, `sched_balance_rq`, IPI resched signaling, and legacy `add_task_to_ready_list*` / `del_task_to_ready_list*` wrappers.
- **Task Management, IPC & Observability (`kernel/task/`, `kernel/ipc/`, `arch/`, `fs/`, `app/`):**
  - Modify `kernel/task/task.c`: Decouple `task->se.run_node` from `task->list`, implement clean 6-state transitions, `task_create_ex`, `task_set_policy`, `task_set_nice`, `task_set_affinity`, `task_pi_boost` / `task_pi_unboost`, `task_waitpid`, and locked `task_foreach`.
  - Create `kernel/task/task_dump.c`: Implement zero-heap dual-mode unwinder (Frame Pointer + Thumb-2 `BL`/`BLX` instruction-verified stack scanner), `dump_task_stack`, `dump_trap_backtrace`, `dump_task_detail`, `dump_sched_rq`, and multi-view `dump_all_task_ex` (`ps`, `ps -l`, `ps -w`, `ps -s <pid>`).
  - Modify `kernel/kallsyms.c`: Upgrade `kallsyms_lookup` to $O(\log N)$ binary search and zero-ROM-overhead dynamic `symbolsize` calculation.
  - Modify `kernel/ipc/mutex.c`, `kernel/ipc/sem.c`, `kernel/ipc/msg_queue.c`, `kernel/ipc/endpoint.c`, `kernel/ipc/notification.c`: Record `wchan` while blocked and use multi-lock safe priority inheritance.
  - Modify `arch/arm/cpu/cpu.c`, `arch/arm/cpu/cortex-m3/cpu_c.c`, `arch/arm/cpu/cortex-m4/cpu_c.c`: Route `arch_dump_stack` and fault handlers (`cpu_hard_fault`, `MemManage_Handler`, `BusFault_Handler`, `UsageFault_Handler`) to the new unwinder and fault register decoder.
  - Modify `app/shell/nshell.c` & `fs/procfs.c`: Upgrade `cmd_ps` (`-l`, `-w`, `-s <pid>`) and `/proc/tasks`, `/proc/loadavg`, `/proc/stat`.
  - Modify `kernel/CMakeLists.txt`: Register new scheduler and task dump source files.
- **Tests:**
  - Create `tests/host/test_sched_multi_policy.c` & `tests/host/run_sched_multi_policy_test.py`
  - Create `tests/host/test_sched_load_dump.c` & `tests/host/run_sched_load_dump_test.py`
  - Modify `tests/run_host_regressions.py`

---

### Task 1: Kconfig Options & Unified Scheduler / Task / Unwind Headers

**Files:**
- Modify: `Kconfig`
- Create: `include/nos/sched.h`
- Modify: `include/kernel/sched.h`
- Modify: `include/kernel/task.h`
- Create: `include/kernel/unwind.h`

- [ ] **Step 1: Add scheduler, SMP, and backtrace options to `Kconfig`**

In `Kconfig` under `menu "Kernel Configuration"`, add:
```kconfig
config NR_CPUS
	int "Maximum number of CPUs"
	default 4 if SMP
	default 1
	range 1 8
	help
	  系统支持的最大 CPU 核心数。单核模式下固定为 1。

config SCHED_FAIR
	bool "Enable CFS/EEVDF Fair Scheduling Class (SCHED_NORMAL)"
	default y
	help
	  启用基于虚拟运行时间 (vruntime) 与 Nice 权重的公平调度类，与实时调度类并存。

config KALLSYMS
	bool "Enable kernel symbol table lookup (kallsyms)"
	default y
	help
	  启用内核符号表查找，用于栈回溯、WCHAN 显示与异常现场解析。

config STACK_BACKTRACE
	bool "Enable cross-task stack backtrace and crash unwinder"
	default y
	help
	  启用零堆分配的双模栈回溯器（支持 Frame Pointer 与栈代码地址扫描）。

config BACKTRACE_MAX_DEPTH
	int "Maximum stack backtrace frame depth"
	depends on STACK_BACKTRACE
	default 12
	range 4 32
```

- [ ] **Step 2: Create `include/nos/sched.h` and update `include/kernel/sched.h`, `include/kernel/task.h`, `include/kernel/unwind.h`**

Define `struct sched_entity`, `struct sched_class`, `struct rt_rq`, `struct fair_rq`, `struct sched_rq`, `enum sched_policy`, `enum task_status`, `struct task_struct`, and `struct unwind_frame`.

- [ ] **Step 3: Commit Task 1**

```bash
git add Kconfig include/nos/sched.h include/kernel/sched.h include/kernel/task.h include/kernel/unwind.h
git commit -m "Sched: Add multi-class scheduler, SMP runqueue, and unwind headers"
```

---

### Task 2: Multi-Class Scheduler Core (`rt`, `fair`, `idle`), Per-CPU SMP Runqueue & PELT Load Tracking

**Files:**
- Create: `kernel/sched/sched_rt.c`
- Create: `kernel/sched/sched_fair.c`
- Create: `kernel/sched/sched_idle.c`
- Create: `kernel/sched/sched_load.c`
- Modify: `kernel/sched/sched.c`
- Create: `tests/host/test_sched_multi_policy.c`
- Create: `tests/host/run_sched_multi_policy_test.py`

- [ ] **Step 1: Write the failing host test `tests/host/test_sched_multi_policy.c` & `tests/host/run_sched_multi_policy_test.py`**

Test:
1. `rt_sched_class`: O(1) priority preemption, `SCHED_POLICY_FIFO` (no time-slice preemption among equal priority), `SCHED_POLICY_RR` (round-robin rotation after `init_tick` ticks).
2. `fair_sched_class`: `SCHED_POLICY_NORMAL` tasks with `nice = 0` (`weight = 1024`) vs `nice = -5` (`weight = 3121`) receiving proportional CPU execution time via `vruntime`, and RT tasks always preempting Fair tasks.
3. `idle_sched_class`: Picked only when both RT and Fair queues are empty.
4. Deferred rescheduling under `sch_lock()` / `sch_unlock()`: calling `switch_task()` while `scheduler_lock_nest > 0` sets `rq->need_resched` without switching until `sch_unlock()`.
5. Multi-core SMP (`CONFIG_NR_CPUS = 4`): `select_task_rq` picks idle/least-loaded CPU respecting `cpus_allowed`, triggers `hal_smp_send_ipi`, and `sched_balance_rq` steals runnable tasks from overloaded CPUs.

- [ ] **Step 2: Implement `sched_rt.c`, `sched_fair.c`, `sched_idle.c`, `sched_load.c`, and `sched.c`**

Implement the complete multi-class scheduler, PELT load tracking, `loadavg` (1m/5m/15m), and SMP load balancing.

- [ ] **Step 3: Run `python3 tests/host/run_sched_multi_policy_test.py` and verify it passes**

Run: `python3 tests/host/run_sched_multi_policy_test.py`
Expected: PASS

- [ ] **Step 4: Commit Task 2**

```bash
git add kernel/sched/ tests/host/test_sched_multi_policy.c tests/host/run_sched_multi_policy_test.py
git commit -m "Sched: Implement RT/Fair/Idle scheduling classes, Per-CPU SMP rq, and PELT load tracking"
```

---

### Task 3: Task Lifecycle, Multi-Lock Priority Inheritance, WCHAN & IPC Integration

**Files:**
- Modify: `kernel/task/task.c`
- Modify: `kernel/ipc/mutex.c`
- Modify: `kernel/ipc/sem.c`
- Modify: `kernel/ipc/msg_queue.c`
- Modify: `kernel/ipc/endpoint.c`
- Modify: `kernel/ipc/notification.c`
- Modify: `kernel/syscall/syscall.c`
- Modify: `init/init.c`

- [ ] **Step 1: Upgrade `kernel/task/task.c` & `init/init.c`**

1. Separate `task->se.run_node` (used exclusively by `sched_rq`) from `task->list` (used exclusively by wait queues and `g_close_task_list`).
2. Properly distinguish `TASK_SLEEPING` (`TASK_WAIT`), `TASK_BLOCKED`, and `TASK_SUSPENDED` (`TASK_SUSPEND`):
   - `task_block_lock(task, status, wchan_name, wchan_addr)` transitions a running/ready task into `TASK_SLEEPING` or `TASK_BLOCKED` and dequeues it from its `sched_rq`.
   - `task_hang(task)` transitions a task into `TASK_SUSPENDED` (or `TASK_WAIT` for legacy callers when invoked from IPC wait paths via `task_hang_lock`).
   - `task_resume(task)` wakes a `TASK_SLEEPING`, `TASK_BLOCKED`, or `TASK_SUSPENDED` task, clears `wchan`, stops its timeout timer, and enqueues it onto the target CPU's `sched_rq`.
3. Implement multi-lock priority inheritance helpers `task_pi_boost()` and `task_pi_unboost()`, plus `task_create_ex()`, `task_set_policy()`, `task_set_nice()`, `task_set_affinity()`, and `task_waitpid()`.
4. Configure `idel_task_init()` in `init/init.c` to bind the idle task to `SCHED_POLICY_IDLE` (`idle_sched_class`).

- [ ] **Step 2: Update `kernel/ipc/{mutex,sem,msg_queue,endpoint,notification}.c` to record WCHAN and multi-lock PI**

Record `wchan_name` and `wchan_addr` before sleeping on `mutex`, `sem`, `msg_queue`, `endpoint`, or `notification`, and compute the next highest waiter priority on `mutex_unlock` / `endpoint` reply so nested locks retain priority inheritance while keeping `tests/host/run_mutex_test.py` passing.

- [ ] **Step 3: Run `python3 tests/host/run_mutex_test.py` and `python3 tests/host/run_sched_multi_policy_test.py`**

Run: `python3 tests/host/run_mutex_test.py && python3 tests/host/run_sched_multi_policy_test.py`
Expected: PASS

- [ ] **Step 4: Commit Task 3**

```bash
git add kernel/task/task.c kernel/ipc/ kernel/syscall/syscall.c init/init.c
git commit -m "Task: Decouple runqueue/wait nodes, add multi-lock PI, WCHAN tracking, and policy APIs"
```

---

### Task 4: Zero-Heap MCU-Friendly Stack Backtrace, Kallsyms Binary Search, Crash Dump & Enhanced `ps` / `procfs`

**Files:**
- Modify: `kernel/kallsyms.c`
- Create: `kernel/task/task_dump.c`
- Modify: `kernel/CMakeLists.txt`
- Modify: `arch/arm/cpu/cpu.c`
- Modify: `arch/arm/cpu/cortex-m3/cpu_c.c`
- Modify: `arch/arm/cpu/cortex-m4/cpu_c.c`
- Modify: `fs/procfs.c`
- Modify: `app/shell/nshell.c`
- Create: `tests/host/test_sched_load_dump.c`
- Create: `tests/host/run_sched_load_dump_test.py`
- Modify: `tests/run_host_regressions.py`

- [ ] **Step 1: Write the failing host test `tests/host/test_sched_load_dump.c` & `tests/host/run_sched_load_dump_test.py`**

Test:
1. `kallsyms_lookup`: binary search accuracy, dynamic `symbolsize` from adjacent sorted symbols (`0` extra ROM cost), and out-of-range filtering.
2. PELT & LoadAvg: window utilization accumulation, EWMA decay across active and idle windows, `task_get_cpu_usage` (`0..10000` scale), and 1m/5m/15m `sched_get_loadavg` updates.
3. Dual-mode stack unwinder (`unwind_task_stack` & `dump_task_stack`):
   - Unwinding via Frame Pointer chain within `[stack_low, stack_high)` bounds;
   - Unwinding via Stack Text Scanner with ARM Thumb-2 `BL` (`0xF000 / 0xF800`) and `BLX Rm` (`0x4780`) instruction verification, rejecting non-call function pointers and out-of-stack pointers;
   - Zero heap allocations during unwinding and dump.
4. Multi-view `ps` formatting (`PS_MODE_NORMAL`, `PS_MODE_LONG`, `PS_MODE_STACK` / `dump_task_detail`) verifying `PID`, `PPID`, `CPU`, `POL`, `PRIO`, `STAT`, `%CPU`, `TIME`, `WCHAN`, and call trace output.

- [ ] **Step 2: Implement `kernel/kallsyms.c`, `kernel/task/task_dump.c`, ARM fault diagnostics, `fs/procfs.c`, and `app/shell/nshell.c`**

Implement binary-search `kallsyms_lookup`, `kernel/task/task_dump.c`, Cortex-M fault backtrace & register decoding, `/proc/loadavg` + `/proc/stat` + `/proc/tasks`, and `nshell` `ps [-l|-w|-s <pid>]`.

- [ ] **Step 3: Run `python3 tests/host/run_sched_load_dump_test.py`**

Run: `python3 tests/host/run_sched_load_dump_test.py`
Expected: PASS

- [ ] **Step 4: Register new host tests in `tests/run_host_regressions.py` and commit Task 4**

```bash
git add kernel/kallsyms.c kernel/task/task_dump.c kernel/CMakeLists.txt arch/arm/cpu/ fs/procfs.c app/shell/nshell.c tests/host/test_sched_load_dump.c tests/host/run_sched_load_dump_test.py tests/run_host_regressions.py
git commit -m "Task: Add zero-heap dual-mode stack unwinder, crash dump, procfs loadavg, and multi-view ps"
```

---

### Task 5: Full Host Regression Suite, 4-Board Firmware Builds (`wl-001`, `nk60-v2`, `qemu-stm32`, `zj-v3`) & QEMU Smoke Verification

**Files:**
- Verify: `build/wl-001/nos.elf`
- Verify: `build/nk60-v2/nos.elf`
- Verify: `build/qemu-stm32/nos.elf`
- Verify: `build/zj-v3/nos.elf`

- [ ] **Step 1: Run all host regressions and layer dependency checks**

```bash
python3 tests/run_host_regressions.py
python3 -m unittest discover -s tests -p 'test_*.py'
cmake -P tests/test_arch_flags.cmake
cmake -P tests/test_soc_headers.cmake
python3 scripts/check_layer_deps.py
```

- [ ] **Step 2: Build all four board presets (`wl-001`, `nk60-v2`, `qemu-stm32`, `zj-v3`) and verify zero warnings/errors and `wl-001` RAM/Flash fit**

```bash
cmake --preset wl-001 && cmake --build --preset wl-001
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
cmake --preset zj-v3 && cmake --build --preset zj-v3
```

- [ ] **Step 3: Run QEMU STM32 smoke test**

```bash
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```
