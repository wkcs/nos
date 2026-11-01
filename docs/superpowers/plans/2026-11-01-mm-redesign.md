# NOS Memory Management Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Redesign NOS memory management to support coexisting low-fragmentation MCU allocation (TLSF + zero-hole linker/sbrk optimization) and Linux-style large-memory allocation (head-page Buddy + `alloc_pages_exact` + SLUB `kmem_cache`), while adding ARMv7-M hardware MPU protection (static regions + 32B task stack guard + dynamic VM slots + `MemManage` diagnostics) and MMU virtual memory (VMA + multi-level page tables + Demand Paging + Copy-on-Write).

**Architecture:** A unified memory & VMM API (`include/kernel/mm.h`, `include/nos/mm.h`, `include/nos/hal_arch.h`) backed by polymorphic physical memory nodes (`MM_NODE_ALGO_TLSF` and `MM_NODE_ALGO_BUDDY` + SLUB) and dual-mode address space engines (`kernel/mm/vmm_mpu.c` + `arch/arm/mm/mpu.c` for MCU MPU, `kernel/mm/vmm_mmu.c` + arch HAL page tables for MMU targets).

**Tech Stack:** C99/GNU11, ARMv7-M PMSAv7 MPU, ARM64/RISC-V/x86_64 Page Tables, CMake + Ninja + Kconfiglib, Python host regression runner & QEMU STM32 smoke tests.

---

## File Structure Map

- **Configuration & Linker Scripts:**
  - Modify `Kconfig`: Add `CONFIG_MM_TLSF`, `CONFIG_MM_SLUB`, `CONFIG_MPU_STACK_GUARD`.
  - Modify `board/{nk60-v2,wl-001,zj-v3,qemu-stm32,nutiny-nuc472}/nos_kernel.ld.S`: Remove `.data` `ALIGN(CONFIG_PAGE_SIZE)` holes and relax pool alignment under `CONFIG_MM_TLSF`.
  - Modify `libs/libc/port/picolibc_newlib.c`: Remove static 16KB `g_libc_sbrk_arena` BSS array and bridge `_sbrk_r` to `kmalloc`.
- **Core Physical & Kernel Allocators (`kernel/mm/`):**
  - Modify `include/kernel/mm.h`: Declare `enum mm_node_algo`, `kmalloc_aligned`, `kzalloc_aligned`, `alloc_pages_exact`, `free_pages_exact`, `kmem_cache_*`, and TLSF/SLUB/Buddy structures.
  - Create `include/nos/mm.h`: Declare unified VMM (`nos_vm_space`, `nos_vm_area`, `nos_vm_mmap`, `nos_vm_munmap`, `nos_vm_mprotect`, `nos_vm_handle_fault`, `nos_vm_clone`) and MPU region APIs.
  - Create `kernel/mm/mm_tlsf.c`: Implement $O(1)$ two-level segregated fit (FLI/SLI) allocator with compact boundary tags, payload-embedded free lists, immediate neighbor coalescing, aligned allocation, and PID ownership cleanup.
  - Modify `kernel/mm/mm_buddy.c`: Upgrade to standard Linux-style head-page-only free lists, greedy power-of-two initialization without wasteful high-order address alignment loss, and `alloc_pages_exact` / `free_pages_exact`.
  - Create `kernel/mm/mm_slub.c`: Implement Linux-style SLUB `kmem_cache` allocator and built-in `kmalloc-16..2048` caches.
  - Modify `kernel/mm/mm_node.c`, `kernel/mm/mm_page.c`, `kernel/mm/mm.c`, `kernel/mm/mm_block.c`: Polymorphic node initialization and unified dispatch across TLSF, SLUB, Buddy, and legacy block APIs.
- **MCU Hardware MPU Subsystem:**
  - Create `arch/arm/mm/mpu.c` & `kernel/mm/vmm_mpu.c`: Implement ARMv7-M PMSAv7 8-region configuration (NULL trap, Flash RX, SRAM RW+XN, Periph Device, 32B Stack Guard in Region 5, Dynamic VM slots in Regions 6-7, and `MemManage_Handler` diagnostics).
  - Modify `arch/arm/hal/hal_arm.c`, `arch/arm/CMakeLists.txt`, `kernel/task/task.c`, `kernel/sched/sched.c`: Wire 32-byte aligned task stack guard allocation and fast MPU context switching.
- **MMU Virtual Memory Subsystem:**
  - Create `kernel/mm/vmm_mmu.c`: Implement VMA interval management (allocation, merge, split, protect), multi-level page table walker (`hal_vm_map`, `hal_vm_unmap`, `hal_vm_translate`, `hal_vm_space_destroy`), Demand Paging, and Copy-on-Write (`nos_vm_handle_fault`, `nos_vm_clone`).
  - Modify `include/nos/hal_arch.h`, `arch/arm64/hal/hal_arm64.c`, `arch/riscv/hal/hal_riscv.c`, `arch/x86/hal/hal_x86_64.c`: Connect HAL VM operations to real page table management.
- **Tests:**
  - Create `tests/host/test_mm_tlsf.c` & `tests/host/run_mm_tlsf_test.py`
  - Modify `tests/host/test_mm_buddy.c` & `tests/host/run_mm_buddy_test.py`
  - Create `tests/host/test_mm_slub.c` & `tests/host/run_mm_slub_test.py`
  - Create `tests/host/test_mm_mpu.c` & `tests/host/run_mm_mpu_test.py`
  - Create `tests/host/test_mm_vmm.c` & `tests/host/run_mm_vmm_test.py`
  - Modify `tests/run_host_regressions.py`

---

### Task 1: Kconfig Options, Linker Script Zero-Hole Optimization & Newlib `_sbrk` Cleanup

**Files:**
- Modify: `Kconfig`
- Modify: `board/nk60-v2/nos_kernel.ld.S`
- Modify: `board/wl-001/nos_kernel.ld.S`
- Modify: `board/zj-v3/nos_kernel.ld.S`
- Modify: `board/qemu-stm32/nos_kernel.ld.S`
- Modify: `board/nutiny-nuc472/nos_kernel.ld.S`
- Modify: `libs/libc/port/picolibc_newlib.c`

- [ ] **Step 1: Add `CONFIG_MM_TLSF`, `CONFIG_MM_SLUB`, and `CONFIG_MPU_STACK_GUARD` to `Kconfig`**

```kconfig
config MM_TLSF
	bool "Enable TLSF O(1) low-fragmentation allocator"
	default y if !MMU
	help
	  启用 TLSF 二级位图紧凑边界标记分配器，消除页截断浪费与小内存碎片。

config MM_SLUB
	bool "Enable SLUB kmem_cache small-object allocator"
	default y
	help
	  启用 Linux 风格 SLUB (kmem_cache) 小对象分配器，支持与 Buddy/TLSF 并存。

config MPU_STACK_GUARD
	bool "Enable 32-byte hardware MPU task stack guard"
	default y
	depends on MPU
	help
	  使用硬件 MPU Region 5 在当前运行任务栈底设置 32 字节禁访红区，实时捕获栈溢出。
```

- [ ] **Step 2: Remove `.data` page-alignment holes in `board/*/nos_kernel.ld.S` and static 16KB `g_libc_sbrk_arena` in `libs/libc/port/picolibc_newlib.c`**

In `board/*/nos_kernel.ld.S`, replace `. = ALIGN(CONFIG_PAGE_SIZE);` before `_sdata` with `. = ALIGN(8);`, and before `__mm_pool_start` use:
```ld
#ifdef CONFIG_MM_TLSF
  . = ALIGN(8);
#else
  . = ALIGN(CONFIG_PAGE_SIZE);
#endif
```
In `libs/libc/port/picolibc_newlib.c`, replace the static 16KB `g_libc_sbrk_arena` with on-demand kernel heap allocation when `_sbrk_r` is invoked.

- [ ] **Step 3: Commit Task 1**

```bash
git add Kconfig board/*/nos_kernel.ld.S libs/libc/port/picolibc_newlib.c
git commit -m "MM: Add Kconfig allocator options and remove linker/sbrk RAM holes"
```

---

### Task 2: TLSF $O(1)$ Low-Fragmentation Allocator Engine (`kernel/mm/mm_tlsf.c`)

**Files:**
- Modify: `include/kernel/mm.h`
- Create: `kernel/mm/mm_tlsf.c`
- Create: `tests/host/test_mm_tlsf.c`
- Create: `tests/host/run_mm_tlsf_test.py`

- [ ] **Step 1: Write the failing host test `tests/host/test_mm_tlsf.c` and runner `tests/host/run_mm_tlsf_test.py`**

Test cases in `tests/host/test_mm_tlsf.c`:
1. Exact tail-byte utilization (non-page-aligned arena size, e.g. 20,120 bytes on MCU, >98% allocatable).
2. Immediate physical neighbor coalescing (allocate 64 chunks, free in random/alternating order, then allocate a single chunk spanning the full arena -> succeeds with 0 external fragmentation).
3. Aligned allocation (`mm_tlsf_alloc_aligned` with 32B MPU guard alignment and 4096B page alignment, verifying leading gap is recycled into the free pool).
4. `mm_tlsf_free_by_pid(pid)` releases all blocks owned by `pid` and merges adjacent blocks.
5. 8-thread concurrent allocation/free/realloc stress test.

- [ ] **Step 2: Run `python3 tests/host/run_mm_tlsf_test.py` to verify it fails before implementation**

Run: `python3 tests/host/run_mm_tlsf_test.py`
Expected: FAIL (missing `kernel/mm/mm_tlsf.c`).

- [ ] **Step 3: Implement `kernel/mm/mm_tlsf.c` and declarations in `include/kernel/mm.h`**

Implement:
- `int mm_tlsf_init_pool(struct mm_tlsf_pool *pool, addr_t start, size_t size);`
- `void *mm_tlsf_alloc(struct mm_tlsf_pool *pool, size_t size, size_t align, pid_t pid);`
- `int mm_tlsf_free(struct mm_tlsf_pool *pool, void *ptr);`
- `size_t mm_tlsf_block_size(void *ptr);`
- `int mm_tlsf_free_by_pid(struct mm_tlsf_pool *pool, pid_t pid);`

- [ ] **Step 4: Run `python3 tests/host/run_mm_tlsf_test.py` and verify it passes**

Run: `python3 tests/host/run_mm_tlsf_test.py`
Expected: PASS.

- [ ] **Step 5: Commit Task 2**

```bash
git add include/kernel/mm.h kernel/mm/mm_tlsf.c tests/host/test_mm_tlsf.c tests/host/run_mm_tlsf_test.py
git commit -m "MM: Add O(1) TLSF low-fragmentation allocator engine"
```

---

### Task 3: Linux-Style Head-Page Buddy Allocator & `alloc_pages_exact` (`kernel/mm/mm_buddy.c`)

**Files:**
- Modify: `include/kernel/mm.h`
- Modify: `kernel/mm/mm_buddy.c`
- Modify: `kernel/mm/mm_page.c`
- Modify: `tests/host/test_mm_buddy.c`
- Modify: `tests/host/buddy_stubs/kernel/mm.h`

- [ ] **Step 1: Extend `tests/host/test_mm_buddy.c` with head-page-only list checks, unaligned start PFN greedy block placement, and `alloc_pages_exact` / `free_pages_exact` tests**

- [ ] **Step 2: Run `python3 tests/host/run_mm_buddy_test.py` to confirm new tests fail**

Run: `python3 tests/host/run_mm_buddy_test.py`
Expected: FAIL on `alloc_pages_exact` and unaligned high-order block formation.

- [ ] **Step 3: Upgrade `kernel/mm/mm_buddy.c` and `kernel/mm/mm_page.c`**

1. Only link the block head page `page[head_idx].list` into `buddy->info[order].list` ($O(1)$ split and coalesce).
2. Use node-relative buddy PFN calculation `rel_pfn = pfn - (node->start_pfn + node->buddy_page_index)` so contiguous memory after `buddy_page_index` forms maximum-order blocks without losing pages to physical alignment gaps.
3. Implement `__alloc_pages_exact` / `__free_pages_exact` and public `alloc_pages_exact` / `free_pages_exact` that split and return unused trailing pages immediately.

- [ ] **Step 4: Run `python3 tests/host/run_mm_buddy_test.py` and verify it passes**

Run: `python3 tests/host/run_mm_buddy_test.py`
Expected: PASS.

- [ ] **Step 5: Commit Task 3**

```bash
git add include/kernel/mm.h kernel/mm/mm_buddy.c kernel/mm/mm_page.c tests/host/test_mm_buddy.c tests/host/buddy_stubs/kernel/mm.h
git commit -m "MM: Upgrade buddy allocator to O(1) head-page lists and add alloc_pages_exact"
```

---

### Task 4: SLUB (`kmem_cache`) Allocator & Polymorphic `mm_node` Dispatch

**Files:**
- Create: `kernel/mm/mm_slub.c`
- Modify: `kernel/mm/mm_node.c`
- Modify: `kernel/mm/mm_page.c`
- Modify: `kernel/mm/mm.c`
- Modify: `kernel/CMakeLists.txt`
- Create: `tests/host/test_mm_slub.c`
- Create: `tests/host/run_mm_slub_test.py`

- [ ] **Step 1: Write failing host test `tests/host/test_mm_slub.c` and runner `tests/host/run_mm_slub_test.py`**

Test cases:
1. `kmem_cache_create`, `kmem_cache_alloc`, `kmem_cache_free`, `kmem_cache_destroy` with zero per-object header overhead inside slab pages.
2. Automatic slab page return to Buddy when all objects in a slab page are freed.
3. Built-in `kmalloc-16..2048` caches and transparent fallback to `alloc_pages_exact` for large allocations (`> 2048B`).
4. Coexistence of `MM_NODE_ALGO_TLSF` node and `MM_NODE_ALGO_BUDDY` node in the same system.

- [ ] **Step 2: Run `python3 tests/host/run_mm_slub_test.py` to verify it fails**

Run: `python3 tests/host/run_mm_slub_test.py`
Expected: FAIL (missing `kernel/mm/mm_slub.c`).

- [ ] **Step 3: Implement `kernel/mm/mm_slub.c` and polymorphic `mm_node` dispatch in `mm_node.c`, `mm_page.c`, `mm.c`**

- [ ] **Step 4: Run `run_mm_slub_test.py`, `run_mm_tlsf_test.py`, `run_mm_buddy_test.py`, and `run_mm_block_test.py`**

Run: `python3 tests/host/run_mm_slub_test.py && python3 tests/host/run_mm_tlsf_test.py && python3 tests/host/run_mm_buddy_test.py && python3 tests/host/run_mm_block_test.py`
Expected: All PASS.

- [ ] **Step 5: Commit Task 4**

```bash
git add include/kernel/mm.h kernel/mm/mm_slub.c kernel/mm/mm_node.c kernel/mm/mm_page.c kernel/mm/mm.c kernel/CMakeLists.txt tests/host/test_mm_slub.c tests/host/run_mm_slub_test.py
git commit -m "MM: Add SLUB kmem_cache allocator and polymorphic mm_node dispatch"
```

---

### Task 5: MCU Hardware MPU (PMSAv7) Subsystem & 32B Task Stack Guard

**Files:**
- Create: `include/nos/mm.h`
- Create: `kernel/mm/vmm_mpu.c`
- Create: `arch/arm/mm/mpu.c`
- Modify: `arch/arm/hal/hal_arm.c`
- Modify: `arch/arm/cpu/cortex-m3/cpu_c.c`
- Modify: `arch/arm/cpu/cortex-m4/cpu_c.c`
- Modify: `arch/arm/CMakeLists.txt`
- Modify: `kernel/task/task.c`
- Modify: `kernel/sched/sched.c`
- Modify: `include/kernel/task.h`
- Create: `tests/host/test_mm_mpu.c`
- Create: `tests/host/run_mm_mpu_test.py`

- [ ] **Step 1: Write failing host test `tests/host/test_mm_mpu.c` and runner `tests/host/run_mm_mpu_test.py`**

Test cases:
1. PMSAv7 `RBAR` and `RASR` encoding for static regions (NULL 64B No-Access, Flash 1MB RO+Exec, SRAM 128KB RW+XN, Periph 512MB Device+RW+XN).
2. 32-byte Task Stack Guard (`nos_mpu_configure_stack_guard`) alignment validation and Region 5 `RBAR`/`RASR` generation.
3. Dynamic task mappings (`hal_vm_map` / `hal_vm_unmap`) in Regions 6 & 7, including power-of-two validation, SRD subregion disable calculation, and `hal_vm_switch` register programming into mock/real `MPU_Type` registers.
4. `nos_mpu_classify_fault(cfsr, mmfar, current_task)` accurately distinguishes `NOS_MPU_FAULT_STACK_OVERFLOW`, `NOS_MPU_FAULT_NULL_PTR`, `NOS_MPU_FAULT_EXEC_XN`, and `NOS_MPU_FAULT_ACCESS_VIOLATION`.

- [ ] **Step 2: Run `python3 tests/host/run_mm_mpu_test.py` to verify it fails**

Run: `python3 tests/host/run_mm_mpu_test.py`
Expected: FAIL.

- [ ] **Step 3: Implement `include/nos/mm.h`, `kernel/mm/vmm_mpu.c`, `arch/arm/mm/mpu.c`, and integrate with `task.c` / `sched.c` / `cpu_c.c`**

- [ ] **Step 4: Run `python3 tests/host/run_mm_mpu_test.py` and verify it passes**

Run: `python3 tests/host/run_mm_mpu_test.py`
Expected: PASS.

- [ ] **Step 5: Commit Task 5**

```bash
git add include/nos/mm.h include/kernel/task.h kernel/mm/vmm_mpu.c arch/arm/mm/mpu.c arch/arm/hal/hal_arm.c arch/arm/cpu/cortex-m3/cpu_c.c arch/arm/cpu/cortex-m4/cpu_c.c arch/arm/CMakeLists.txt kernel/task/task.c kernel/sched/sched.c tests/host/test_mm_mpu.c tests/host/run_mm_mpu_test.py
git commit -m "MM: Add ARMv7-M hardware MPU support, 32B stack guard, and MemManage diagnostics"
```

---

### Task 6: MMU Virtual Memory Subsystem (`kernel/mm/vmm_mmu.c`) & Multi-Level Page Tables

**Files:**
- Create: `kernel/mm/vmm_mmu.c`
- Modify: `include/nos/hal_arch.h`
- Modify: `arch/arm64/hal/hal_arm64.c`
- Modify: `arch/riscv/hal/hal_riscv.c`
- Modify: `arch/x86/hal/hal_x86_64.c`
- Create: `tests/host/test_mm_vmm.c`
- Create: `tests/host/run_mm_vmm_test.py`

- [ ] **Step 1: Write failing host test `tests/host/test_mm_vmm.c` and runner `tests/host/run_mm_vmm_test.py`**

Test cases:
1. Multi-level page table walk (`hal_vm_map`, `hal_vm_translate`, `hal_vm_unmap`, `hal_vm_space_destroy`) verifying intermediate page table allocation, permission flag updates, unmap reclamation, and 0 page-table leaks on destroy.
2. VMA interval management (`nos_vm_mmap` with free-gap search and `NOS_VM_FLAG_FIXED`, adjacent VMA merging on identical permissions, VMA splitting on partial `nos_vm_munmap` / `nos_vm_mprotect`).
3. Demand Paging (`NOS_VM_FLAG_DEMAND`): `nos_vm_mmap` reserves VMA without physical pages; `nos_vm_handle_fault` allocates zeroed page on first access and maps it into page tables.
4. Copy-on-Write (`nos_vm_clone` + `nos_vm_handle_fault`): parent and child share physical page read-only with `use_cnt == 2`; write fault allocates private copy for writer, preserves data, and when second process writes with `use_cnt == 1` upgrades PTE in-place without extra copy.

- [ ] **Step 2: Run `python3 tests/host/run_mm_vmm_test.py` to verify it fails**

Run: `python3 tests/host/run_mm_vmm_test.py`
Expected: FAIL.

- [ ] **Step 3: Implement `kernel/mm/vmm_mmu.c` and wire `hal_vm_*` in `arch/{arm64,riscv,x86}/hal/hal_*.c`**

- [ ] **Step 4: Run `python3 tests/host/run_mm_vmm_test.py` and verify it passes**

Run: `python3 tests/host/run_mm_vmm_test.py`
Expected: PASS.

- [ ] **Step 5: Commit Task 6**

```bash
git add include/nos/hal_arch.h include/nos/mm.h kernel/mm/vmm_mmu.c arch/arm64/hal/hal_arm64.c arch/riscv/hal/hal_riscv.c arch/x86/hal/hal_x86_64.c kernel/CMakeLists.txt tests/host/test_mm_vmm.c tests/host/run_mm_vmm_test.py
git commit -m "MM: Implement MMU VMA manager, multi-level page tables, demand paging, and CoW"
```

---

### Task 7: Full Regression Suite, Multi-Board Build & QEMU STM32 Verification

**Files:**
- Modify: `tests/run_host_regressions.py`
- Modify: `configs/qemu-stm32_defconfig` (enable `CONFIG_MPU=y` to exercise MPU + Stack Guard in QEMU smoke test)

- [ ] **Step 1: Add `run_mm_tlsf_test.py`, `run_mm_slub_test.py`, `run_mm_mpu_test.py`, `run_mm_vmm_test.py` to `tests/run_host_regressions.py`**

- [ ] **Step 2: Run all host regressions and layer dependency checks**

Run:
```bash
python3 tests/run_host_regressions.py
python3 -m unittest discover -s tests -p 'test_*.py'
cmake -P tests/test_arch_flags.cmake
cmake -P tests/test_soc_headers.cmake
python3 scripts/check_layer_deps.py
```
Expected: All PASS with 0 errors.

- [ ] **Step 3: Build all 4 board presets (`nk60-v2`, `qemu-stm32`, `wl-001`, `zj-v3`) and run QEMU STM32 smoke test**

Run:
```bash
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
cmake --preset wl-001 && cmake --build --preset wl-001
cmake --preset zj-v3 && cmake --build --preset zj-v3
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```
Expected: Zero build warnings/errors, QEMU smoke test PASS.

- [ ] **Step 4: Commit Task 7**

```bash
git add tests/run_host_regressions.py configs/qemu-stm32_defconfig
git commit -m "Tests: Register MM redesign regression suite and enable MPU on qemu-stm32"
```
