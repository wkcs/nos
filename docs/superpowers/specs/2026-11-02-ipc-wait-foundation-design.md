# NOS IPC 重设计：第一阶段等待与生命周期底座

- 日期：2026-11-02
- 基线：`eb07be4`
- 状态：总体架构与三阶段顺序已获用户批准；本文为待书面评审的第一阶段设计规范。
- 前置审查：[IPC 审查及总体方案](../reviews/2026-11-02-ipc-audit.md)。
- 适用对象：单核 MCU 和共享一致性内存的 SMP CPU；本轮 SMP 验收使用主机多线程，真实多核 QEMU 后续专项验收。

## 1. 目标、分期和边界

用户已同意采用方案 A：共用等待与生命周期机制，分别提供有界消息队列、计数信号量、完成事件、位图通知和同步 Endpoint。允许修改 API，并同步迁移仓库内调用方。

| 阶段 | 内容 | 完成边界 |
| --- | --- | --- |
| 1，本文 | 等待状态机、延后唤醒、任务引用、带代数的超时事件、per-CPU 抢占计数，以及 mutex 接入 | 底座和 mutex 路径的并发协议通过测试，其他旧 IPC 尚待迁移 |
| 2 | semaphore/completion/notification/有界 msgq，迁移所有设备、shell、VFS/FatFs 调用方 | 本地同步与数据传递使用统一底座，删除相应旧接口 |
| 3 | Endpoint 请求事务、reply 凭证、Capability 生命周期/原子传输、用户拷贝边界及 libnos 迁移 | RPC 与本地 IPC 完成共同生命周期协议 |

各阶段有独立 spec、plan、实现和验收。第一阶段不能宣称旧 Endpoint 或整个操作系统已经 SMP 安全。

第一阶段不增加 POSIX/SysV 消息队列、futex、通用无锁 MPMC、CPU 热插拔、非一致性缓存支持、硬件 IPI 或新的 CPU 启动流程。不改变 64 字节 `nos_ipc_msg` UAPI。

## 2. 必须成立的不变量

1. 一次阻塞操作只有一个完成结果：成功、超时、关闭或取消。
2. 资源交付和成功结果在同一个对象临界区内提交；不能先移出等待队列，再由超时路径决定是否成功。
3. 同一操作最多发布一次调度唤醒；终态不会被后到的超时或关闭覆盖。
4. 已发布的旧回调不能影响该任务的后续等待。
5. 任务栈、TCB 和对象存储在最后一个在途使用者释放引用前保持有效。
6. 等待节点、调度节点、延后唤醒节点和僵尸回收节点互不复用。
7. CPU 本地禁止抢占及关中断不替代跨 CPU 的对象锁。
8. 同一任务不能在两个 CPU 上同时执行；准备睡眠和真正切换之间的唤醒不得丢失。
9. ISR 不执行阻塞等待、不等待其他 CPU 上的回调结束，也不直接回收对象。
10. IRQ 恢复不得发生在仍持有其他 IRQ 自旋锁时；已有 spinlock 嵌套语义保持成立。

## 3. 模块职责和内部接口

新增内部头文件使用 `include/nos/`，不放入 UAPI。接口名称是实现计划的契约，字段布局属于内部实现，不构成二进制兼容承诺。

| 文件 | 职责 |
| --- | --- |
| `include/nos/timeout.h` | 具名 timeout、有限超时校验和绝对 deadline 转换 |
| `include/nos/wait.h`、`kernel/ipc/wait.c` | 等待记录、队列、唯一完成裁决、唤醒批次和调用约束 |
| `include/nos/object.h`、`kernel/ipc/object.c` | 对象引用、关闭/退休状态及延后回收框架 |
| `include/kernel/task.h`、`kernel/task/task.c` | 等待记录嵌入、task_get/put、取消和 reaper 条件 |
| `include/nos/sched.h`、`kernel/sched/sched.c` | 准备阻塞、park、带代数唤醒和 runqueue 衔接 |
| `include/kernel/timer.h`、`kernel/timer.c` | 带不可变代数及引用所有权的单次 deadline 事件 |
| `include/kernel/sched.h`、`kernel/irq/irq.c` | CPU 本地抢占/IRQ 上下文与可阻塞条件检查 |
| `include/kernel/mutex.h`、`kernel/ipc/mutex.c` | 第一种真实资源原语接入；保留 owner、交接和有界链式 PI |

另外为旧 sem/msgq/endpoint/notification 增加最小的任务引用和延后唤醒节点适配；它们的公开 API、资源语义及完整等待迁移仍在第二/三阶段处理。兼容适配属于内存安全边界，不能仅修改 mutex 后继续让旧回调或唤醒批次访问已回收任务。

等待底座不认识消息格式、令牌计数、reply 权限或 Capability 表。原语提供对象锁、资源交付及取消清理函数；底座提供等待和终态规则。底座不自行获取任意原语锁或执行可能睡眠的回调。

### 3.1 超时

新的内部 `nos_timeout_t` 使用 32 位 tick 值：

- `NOS_NO_WAIT = 0`；不能立即完成则 `-EAGAIN`，不创建等待记录和定时器。
- `NOS_WAIT_FOREVER = UINT32_MAX`；不注册 deadline 事件。
- `1 .. UINT32_MAX - 1` 为有限相对 tick。
- 入口只读取一次单调 tick，校验加法溢出，生成 64 位绝对 deadline；多次检查或锁竞争重试不重新计算预算。
- 资源获取/交付路径取得对象锁后若发现有限 deadline 已到，先提交超时，不再交付资源；已提交的成功不被后来的超时覆盖。成功线性化点取对象锁内的 deadline 检查及资源提交。关闭和取消不追溯时间先后，与超时按锁内第一个终态提交者决定结果。
- 无当前任务、idle、IRQ、预先持有自旋锁或禁止抢占时，阻塞型入口返回 `-EWOULDBLOCK`。显式 no-wait 的原语按其 ISR 契约执行。

第一阶段保留现有 `mutex_lock_timeout(lock, tick)` 的 `0=无限` 兼容包装，并提供使用新 timeout 的 `mutex_acquire()`。其他旧 IPC 的 timeout 常量保持现状，第二/三阶段按模块整体迁移，禁止只替换常量导致语义反转。

这里的单调 tick 必须经统一读取接口取得：32 位目标上用短暂 IRQ 保护读取，SMP 测试使用同步的逻辑时钟。真实多核时钟源和各 CPU tick 汇总属于后续平台验收，不能依赖裸读共享 64 位值。

### 3.2 等待记录

每个任务内嵌一个 `nos_wait_context`；同一任务同时只允许一个阻塞操作。核心字段包括：

- 独立的 `wait_node` 和 `wake_node`。
- 64 位非零 `epoch`，任务每次开始新等待时递增；禁止回绕复用，耗尽返回 `-EOVERFLOW`。
- 被等待对象的固定引用、队列及保护该状态的对象锁。
- `IDLE / WAITING / SUCCEEDED / TIMEDOUT / CLOSED / CANCELLED` 和返回结果。
- deadline、等待种类及调试用 wchan。
- 对象锁保护的排序快照：调度等级、有效优先级、FIFO 到达顺序。
- 原语自有、已初始化的数据指针或小型结果记录；跨地址空间数据先复制到内核，底座不访问用户指针。

任务锁保护记录的安装、活动身份和重用；活动记录的队列、排序、资源结果及终态由对象锁保护。访问这些字段时必须有相应锁，不能以 `volatile` 代替同步。

终态不立即复位。调用方在收到本代唤醒确认、停止本代 deadline 并取得最终结果后，才将记录转为 IDLE。已经脱离该记录的旧回调可以继续持有任务引用，但不得再读取被重用记录的旧字段。

### 3.3 对象和任务引用

`nos_object` 内置对象自旋锁，并提供 owner 引用、操作引用、关闭标记、存储类型及退休节点；新原语内嵌该头部，所有资源状态和等待队列由同一把对象锁保护，不为 close 再引入反向嵌套的独立锁。mutex 的既有 lock 字段迁移为这个锁，静态初始化宏同步调整。

引用使用带溢出检查的 32 位原子操作；从有效引用或受保护的注册表取得新引用，禁止对不受保护的裸指针执行 get。引用为零后不可复活。最后一个 put 只安排退休，不在 IRQ 或持锁上下文运行析构。

- owner 在初始化后持有一份引用；进入操作、排队等待和异步工作分别持有明确的引用。
- `close` 拒绝新等待，并在同一对象锁内完成所有 WAITING 记录；它本身不释放存储。
- `retire` 要求已经 close，释放 owner 引用；最后一个引用在任务上下文触发析构。
- 静态对象不调用 `kfree()`；退休完成表示调用方可回收或重新初始化存储。
- `close` 不回滚已经提交的 SUCCESS；成功和关闭按对象锁内的提交顺序排列。

`task_get/put` 同样采用 32 位引用。任务管理器/reaper 保留基础引用；等待注册、唤醒批次、已排队或已派发的 deadline 事件各持有所需引用。任务在 ZOMBIE、已离开执行 CPU、无等待/唤醒链表关系且只剩 reaper 引用时，才能释放栈、Capability 表和 TCB。

本阶段新增对象框架供后续 IPC 接入，不将现有 Capability 裸查找冒充固定引用。第三阶段必须提供表锁内查找并取引用的接口，才允许动态 Endpoint/Notification 使用退休框架。

## 4. 状态机和数据流

### 4.1 准备等待

1. 原语在入口检查参数和上下文，持有对象操作引用，计算本次 deadline。
2. 获取当前任务锁，再获取对象锁，检查对象状态并尝试立即取得资源。
3. 资源不可用且允许阻塞时，安装新 epoch 和 WAITING 记录，固定对象/任务引用，将专用等待节点入队。
4. 在任务锁及对象锁保护下将任务从 runqueue 摘除，标记对应等待状态并记录 wchan；有限等待注册本代 tagged deadline。
5. 若注册失败，在仍受保护时退回入队和阻塞状态，撤销已取得引用。不得遗留“没有定时器却等待有限超时”的任务。
6. 释放对象锁和任务锁，再进入 `nos_wait_park(task, epoch)`。

资源尝试和等待入队在同一个对象临界区完成，不存在“观察资源为空，解锁，再排队”的窗口。

### 4.2 成功、超时和关闭

原语释放/发送路径持对象锁选择候选等待者：先校验 WAITING 和 deadline，然后交付资源、写入结果、转为 SUCCEEDED、移出 wait_node 并加入本操作的延后唤醒批次。成功提交以后，其他路径不能再交付第二份资源或改写结果。

超时使用相同对象锁和 epoch 校验，将 WAITING 变为 TIMEDOUT；关闭变为 CLOSED，任务取消变为 CANCELLED。均通过相同的唯一完成函数摘链和生成唤醒票据。失败的竞争者不摘链、不修改结果、不发布第二次唤醒。

返回码：成功为 0，有限超时为 `-ETIMEDOUT`，关闭为 `-EPIPE`，取消为 `-ECANCELED`；无效参数为 `-EINVAL`，重复阻塞为 `-EBUSY`。原语可以在资源结果字段保存成功返回值，等待底座只提供完成状态。

### 4.3 延后唤醒与提前唤醒握手

唤醒分成“对象完成”和“调度确认”两个步骤，禁止仅检查终态便返回等待。

1. 对象锁内完成记录时，唤醒批次取得任务引用，并记录本代 epoch。只有成功完成者能加入该记录的 wake_node。
2. 释放对象锁后，批次先摘掉 wake_node，将 task/epoch 复制到局部票据；再取消该代排队 deadline。
3. 获取任务锁，校验活动 epoch 且任务没有退出，将 `wake_epoch` 更新为本代；按调度协议使该任务可运行。
4. 调度确认发布后，批次不再读取等待记录和 wake_node，仅释放其局部持有的引用。

`nos_wait_park()` 只根据任务锁保护的本代 `wake_epoch` 决定是否还要睡眠。即使对象结果已经终态，只要调度确认尚未发布，仍等待对应唤醒批次。这样保证快速运行的等待者不会在旧 wake_node 尚被遍历时复用同一记录。

若唤醒发生在 park 前，park 看见本代确认后直接进入 finish；若 park 先执行，后续确认将任务恢复就绪。二者都不重新写 WAITING，也不再次注册整个超时预算。

唤醒批次必须在原语返回或 IRQ 返回前完成调度确认；ISR 仅入队/请求 reschedule，不执行 `switch_task()`。批次数据不依赖已经返回函数的栈地址。

### 4.4 finish 和记录重用

调用方确认本代已获调度唤醒，持任务锁及对象锁读取最终结果，清理活动指针和 wchan，再将记录转为 IDLE；锁外释放该操作的对象/任务引用。

旧 tagged 回调只持局部 task/epoch，进入后重新检查活动身份；不匹配立即释放自己的引用并结束。它不通过旧对象指针访问新的等待。

成功是资源消耗的线性化点。任务之后被终止，不自动回滚已经消费的信号量或消息；应用需要自己的事务/资源恢复协议。mutex 作为有 owner 的对象另有退出限制，见第 8 节。

## 5. 锁顺序和内存可见性

唯一允许的嵌套方向是：

`任务锁（多个时按稳定 ID 递增） → 单个等待对象锁 → 单个 runqueue 锁或 timer-base 锁`

runqueue 锁、timer-base 锁和兼容唤醒队列锁是分离的叶子，彼此不嵌套。定时器 dispatcher 先释放 base 锁再进入回调。全局任务列表、close/reaper 列表的锁不能被带入对象、调度、回调及析构操作。

具体约束：

- 对象锁内的完成者仅修改对象保护的数据和独占 wake_node，不获取目标任务锁；调度唤醒在解锁后进行。
- 超时/取消者持任务锁，固定活动对象引用，再取对象锁并检查 epoch；已经取得任务锁但尚未取得对象锁，不得预先写超时结果。
- 单独持对象锁时可快照并固定候选任务引用；如后续需要两个任务锁，必须先释放对象锁，再按顺序获取任务锁并重取对象锁，重新验证候选身份和对象版本。
- task_set_prio/PI 改变活动等待的排序时按任务锁→对象锁更新快照；选择者不裸读其他任务的 priority。
- 不在持对象锁时调用现有 `task_resume()` 或 `task_pi_boost()`；这些接口会取得任务锁，必须改走解锁后的专用操作。
- 引用最后一次释放、任意可能分配/睡眠的操作和用户内存访问均在以上锁外进行。

跨 CPU 发布依赖自旋锁的 acquire/release 和明确的 32 位原子引用操作；64 位 epoch/deadline 只在锁内读写或在 dispatcher 内复制为局部值。MCU 不要求 64 位 lock-free 原子指令，也不引入 `libatomic` 链接依赖。

第一版等待队列采用紧凑链表。实时任务按有效优先级，普通任务按到达顺序，实时任务优先于普通任务；同级 FIFO，低等级任务可能被持续高等级负载延迟，这是明确策略。队列入队/重新排序是 O(N)，不声称整个 IPC 为 O(1)。

## 6. Tagged deadline 与定时器引用

保持现有普通 timer API；增加内部 tagged one-shot 模式及按 tag 取消接口，供新等待底座使用。每个 task 另有一个专用等待 deadline timer，旧 `task.timer` 暂留给尚未迁移的旧阻塞路径。

选择专用 timer 是为了使新旧回调、参数和引用模式互不混淆。第二/三阶段完成旧阻塞路径迁移后可以合并存储，但本阶段不能通过共用同一可变回调参数来节省内存。

### 6.1 入队和派发

- arm 使用绝对 deadline，固定一个 task 引用，保存 task、epoch、tagged 回调及引用释放操作。
- 到期时在 base 锁内摘链，并把回调、task、epoch 和释放操作全部快照到 dispatcher 的局部事件；排队引用的所有权转交给局部事件。
- 解开 base 锁后调用 tagged 回调；回调结束释放局部 task 引用。派发后不从可被重装的 timer 字段重新读取 epoch。
- 旧回调执行时允许下次等待重装同一个专用 timer：旧事件的 tag/引用已经完全独立。dispatcher 在调用回调后不得再访问可能已被退休的 timer 存储。

### 6.2 取消

- `cancel_tag(timer, epoch)` 只摘除匹配本代且仍排队的事件；不能误删刚注册的新代 timer。
- 若事件已派发，取消不等待它结束。旧事件靠代数校验失效，靠其 task 引用保证内存有效。
- 取消函数报告是否摘除了排队事件，并把对应引用归还给锁外的释放者；不在 base 锁下执行最后一次 put。
- 同一专用 timer 固定在任务创建时选定的 timer base，第一阶段不跨 base 迁移。可唤醒其他 CPU 的任务；硬件远端定时器编程另属平台工作。

旧 timeout 回调取得任务锁后，若发现任务正在执行新的 managed wait，必须直接返回，不能按旧 task.status 修改本代结果。旧 `task_resume()` 对 managed wait 也必须拒绝直接操作，调用方必须使用带 epoch 的完成路径。

旧 timer 的并发回收仍需被纳入 reaper：所有指向任务的旧 timer arm/dispatch 也持有 task 引用，并由任务定时器包装层捕获 legacy 等待代数；保持旧回调签名，但只在其代数仍匹配时转发到旧处理逻辑。这样既不让旧 IPC 声称完成新协议迁移，也不允许它的回调破坏新 mutex、后续 legacy 等待或任务内存。

## 7. 调度、per-CPU 状态与上下文

### 7.1 CPU 本地状态

移除 `scheduler_lock_nest` 的共享普通整数实现，替换为统一访问函数及 per-CPU 存储；UP 只有一个实例，主机多线程模拟使用 TLS 绑定 CPU。spinlock、timer 检查、scheduler 和对应测试均通过访问函数，不保留一个仍能被误读的全局兼容变量。

计数的修改在短暂本地 IRQ 保护内进行，避免 IRQ 打断读改写。零计数恢复只允许请求 reschedule，不能在自旋锁解锁但 IRQ 状态尚未恢复的中间执行调度。调度检查位于安全的 API/IRQ 退出边界。

加入 `nos_can_block()`：检查内核已运行、当前任务有效且非 idle、当前 CPU 的 IRQ 深度为零、抢占禁止深度为零。Cortex-M 使用架构异常上下文信息补充检查；主机提供真实 TLS 深度。A-profile/RISC-V/x86 的 trap 接入必须显式维护深度；未接入的平台不能据此宣称支持 ISR 阻塞检查。

保留 Cortex-M 汇编所需的 UP `interrupt_nest` 接口；SMP 内部使用每 CPU 深度，不能让该 UP 兼容符号参与 SMP 并发判断。系统调用上下文不等于硬 IRQ，上下文标记必须区分。

### 7.2 提前唤醒和 CPU 归属

准备阻塞在任务锁和源 rq 锁下完成出队，但任务可能尚在原 CPU 执行。新的唤醒入口在任务锁下快照源 CPU，再取得源 rq 锁重新检查 on_cpu、任务状态和队列关系。

- 若仍在源 CPU 执行，只在源 rq 恢复可运行状态，不将该任务迁移到其他 CPU；设置源 CPU 的 need_resched。
- 若源 rq 已确认任务离开 CPU，则释放源 rq，在仍持任务锁的条件下选择目标 rq，并入队一次。整个过程不同时持有源和目标 rq。
- 若任务已经 ZOMBIE/退出，确认消费旧票据但不使其 runnable。
- 同一 epoch 的重复请求是幂等 no-op，不触发非法状态断言。

on_cpu 的实际交接必须受源 rq 保护。同步上下文切换平台在真正保存旧上下文期间保持切换保护，接收上下文完成后才能让远端将旧任务视为可迁移；不能在释放保护前提前清零并让旧任务仍自由执行。

Cortex-M 的 `context_switch()` 只请求 PendSV；UP 没有远端迁移，但 park/finish 仍需等待调度确认，不能将函数返回当成上下文切换已经完成。主机 HAL 必须能模拟“已准备、尚未保存上下文、切换已完成”三个时刻，验证上述约束。

本轮不补齐 ARM64/RISC-V/x86 的上下文切换空桩或硬件 IPI；其完整实现是实际多核 QEMU 专项的前置条件。

## 8. Mutex 接入及 PI 边界

mutex 成为本阶段第一个真实使用新等待协议的原语。新增可返回错误的 `mutex_acquire(lock, timeout)`；既有 mutex_lock、mutex_lock_timeout、mutex_try_lock 和 mutex_unlock 保留源码兼容包装，调用方整体接口迁移在第二阶段完成。

- 无竞争 acquire/unlock 不创建 timer、不发布 wake batch、不强制 switch_task。
- 递归 acquire 返回 `-EDEADLK`；boot 时仅允许明确的 try/no-wait 获取，不允许静默假成功。
- owner、等待队列和交接版本由 mutex 对象锁保护；held_mutex_list 和任务 PI 汇总由对应任务锁保护。
- unlock 如需把 ownership 和 held_node 从旧 owner 转给等待者，先在对象锁内快照候选并固定任务引用，再解锁，按稳定 ID 顺序取得旧 owner/候选任务锁，重取对象锁并重新验证。候选变化则释放重试，禁止对象锁→目标任务锁。
- 验证成功后，在同一临界区转移 owner/held_node，并将候选 WAITING 提交为 SUCCEEDED；deadline 已到或已取消的候选不能得到 ownership。
- 超时胜出时移除等待者并撤销其捐赠；交接胜出时超时不能再撤销 ownership，也不产生第二次唤醒。

PI 保持现有最大 4 跳的有界链式能力；不承诺无限链或 CPU 预算继承。每一步快照并固定 owner/下一等待对象，释放对象锁后再取得该任务锁并验证关系；不跨两把 mutex 锁持锁递归。

PI 重新计算持任务锁遍历 held_mutex_list，逐个取得 mutex 锁读取对象保护的等待排序快照。并发等待者增删/改优先级需提高捐赠变更序号并请求重新计算，禁止较旧的计算在新捐赠之后覆盖有效优先级；变更方在完成本次 PI 传播后才能把操作报告为完成。发生关系版本变化时重试相应步骤。这里只限制链深，不宣称并发重试次数固定。

task_set_prio、mutex PI 与现有 IPC boost 使用独立来源记录汇总，不互相清空。Endpoint 在第三阶段替换旧的单 partner 来源；第一阶段至少保持现有 Endpoint 捐赠输入，不能因 mutex 超时删除仍有效的 IPC 捐赠。

持有 kernel mutex 的任务不能被强制删除并释放栈：新增可报告 `-EBUSY` 的取消入口，旧 task_del 包装报告拒绝；正常线程退出时仍持锁属于编程错误并触发诊断。第一版不实现 robust mutex 或 owner-death 自动解锁。

## 9. 取消、退休与新旧路径共存

任务取消先在任务锁下禁止新 managed wait，再按任务锁→对象锁提交本代 CANCELLED；若 SUCCESS 已提交则保留结果。延后唤醒批次仍需被消费，但不会重新激活退出任务。

reaper 检查专用等待队列、wake batch、managed/legacy deadline 引用以及 CPU 运行关系。无资格回收的 ZOMBIE 留在专用 reap_node 队列，下次 reaper 再检查；不在 IRQ 忙等，不把 task->list 同时挂到多个队列。

退出过程不在 task/object 锁内销毁 VM、Capability 表或执行 cleanup。Capability 对任务的长期引用管理尚在第三阶段，因此第一阶段不能允许新跨任务句柄通过裸指针绕过 task_get 规则。

尚未迁移的 sem/msgq/endpoint/notification 继续使用旧字段和 API，但不得直接唤醒 managed wait；其任务 timer 统一持有引用。`task_block_lock()` 为 legacy 阻塞增加等待代数，旧释放路径在原队列锁内快照 task/代数并取得 task 引用。

兼容唤醒队列使用独立 legacy 节点和自己的短临界区锁，禁止复用 task->list 或 managed wake_node。一个任务的 legacy 节点最多排队一次；同代重复请求合并，后代请求可取代尚未派发的过期代数。dispatcher 在队列锁内摘节点并将 task/代数/引用转移到局部票据，再解锁进入任务调度操作；此后不再读取节点。节点重新入队使用新的引用，因此前后票据互不共享所有权。过期票据不改变新一代状态，重复就绪请求为 no-op。

任务退出不操作已摘链的 legacy 唤醒节点，reaper 等待兼容票据释放引用。这些适配覆盖列表复用和对象存储安全，不代替原语锁内的资源终态裁决。

这些引用和节点适配避免引入跨新旧路径的释放后访问；旧等待间的资源裁决缺陷仍在第二/三阶段修复，须持续记录为未完成事项。

## 10. 验证与验收条件

测试必须直接编译生产 wait/object/timer/scheduler/mutex 逻辑。pthread 只实现主机的原子锁、CPU 上下文和实际阻塞/恢复执行环境；禁止用 pthread semaphore 或 condvar 重写被测对象本身。

### 10.1 必测交错

| 场景 | 断言 |
| --- | --- |
| 入队前/入队后/park 前完成 | 结果不丢失；至多一次调度确认；不会重复入队 |
| 交付与 deadline 同时发生 | 一个终态；交付与结果一致；已过截止时间不再交付 |
| 完成已提交，wake batch 暂停 | 等待者不能提前重用 wake_node 或开始下一代 |
| 旧回调已派发，新等待开始 | 旧 epoch 不影响新等待；引用归还恰好一次 |
| cancel_tag 与 timer 重装 | 旧取消不会移除新 timer |
| 任务取消/close 与回调、wake batch 竞争 | 不重新激活退出任务；对象和栈在引用归零前不释放 |
| 唤醒发生在源 CPU 上下文尚未保存时 | 仅源 rq 可运行；无双 CPU 执行 |
| 两个 CPU 持不同 IPC 锁 | 抢占计数和 IRQ 标志互不污染 |
| mutex 超时与 unlock 交接 | owner、held_node、等待结果和 PI 同步一致 |
| mutex 多捐赠者、4 跳链、捐赠者退出 | 只撤销自身捐赠；保持其他捐赠，旧计算不覆盖新值 |
| IRQ/no-current/idle/已持锁调用 | 返回规定错误，且没有队列或引用残留 |

测试 hook 仅暴露调度交错点，不替换生产终态算法。每个 hook 测试有超时和最终引用/队列一致性检查；随机压力不能替代这些受控交错测试。

### 10.2 配置及回归

- 主机 UP 和 SMP 配置、small-memory 和普通 timer 模式。
- 多线程高频等待/取消/关闭，启用可用的 ASan/UBSan/TSan；工具不可用时明确记录原因，不等价声称 sanitizer 通过。
- 现有 58 个 host runner 配置、Python unittest、架构/SoC header CMake 检查及层次依赖检查。
- 四个 MCU preset 重新 configure/build，无新增 warning；STM32 QEMU 启动、日志、RX burst 冒烟。
- 保留 spinlock 非 LIFO IRQ 嵌套、mutex owner/交接及有界链 PI 的既有回归，并把仅单线程 stub 验证的部分补为真实并发测试。
- 检查 32 位交叉链接无 64 位 atomic 运行库依赖；UAPI 64 字节布局不变。

### 10.3 成本与完成判定

对象和等待操作不得在热路径动态分配。报告 UP/SMP 下 TCB、等待记录、timer 元数据的实际 sizeof，以及四个固件 RAM/ROM 增量；专用等待 timer 的 RAM 成本必须显式列出，不以估算数字替代编译结果。

本阶段完成要求：上述不变量、受控交错、mutex 接入及引用回收均通过；旧模块未迁移事项仍明确列在后续阶段。不得把通过主机模拟表述为硬件 IPI、实际多核上下文切换或实时性已经验收。

## 11. 参考和适用关系

- [Linux wait queue](https://github.com/torvalds/linux/blob/master/include/linux/wait.h)：准备等待、条件检查与内存可见性共同构成协议。
- [Linux scheduler](https://github.com/torvalds/linux/blob/master/kernel/sched/core.c)：参考任务状态、runqueue 与执行 CPU 交接的完整同步要求；本文采用的是适合 NOS 的实现契约。
- [Linux timer 生命周期](https://docs.kernel.org/driver-api/basics.html)：摘除 timer、等待回调结束和禁止重装是不同保证。NOS 的 tagged 引用方案不等价提供通用 timer_shutdown_sync API。
- [Linux completion](https://docs.kernel.org/scheduler/completion.html)：等待结果与异步生命周期要一起设计。
- [Zephyr SMP](https://docs.zephyrproject.org/latest/kernel/services/smp/smp.html)：UP/SMP 保持应用语义一致，CPU 本地 IRQ 控制不能代替共享锁。
- [既有 mutex 设计](2026-11-02-spinlock-mutex-redesign-design.md)：保留 owner、无竞争快路和有界链 PI，替换与统一等待协议冲突的实现细节。
- [既有 timer 设计](2026-11-02-timer-redesign-design.md)：保留 per-CPU base、small-memory/list 与 wheel 后端，扩展安全派发契约。

以上为机制参考；本文不宣称与 Linux 或 Zephyr 完整 API 兼容，也不继承它们的安全或实时性保证。
