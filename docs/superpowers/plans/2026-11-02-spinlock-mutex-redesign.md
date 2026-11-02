# NOS Spinlock and Mutex Redesign Implementation Plan

- **Date:** 2026-11-02
- **Spec:** `docs/superpowers/specs/2026-11-02-spinlock-mutex-redesign-design.md`

## Step 1: Architecture Spinlock Headers (`arch/*/include/asm/spinlock.h`)
- Unify `arch_spinlock_t` layout across `arm`, `arm64`, `riscv`, and `x86` (`union { u32 slock; struct { u16 owner; u16 next; } tickets; }`).
- Add `arch_spin_is_locked()` and `arch_spin_trylock()` across all 4 architectures.
- On `arm` (`arch/arm/include/asm/spinlock.h`), provide lightweight UP mode when `!defined(CONFIG_SMP) && (!defined(CONFIG_NR_CPUS) || CONFIG_NR_CPUS <= 1)` with `BUG_ON(arch_spin_is_locked(lock))` deadlock detection instead of infinite `wfe`.

## Step 2: Generic Spinlock Header (`include/kernel/spinlock.h`)
- Implement per-CPU (and host `_Thread_local`) IRQ spinlock nesting depth (`__nos_spin_irq_depth`) and outermost saved flags (`__nos_spin_irq_flags`) so both LIFO and non-LIFO nested `spin_lock_irq` / `spin_unlock_irq` keep interrupts disabled until the outermost IRQ spinlock is released.
- Fix `spin_unlock_irq` ordering so `sch_unlock()` runs before restoring outermost interrupts.
- Add `spin_trylock()`, `spin_trylock_irq()`, `spin_lock_irqsave()` / `spin_unlock_irqrestore()`, and `spin_is_locked()`.

## Step 3: Mutex Fast-Path, Timeout, Self-Deadlock Check & Chain PI (`include/kernel/mutex.h`, `kernel/ipc/mutex.c`, `include/kernel/task.h`)
- Add `struct mutex *blocked_on` to `struct task_struct`.
- Add `MUTEX(name)` static initializer macro, `mutex_lock_timeout(struct mutex *lock, uint32_t tick)`, and `mutex_is_locked(const struct mutex *lock)` to `include/kernel/mutex.h`.
- Implement O(1) uncontended fast-path in `mutex_unlock()` (skip `task_pi_unboost()` and `switch_task()` when `wait_list` is empty and `owner` is unboosted).
- Implement `mutex_lock_timeout()` with recursive self-deadlock check (`-EDEADLK`), timer-backed timeout (`-ETIMEDOUT`), handoff-vs-timeout race resolution (`lock->owner == task`), and PI rollback on timeout.
- Implement bounded transitive priority inheritance (`MUTEX_PI_MAX_DEPTH = 4`) across `task->blocked_on` chains.

## Step 4: Verification & Hardware Test
- Extend `tests/host/test_spinlock_irq.c` to test 2-CPU contention, LIFO nesting, non-LIFO (`unlock(A)` before `unlock(B)`) nesting, `spin_trylock`, and `spin_lock_irqsave`.
- Extend `tests/host/test_mutex.c` to test uncontended `mutex_unlock` fast-path (verifying 0 `switch_task()` calls), recursive self-deadlock detection (`-EDEADLK`), `mutex_lock_timeout()` expiration and PI unboost rollback, and 3-task / 2-mutex transitive Chain PI (`Task A -> Mutex 1 -> Task B -> Mutex 2 -> Task C`).
- Run all host regressions, layer dependency checks, all 4 board builds, QEMU smoke test, and flash `nk60-v2` hardware via `cmake --build --preset nk60-v2 --target flash`.
