# NOS Spinlock and Mutex Subsystem Redesign Specification

- **Date:** 2026-11-02
- **Author:** Nick Hu / Codex
- **Target Modules:** `include/kernel/spinlock.h`, `arch/*/include/asm/spinlock.h`, `include/kernel/mutex.h`, `kernel/ipc/mutex.c`, `include/kernel/task.h`, `kernel/task/task.c`

---

## 1. Problem Diagnosis

### 1.1 Spinlock Issues (`include/kernel/spinlock.h`, `arch/*/include/asm/spinlock.h`)
1. **Non-LIFO Nested `spin_lock_irq` Interrupt Premature Enable / Permanent Disable Bug:**
   - Storing `irq_level` solely per-lock (`lock->irq_level`) causes critical failure when two locks are acquired via `spin_lock_irq(A); spin_lock_irq(B);` and released out-of-order (`spin_unlock_irq(A); spin_unlock_irq(B);`):
     - Unlocking `A` restores `A->irq_level` (`0`, IRQs enabled) while `B` is still held.
     - Unlocking `B` restores `B->irq_level` (`1`, IRQs disabled), leaving hardware interrupts permanently disabled after all locks are released.
2. **Inverted `sch_unlock()` vs `enable_irq_save()` Order in `spin_unlock_irq()`:**
   - `__UNLOCK_IRQ` previously invoked `sch_unlock()` before `enable_irq_save()`.
3. **Uniprocessor (`CONFIG_NR_CPUS == 1`) Ticket-Lock WFE Hang & Overhead:**
   - `arch/arm/include/asm/spinlock.h` executed `ldrex/strex` + `dsb 0xF` + `wfe` even on single-core Cortex-M targets. On UP with interrupts disabled, a double-lock bug enters `wfe` forever instead of triggering `BUG_ON` fault diagnostics.
   - `arch/riscv/include/asm/spinlock.h` and `arch/x86/include/asm/spinlock.h` lacked the `.tickets` union layout expected by `include/kernel/spinlock.h`.
4. **Missing Standard Primitives:**
   - Missing `spin_trylock()`, `spin_trylock_irq()`, `spin_lock_irqsave()` / `spin_unlock_irqrestore()`, and `spin_is_locked()`.

### 1.2 Mutex Issues (`include/kernel/mutex.h`, `kernel/ipc/mutex.c`)
1. **Unconditional `switch_task()` & `task_pi_unboost()` on Uncontended `mutex_unlock()`:**
   - Even when `lock->wait_list` was empty and `owner` had no active PI boost, `mutex_unlock()` called `sch_lock()`, `task_pi_unboost()`, `sch_unlock()`, and `switch_task()`.
2. **Missing Wakeup Ownership Verification & `mutex_lock_timeout()`:**
   - `mutex_lock()` did not verify `lock->owner == task` after waking from `switch_task()`.
   - No `mutex_lock_timeout(struct mutex *lock, uint32_t tick)` API existed, and recursive self-locking (`lock->owner == current`) caused silent self-deadlock.
3. **Missing Transitive (Chain) Priority Inheritance:**
   - Priority inheritance only boosted the immediate `lock->owner` by one hop (`Waiter A -> Mutex 1 -> Owner B`). If `Owner B` was blocked on `Mutex 2` held by `Owner C`, `Mutex 2`'s wait-list order and `Owner C`'s priority were not updated.

---

## 2. Architecture & Design

### 2.1 Per-CPU IRQ Spinlock Nesting Tracker & Unified `arch_spinlock_t`
- **Per-CPU IRQ Spinlock Depth & Outermost Flags:**
  - Maintain per-CPU `irq_depth` and `irq_flags` (using `_Thread_local` in multi-threaded host tests and per-CPU array in kernel builds).
  - On `spin_lock_irq()` / `spin_lock_irqsave()`:
    - `flags = disable_irq_save()` disables local interrupts first.
    - Acquire `sch_lock()` and `arch_spin_lock()`.
    - If `irq_depth == 0`, record `irq_flags = flags`; increment `irq_depth`.
  - On `spin_unlock_irq()` / `spin_unlock_irqrestore()`:
    - Release `arch_spin_unlock()`.
    - Decrement `irq_depth`; only when `irq_depth == 0`, restore the outermost `irq_flags` after `sch_unlock()`.
    - Guarantees interrupts stay disabled across both LIFO and non-LIFO nested `spin_lock_irq` pairs and are properly restored when the last lock is dropped.
- **UP vs SMP Adaptive `arch_spinlock_t`:**
  - All 4 architectures (`arm`, `arm64`, `riscv`, `x86`) expose a uniform `arch_spinlock_t` union (`slock` + `tickets.{owner, next}`) and `arch_spin_lock`, `arch_spin_unlock`, `arch_spin_trylock`, `arch_spin_is_locked`.
  - On ARM Cortex-M with `CONFIG_NR_CPUS == 1` (`!CONFIG_SMP`), `arch_spin_lock` checks `BUG_ON(arch_spin_is_locked(lock))` and updates `tickets.next` without `wfe` / `dsb_sev`.

### 2.2 Mutex Fast-Path, Timeout, Self-Deadlock Guard, and Bounded Chain PI
- **O(1) Uncontended Fast-Path:**
  - `mutex_unlock()` checks under `lock->lock` if `list_empty(&lock->wait_list)`. If empty and `owner` is not PI-boosted (`owner->pi_boost_prio == TASK_PRIO_INVALID && owner->current_priority == owner->init_priority`), it releases `lock->lock` and returns immediately without calling `task_pi_unboost()` or `switch_task()`.
- **`mutex_lock_timeout(struct mutex *lock, uint32_t tick)`:**
  - `tick == 0`: wait forever (used by `mutex_lock()`).
  - `tick > 0`: arms `timer_start(&task->timer, tick)` before `switch_task()`.
  - Upon wakeup, acquires `lock->lock` and checks `if (lock->owner == task)`:
    - If `lock->owner == task` (handoff succeeded, even if raced with timer expiration), clears `task->flag = 0` and `task->blocked_on = NULL`, stops `task->timer`, and returns `0`.
    - Otherwise removes `task->list` from `lock->wait_list` if still linked, clears `task->blocked_on = NULL`, recomputes PI on `lock->owner`, and returns `-ETIMEDOUT`.
- **Recursive Self-Deadlock Detection:**
  - If `lock->locked && lock->owner == task`, `mutex_lock_timeout()` logs an error and returns `-EDEADLK` instead of hanging `task` on its own wait-list.
- **Bounded Chain Priority Inheritance (`MUTEX_PI_MAX_DEPTH = 4`):**
  - `struct task_struct` records `struct mutex *blocked_on` while blocked on a mutex.
  - When a waiter blocks on `lock` with priority higher than `owner->current_priority`, `mutex_propagate_pi()` walks up to `MUTEX_PI_MAX_DEPTH` hops along `cur_owner->blocked_on`, boosting each owner and re-sorting `cur_owner` in `next_mutex->wait_list`.
