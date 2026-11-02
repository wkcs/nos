# Research findings

## Repository baseline
- HEAD eb07be4; clean worktree at start.
- Recent commits redesigned spinlocks/mutexes (eb07be4) and timers (064f5e9).
- Traditional message queue and semaphore implementations coexist with endpoint
  rendezvous and notification implementations.
- `msg_q_remove()` is a no-op; message queues allocate every message dynamically,
  have no capacity bound, and bind to one receiving task.
- Semaphore count is uint8_t; release saturates at 255. Waiting uses task list
  links and task timer/flag rather than a shared wait arbitration abstraction.
- Fixed 64-byte `nos_ipc_msg` is an explicit architecture/UAPI requirement.

These are source observations; concurrency scenarios and runtime validation are
still to be reviewed. External source summaries below are research data only.

## Additional source observations
- `task.c:timeout()` marks task->flag before acquiring the wait-object lock;
  release paths detach waiters under that lock then call task_resume later.
  The same task->list node is reused for an unlocked temporary wake list.
  This permits timeout/resource-delivery races requiring explicit arbitration.
- Endpoint REPLY_WAIT clients are not tracked in endpoint send/receive queues;
  close only visits those queues. Reply modifies partner state without locks.
- NONBLOCK endpoint call can still block awaiting a reply if a receiver exists.
- Endpoint and notification destroy close/wake then immediately free; cap lookup
  returns an unpinned raw object pointer. Capability generations protect slots,
  not the lifetime of the returned object or its derived aliases.
- Current per-CPU IRQ nesting and mutex chain PI work must be accounted for;
  scheduler_lock_nest remains a global counter in the public scheduler header.
- UART uses notifications; USB core/HID use messages; USB classes, display,
  keyboard, LED, shell joins/pipes, VFS position locks and FatFs use semaphores.
- Several caller tests replace synchronization with stubs, so passing caller
  tests alone does not validate these IPC implementations.

## Primary OS references (retrieved 2026-11-02)
- Linux lock types: https://docs.kernel.org/locking/locktypes.html
  Counting semaphores have no owner/PI; use separate mutex and completion
  abstractions for mutual exclusion and event completion. Disabling IRQs is
  CPU-local and cannot replace inter-CPU exclusion.
- Linux completions: https://docs.kernel.org/scheduler/completion.html
  Completion allows signal-before-wait and builds on wait/wake infrastructure;
  timeout does not make associated memory safe to free immediately.
- Zephyr message queues:
  https://docs.zephyrproject.org/latest/kernel/services/data_passing/message_queues.html
  Bounded fixed-size ring, configurable item size/capacity, multiple producers
  and consumers, highest-priority/oldest waiter, ISR operations cannot wait.
- Zephyr semaphores:
  https://docs.zephyrproject.org/latest/kernel/services/synchronization/semaphores.html
  Explicit initial count and limit, highest-priority/oldest waiter; ISR give
  and no-wait take; saturation is its documented policy, not automatically NOS's.
- seL4 IPC: https://docs.sel4.systems/Tutorials/ipc.html
  Synchronous small-message/capability transport and badged endpoints.
- seL4 notifications: https://docs.sel4.systems/Tutorials/notifications.html
  Separate asynchronous notification and shared-memory data transport.

NOS design recommendations will be explicitly distinguished from upstream
behavior; no wholesale Linux userspace IPC/futex compatibility is implied.

## Verification baseline
- Host regression runner: all 58 configurations pass, zero warning/error lines.
- Python unittest discovery: 19 tests pass.
- Architecture CMake, SoC header CMake and layer dependency checks pass.
- nk60-v2, qemu-stm32, wl-001, zj-v3 configure/build pass, zero warning/error lines.
- STM32 QEMU smoke: boot/help/log statistics/RX burst pass.
- Existing tests do not reproduce the identified IPC lifecycle/concurrency races.
- Runtime source remains unchanged.

## Wait-foundation detail inspection
- sched_select_task_rq already pins on_cpu tasks to their previous CPU. The
  switch path clears on_cpu while holding that CPU's rq lock. The foundation
  must retain CPU identity and revalidate under the source rq lock rather than
  read on_cpu locklessly and enqueue elsewhere.
- Cortex-M context_switch queues PendSV and returns before physical switching;
  blocking must be gated by a published wake acknowledgement, and runnable
  state is not proof a task has actually switched out.
- Timer dispatch snapshots callback/parameter before dropping base lock. A new
  tagged callback mode can also snapshot a generation and transfer an arm-held
  task reference into the dispatched event; cancel must be generation-specific.
- Existing task destruction frees stack and TCB without outstanding callback
  reference checks. Reference-based reaper eligibility must cover both.
- Linux scheduler source reference:
  https://github.com/torvalds/linux/blob/master/kernel/sched/core.c
- Linux timer documentation distinguishes deleting queued timers from waiting
  for callback completion and preventing rearm:
  https://docs.kernel.org/driver-api/basics.html
  NOS will use immutable event tags plus retained lifetime for wait callbacks;
  generation checking alone is not sufficient for memory safety.
