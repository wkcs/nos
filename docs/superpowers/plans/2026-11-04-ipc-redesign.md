# NOS 消息队列、信号量与 IPC 全新架构实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 彻底重构 NOS 的等待底座、信号量、消息队列与微内核 IPC 模块；删除历史有缺陷的无界堆分配与 8 位信号量实现，实现支持 MCU 静态零堆与多核 SMP 并发安全的高性能原语，并全量迁移仓库调用方。

**Architecture:** 
1. **通用底座层**：提供 `nos_object` 原子引用计数与生命周期、`nos_wait_entry` / `nos_wait_queue` 单点状态机仲裁、Tagged Timer 代数截止时间与 `nos_wake_batch` 单向锁序延后唤醒；
2. **本地原语层**：提供定长环形缓冲有界 MPMC 消息队列 `nos_msgq`（零堆分配、双向背压、ISR 快速失败）、32 位计数信号量 `nos_sem`、完成量 `nos_completion`（广播 `complete_all`）、64 位位图通知 `nos_notify`；
3. **微内核 IPC 层**：保留 64 字节 `struct nos_ipc_msg` ABI，引入 `reply_queue` 与一次性安全 Reply 凭证，结合 Capability 表锁固定引用防 UAF；
4. **全仓库迁移与清理**：物理删除 `include/kernel/msg_queue.h`、`kernel/ipc/msg_queue.c`、`include/kernel/sem.h`、`kernel/ipc/sem.c`，迁移 USB、Display、Shell、VFS、UART、板级与测试代码。

**Tech Stack:** C99 / GNU11, CMake + Ninja, Host GCC / Clang, Arm GNU Toolchain 14.2.Rel1, QEMU ARM Cortex-M3/M4.

## Global Constraints

- 严禁在 `include/` 根目录下创建标准 C 库同名头文件。
- 内核与用户态共享 ABI 统一放在 `include/uapi/nos/*.h`，严禁改变 64 字节 `struct nos_ipc_msg` 大小。
- 微内核与服务内部接口放在 `include/nos/*.h`。
- 代码注释、文档与对话统一使用中文。
- 4 空格缩进，禁止 Tab，K&R 内核大括号风格。
- 加锁顺序严格遵循：`task->lock -> object->lock -> runqueue/timer 叶子锁`，严禁持对象锁反向获取任务锁。
- 运行时热路径（发送、接收、take、give）绝对零堆分配（Zero Heap Allocation）。

---

### Task 1: 通用超时与对象生命周期引用头 (`nos_timeout` & `nos_object` & `nos_refcount`)

**Files:**
- Create: `include/nos/timeout.h`
- Create: `include/nos/refcount.h`
- Create: `include/nos/object.h`
- Create: `kernel/ipc/timeout.c`
- Create: `kernel/ipc/object.c`
- Modify: `kernel/CMakeLists.txt`
- Test: `tests/host/test_ipc_object.c`

**Interfaces:**
- Consumes: `<kernel/spinlock.h>`, `<kernel/timer.h>`, `<uapi/nos/types.h>`
- Produces:
  - `NOS_NO_WAIT`, `NOS_WAIT_FOREVER`, `nos_timeout_t`
  - `nos_deadline_from_timeout(nos_timeout_t timeout, uint64_t *out_deadline)`
  - `nos_deadline_is_expired(uint64_t deadline)`
  - `nos_refcount_init`, `nos_refcount_get`, `nos_refcount_put`
  - `nos_object_init`, `nos_object_close`, `nos_object_is_closed`, `nos_object_retire`

- [ ] **Step 1: 编写宿主端超时与对象生命周期测试用例**

创建 `tests/host/test_ipc_object.c`，覆盖：
1. `timeout = 0` (NO_WAIT) 与 `UINT32_MAX` (WAIT_FOREVER) 转换；
2. 相对 tick 到单调 deadline 转换与到期检查；
3. `nos_refcount` 初始计数、get、put 及归零检测；
4. `nos_object_init`、`nos_object_close`（重复 close 幂等）和状态查询。

- [ ] **Step 2: 运行测试验证失败**

运行: `gcc -Iinclude -Itests/host/stubs tests/host/test_ipc_object.c -o /tmp/test_ipc_object && /tmp/test_ipc_object`
预期: 失败（头文件与实现尚未创建）。

- [ ] **Step 3: 实现 `nos/timeout.h`、`nos/refcount.h`、`nos/object.h` 与对应 `.c`**

实现：
1. `include/nos/timeout.h` & `kernel/ipc/timeout.c`：定义超时常量与单调时钟转换函数；
2. `include/nos/refcount.h`：封装 `atomic_t` 的 32 位原子引用计数；
3. `include/nos/object.h` & `kernel/ipc/object.c`：定义 `struct nos_object`，实现自旋锁初始化、close 标记与退休逻辑；
4. 更新 `kernel/CMakeLists.txt` 添加 `ipc/timeout.c` 和 `ipc/object.c`。

- [ ] **Step 4: 编译并运行测试验证通过**

运行: `gcc -Iinclude -Itests/host/stubs tests/host/test_ipc_object.c kernel/ipc/timeout.c kernel/ipc/object.c -o /tmp/test_ipc_object && /tmp/test_ipc_object`
预期: 输出全部测试用例 PASS。

- [ ] **Step 5: 提交更改**

```bash
git add include/nos/timeout.h include/nos/refcount.h include/nos/object.h kernel/ipc/timeout.c kernel/ipc/object.c kernel/CMakeLists.txt tests/host/test_ipc_object.c
git commit -m "IPC: Add universal timeout, refcount, and object lifecycle foundation"
```

---

### Task 2: 通用等待底座与单点仲裁状态机 (`nos_wait_entry`, `nos_wait_queue`, `nos_wake_batch`)

**Files:**
- Create: `include/nos/wait.h`
- Create: `kernel/ipc/wait.c`
- Modify: `include/kernel/task.h`
- Modify: `kernel/task/task.c`
- Modify: `kernel/CMakeLists.txt`
- Test: `tests/host/test_ipc_wait.c`

**Interfaces:**
- Consumes: `struct nos_object`, `nos_timeout_t`, `<kernel/task.h>`
- Produces:
  - `struct nos_wait_entry` (内嵌在 `struct task_struct` 中)
  - `struct nos_wake_batch` (延后唤醒批次)
  - `nos_wait_prepare(entry, obj, deadline)`
  - `nos_wait_finish(entry)`
  - `nos_wait_complete_locked(entry, result, batch)`
  - `nos_wake_batch_init`, `nos_wake_batch_dispatch`

- [ ] **Step 1: 编写通用等待底座状态机与延后唤醒测试**

创建 `tests/host/test_ipc_wait.c`，覆盖：
1. 状态流转：`IDLE -> WAITING -> SUCCEEDED / TIMEDOUT / CLOSED / CANCELLED`；
2. 线性化点测试：一旦 `SUCCEEDED` 提交，后续超时调用视为 no-op；
3. 延后唤醒测试：在释放 `object->lock` 后由 `wake_batch_dispatch` 安全唤醒任务。

- [ ] **Step 2: 运行测试验证失败**

运行: `gcc -Iinclude -Itests/host/stubs tests/host/test_ipc_wait.c -o /tmp/test_ipc_wait && /tmp/test_ipc_wait`
预期: 失败（头文件与实现未定义）。

- [ ] **Step 3: 实现 `nos/wait.h`、`kernel/ipc/wait.c` 并改造 `task_struct`**

实现：
1. 在 `include/kernel/task.h` 中的 `struct task_struct` 内嵌入 `struct nos_wait_entry wait_entry;` 与原子引用计数；
2. 在 `kernel/ipc/wait.c` 中实现单点裁决状态机与延后唤醒批次；
3. 更新 `kernel/CMakeLists.txt` 添加 `ipc/wait.c`。

- [ ] **Step 4: 编译并运行测试验证通过**

运行测试，验证单点裁决与延后唤醒逻辑 100% 正确通过。

- [ ] **Step 5: 提交更改**

```bash
git add include/nos/wait.h kernel/ipc/wait.c include/kernel/task.h kernel/task/task.c kernel/CMakeLists.txt tests/host/test_ipc_wait.c
git commit -m "IPC: Implement universal wait queue, wait entry state machine, and deferred wake batch"
```

---

### Task 3: 计数信号量 (`nos_sem`) 独立实现与单测

**Files:**
- Create: `include/nos/sem.h`
- Create: `kernel/ipc/sem.c`
- Modify: `kernel/CMakeLists.txt`
- Test: `tests/host/test_ipc_sem.c`

**Interfaces:**
- Consumes: `<nos/object.h>`, `<nos/wait.h>`, `<nos/timeout.h>`
- Produces:
  - `struct nos_sem`
  - `int nos_sem_init(struct nos_sem *sem, uint32_t initial, uint32_t limit)`
  - `int nos_sem_take(struct nos_sem *sem, nos_timeout_t timeout)`
  - `int nos_sem_trytake(struct nos_sem *sem)`
  - `int nos_sem_give(struct nos_sem *sem)`
  - `int nos_sem_close(struct nos_sem *sem)`

- [ ] **Step 1: 编写信号量测试用例**

创建 `tests/host/test_ipc_sem.c`，测试：
1. `initial` 与 `limit` 边界校验；
2. 正常 `give`/`take` 令牌守恒；
3. 超过 `limit` 时 `give` 报错 `-EOVERFLOW`（拒绝静默饱和）；
4. `trytake` 在无令牌时返回 `-EAGAIN`；
5. 多任务优先级排队与直接令牌交接唤醒；
6. `close` 时所有等待者被唤醒且返回 `-EPIPE`。

- [ ] **Step 2: 运行测试验证失败**

运行: `gcc -Iinclude -Itests/host/stubs tests/host/test_ipc_sem.c -o /tmp/test_ipc_sem && /tmp/test_ipc_sem`
预期: 失败（头文件未定义）。

- [ ] **Step 3: 实现 `include/nos/sem.h` 与 `kernel/ipc/sem.c`**

编写符合规范的 32 位计数信号量，严格遵守单向锁序与延后唤醒批次。

- [ ] **Step 4: 编译并运行测试验证通过**

运行测试并验证全部用例 PASS。

- [ ] **Step 5: 提交更改**

```bash
git add include/nos/sem.h kernel/ipc/sem.c kernel/CMakeLists.txt tests/host/test_ipc_sem.c
git commit -m "IPC: Implement 32-bit counting semaphore nos_sem adhering to wait foundation"
```

---

### Task 4: 完成量 (`nos_completion`) 独立实现与单测

**Files:**
- Create: `include/nos/completion.h`
- Create: `kernel/ipc/completion.c`
- Modify: `kernel/CMakeLists.txt`
- Test: `tests/host/test_ipc_completion.c`

**Interfaces:**
- Consumes: `<nos/object.h>`, `<nos/wait.h>`, `<nos/timeout.h>`
- Produces:
  - `struct nos_completion`
  - `int nos_completion_init(struct nos_completion *comp)`
  - `int nos_completion_wait(struct nos_completion *comp, nos_timeout_t timeout)`
  - `int nos_complete(struct nos_completion *comp)`
  - `int nos_complete_all(struct nos_completion *comp)`
  - `int nos_completion_reset(struct nos_completion *comp)`
  - `int nos_completion_close(struct nos_completion *comp)`

- [ ] **Step 1: 编写完成量测试用例**

创建 `tests/host/test_ipc_completion.c`，覆盖：
1. 单次 `complete` 唤醒单个等待者；
2. 广播 `complete_all` 唤醒所有活动等待者，且后续进入的 `wait` 立即无阻塞返回 0；
3. `reset` 在无等待者时重置成功，在有等待者时返回 `-EBUSY`；
4. `close` 唤醒所有等待者返回 `-EPIPE`。

- [ ] **Step 2: 运行测试验证失败**

- [ ] **Step 3: 实现 `include/nos/completion.h` 与 `kernel/ipc/completion.c`**

- [ ] **Step 4: 编译并运行测试验证通过**

- [ ] **Step 5: 提交更改**

```bash
git add include/nos/completion.h kernel/ipc/completion.c kernel/CMakeLists.txt tests/host/test_ipc_completion.c
git commit -m "IPC: Implement nos_completion for one-shot and broadcast event synchronization"
```

---

### Task 5: 有界定长消息队列 (`nos_msgq`) 独立实现与单测

**Files:**
- Create: `include/nos/msgq.h`
- Create: `kernel/ipc/msgq.c`
- Modify: `kernel/CMakeLists.txt`
- Test: `tests/host/test_ipc_msgq.c`

**Interfaces:**
- Consumes: `<nos/object.h>`, `<nos/wait.h>`, `<nos/timeout.h>`
- Produces:
  - `struct nos_msgq`
  - `int nos_msgq_init(struct nos_msgq *q, void *buffer, size_t buffer_bytes, size_t element_size, uint32_t capacity)`
  - `int nos_msgq_send(struct nos_msgq *q, const void *element, size_t size, nos_timeout_t timeout)`
  - `int nos_msgq_recv(struct nos_msgq *q, void *element, size_t size, nos_timeout_t timeout)`
  - `int nos_msgq_close(struct nos_msgq *q)`

- [ ] **Step 1: 编写消息队列测试用例**

创建 `tests/host/test_ipc_msgq.c`，覆盖：
1. 缓冲区大小算术溢出校验；
2. 元素大小精确匹配校验（发送大小不一致返回 `-EINVAL`，接收缓冲区过小返回 `-EMSGSIZE`）；
3. 环形队列先进先出（FIFO）数据完整性；
4. 队列满时阻塞发送与队列空时阻塞接收（双向背压）；
5. 非阻塞与 ISR 快速失败（队列满发返回 `-ENOSPC` / `-EAGAIN`，队列空读返回 `-EAGAIN`）；
6. MPMC 多任务并发读写与 `close` 退出。

- [ ] **Step 2: 运行测试验证失败**

- [ ] **Step 3: 实现 `include/nos/msgq.h` 与 `kernel/ipc/msgq.c`**

严格保证热路径零堆分配，内部使用静态环形缓冲索引与双等待队列（`wait_send`, `wait_recv`）。

- [ ] **Step 4: 编译并运行测试验证通过**

- [ ] **Step 5: 提交更改**

```bash
git add include/nos/msgq.h kernel/ipc/msgq.c kernel/CMakeLists.txt tests/host/test_ipc_msgq.c
git commit -m "IPC: Implement bounded fixed-element message queue nos_msgq with zero heap allocation"
```

---

### Task 6: 64 位异步位图通知 (`nos_notify`) 实现与单测

**Files:**
- Create: `include/nos/notification.h`
- Create: `kernel/ipc/notification.c`
- Modify: `kernel/CMakeLists.txt`
- Test: `tests/host/test_ipc_notify.c`

**Interfaces:**
- Consumes: `<nos/object.h>`, `<nos/wait.h>`, `<nos/timeout.h>`
- Produces:
  - `struct nos_notification`
  - `int nos_notification_init(struct nos_notification *notif)`
  - `int nos_notify_signal_obj(struct nos_notification *notif, uint64_t bits)`
  - `int nos_notify_wait_obj(struct nos_notification *notif, uint64_t *out_bits, nos_timeout_t timeout)`
  - `int nos_notification_close(struct nos_notification *notif)`

- [ ] **Step 1: 编写异步位图通知测试用例**

覆盖按位或累加、同位事件合并、等待并原子读清（Read-and-Clear）、超时和关闭。

- [ ] **Step 2: 运行测试验证失败**

- [ ] **Step 3: 实现 `include/nos/notification.h` 与 `kernel/ipc/notification.c`**

- [ ] **Step 4: 编译并运行测试验证通过**

- [ ] **Step 5: 提交更改**

```bash
git add include/nos/notification.h kernel/ipc/notification.c kernel/CMakeLists.txt tests/host/test_ipc_notify.c
git commit -m "IPC: Implement 64-bit atomic bitmask notification nos_notify for IRQ signaling"
```

---

### Task 7: 微内核同步 Endpoint IPC 事务与安全 Reply 机制

**Files:**
- Modify: `include/nos/ipc.h`
- Modify: `kernel/ipc/endpoint.c`
- Modify: `kernel/cap/cap_table.c`
- Test: `tests/host/test_ipc_endpoint.c`

**Interfaces:**
- Consumes: `<uapi/nos/ipc.h>`, `<nos/object.h>`, `<nos/wait.h>`
- Produces:
  - `struct nos_endpoint` (带 `reply_queue`)
  - `nos_endpoint_call`
  - `nos_endpoint_recv`
  - `nos_endpoint_reply_wait`
  - `nos_endpoint_close` (遍历 `send`, `recv`, `reply_queue`)
  - Capability 表查找固定引用 (`nos_cap_lookup` 增引用)

- [ ] **Step 1: 编写微内核 Endpoint 同步 RPC 与在途 Reply 凭证测试**

覆盖：
1. 正常 Client-Server 同步调用与优先级继承；
2. 客户端超时时，服务端回复安全返回 `-ETIMEDOUT`，不写已失效内存；
3. Endpoint 关闭时，`reply_queue` 中等待回复的客户端被安全唤醒返回 `-EPIPE`；
4. Capability 查找递增引用，并发销毁不引发悬空访问。

- [ ] **Step 2: 运行测试验证失败**

- [ ] **Step 3: 重构 `include/nos/ipc.h`、`kernel/ipc/endpoint.c` 与 `cap_table.c`**

- [ ] **Step 4: 编译并运行测试验证通过**

- [ ] **Step 5: 提交更改**

```bash
git add include/nos/ipc.h kernel/ipc/endpoint.c kernel/cap/cap_table.c tests/host/test_ipc_endpoint.c
git commit -m "IPC: Redesign synchronous endpoint IPC with in-flight reply_queue and pinned capability lookup"
```

---

### Task 8: 物理删除旧实现，全面重构迁移仓库现有调用方

**Files:**
- Delete: `include/kernel/msg_queue.h`
- Delete: `kernel/ipc/msg_queue.c`
- Delete: `include/kernel/sem.h`
- Delete: `kernel/ipc/sem.c` (原旧实现)
- Modify:
  - `drivers/usb/device/core/core.c` (迁移至 `nos_msgq`)
  - `drivers/usb/device/class/hid.c` (迁移至 `nos_msgq`)
  - `drivers/usb/device/class/cdc.c` (迁移至 `nos_completion`)
  - `drivers/usb/device/class/winusb.c` (迁移至 `nos_completion`)
  - `drivers/display/` & `app/server/display/` & `app/display/` (迁移至 `nos_completion` / `nos_notify`)
  - `app/shell/nshell.c`, `app/shell/pipe.h` (迁移至 `nos_completion`)
  - `fs/fatfs/sync.c`, `fs/vfs_io.c` (迁移至 `mutex`)
  - `board/nk60-v2/led.c` (迁移至 `nos_notify`)
  - `app/tests/test.c`, `app/tests/fatfs_test.c` (迁移至新 API)

- [ ] **Step 1: 物理删除旧文件**

```bash
git rm include/kernel/msg_queue.h kernel/ipc/msg_queue.c
```

- [ ] **Step 2: 迁移 USB Core 与 HID 驱动到 `nos_msgq`**

在 `core.c` 与 `hid.c` 中定义静态事件缓冲区，替换旧 `msg_q_init`、`msg_q_send`、`msg_q_recv` 为 `nos_msgq_init`、`nos_msgq_send`（ISR 中传入 `NOS_NO_WAIT`）、`nos_msgq_recv`。

- [ ] **Step 3: 迁移 USB CDC/WinUSB 与显示驱动到 `nos_completion` / `nos_notify`**

将旧 `sem_t` 传输同步替换为 `nos_completion`，帧同步替换为 `nos_notify`。

- [ ] **Step 4: 迁移 Shell、VFS/FatFs 与测试程序**

Shell 子进程 join 迁移至 `nos_completion_wait`/`nos_complete_all`；VFS/FatFs 互斥迁移至 `mutex`；更新测试用例。

- [ ] **Step 5: 提交更改**

```bash
git add -u
git commit -m "IPC: Migrate all repository callers to modern IPC primitives and remove legacy implementations"
```

---

### Task 9: 全系统回归测试与板级固件验证

**Files:**
- Modify: `tests/run_host_regressions.py` (注册所有新 IPC 宿主测试)

- [ ] **Step 1: 运行全量主机回归测试套件**

运行: `python3 tests/run_host_regressions.py`
预期: 全部回归测试 PASS，无任何报错。

- [ ] **Step 2: 运行 Python 单元测试发现**

运行: `python3 -m unittest discover -s tests -p 'test_*.py'`
预期: 全部 unittest PASS。

- [ ] **Step 3: 运行架构标志与分层依赖检查**

运行:
```bash
cmake -P tests/test_arch_flags.cmake
cmake -P tests/test_soc_headers.cmake
python3 scripts/check_layer_deps.py
```
预期: 全部无违规、无架构层级泄漏。

- [ ] **Step 4: 构建四个板级固件预设**

运行:
```bash
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset wl-001 && cmake --build --preset wl-001
cmake --preset zj-v3 && cmake --build --preset zj-v3
```
预期: 四个固件全部 0 错误、0 警告构建成功。

- [ ] **Step 5: 执行 QEMU STM32 冒烟测试**

运行: `python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf`
预期: 引导成功，nShell 交互、日志统计和 RX 突发测试全部通过。

- [ ] **Step 6: 提交最终验证结果**

```bash
git add tests/run_host_regressions.py
git commit -m "IPC: Register new IPC regressions and verify complete system builds"
```
