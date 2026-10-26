# ESP32 调度暂停与任务回收

<!-- Copyright (C) 2026 NOS contributors -->

本批处理C3/S3 FreeRTOS兼容接口的调度暂停语义，以及来宾回归对异步回收的错误同步方式。NOS继续拥有任务、调度器和IPC；未修改vendor tasks.c或公共内核/VFS。

## 可重复的问题

调度暂停并不关闭中断。旧OSAL的xTaskIncrementTick仍调用scheduler_tick，推进kernel_ticks、处理超时并移除事件节点。IDF队列/定时器实现使用vTaskSuspendAll保护这段事件处理，因此不能让tick中断在暂停期间修改它们。

诊断基线ca3e2e7不改变该行为。两架构真实QEMU均得到：嵌套暂停50ms，tick从0到5、等待链表节点从1到0；新合同断言按预期失败。证明的是确定的兼容层违约，不能由此直接倒推上一批C3一次定时器超时的全部内部状态。

同一受控用例让一个有限生命周期任务临时占用idle之上的优先级：worker退出后，父任务延迟两个tick时，errno和线程局部数据仍正确，而TLS回收回调尚未执行；释放idle后回调完成。这说明固定两个tick不能作为异步清理完成凭据。上一批S3复合断言没有捕获子项值，不能追溯断言当时一定是哪一子项失败。

## 修正

- 暂停期间只累计64位pending tick，不推进任务超时或修改等待链表；嵌套内层resume保持冻结。
- 最外层resume在IRQ保护下重放pending tick，再恢复调度；保留实际经过的tick，不丢弃超时。
- TLS用例分别等待worker完成和TLS回调确认。errno、thread cookie和精确两次回调的要求全部保留，四个等待阶段共享一个从创建worker开始的3秒总预算，而不是扩大原有3秒等待。
- 控制用例和40轮堆回收用例按任务代次等待兼容记录真正释放，再检查资源；不把eDeleted或一个固定延迟当成释放完成。
- 信号量使用静态生命周期，避免idle在Give内部被抢占后，对象被父任务提前释放。
- 失败诊断记录errno/cookie/回调计数、tick、pending tick、暂停层数，以及任务状态、等待节点标志、删除状态和deadline。

以上诊断入口与任务表快照只编入回归固件，正常固件不包含这些符号；审计会检查这一点。正常固件的OSAL行为变化是正确推迟并重放暂停期间的tick。

## 定时器回归

新增64轮嵌套暂停测试，在暂停后用零等待把单次软件定时器排入队列，保持35ms硬件时间后恢复，要求暂停期间tick与回调次数不变，内层resume不提前推进，外层resume后每轮恰好一次回调，等待仍以3秒为限。

测试必须先暂停再arm：xTimerStart可能让出CPU，若先arm再暂停，短定时器可在进入暂停区前合法触发。开发测试已捕获并修正这个用例本身的时序假设，没有将其算成OSAL缺陷。

原来的软件定时器、esp_timer任务/ISR、通知、优先级继承、TLS/FPU、私有stdio、全部任务槽、退出回收和Flash/WL回归继续执行。7812b03四组专项与四轮并行复测通过，证据和保留失败见 [ESP32_TIMING_VALIDATION.md](ESP32_TIMING_VALIDATION.md)。没有新增实板或多核结论；长期稳定性仍需持续观察。

## UART 停止时的待处理中断

冻结c8dfd22的四组验证中，C3 raw/WL和S3 raw通过，S3 WL在设备阶段持续报`Unhandled interrupt 8 on cpu 0!`。该失败日志和ELF保留，未将串行重试当成修复。

固定版本QEMU的S3中断矩阵在路由寄存器写入时只更新映射，不撤销旧CPU线。IDF原版uart_driver_delete先esp_intr_free（重路由源、删除处理函数），之后才关闭UART中断。因此在断开时仍有效的外设中断可留下没有处理函数的旧CPU线。

同一c8dfd22 ELF的受控调试用例在SDK持有释放临界区时，通过qtest写入UART1 TXFIFO_EMPTY使中断源有效：源28原来路由CPU8，继续后路由变成禁用值6，而CPU INTERRUPT仍为0x100、IRQ8处理函数已是xt_unhandled_interrupt，停在该函数入口。这证明了该模拟器的待处理中断卸载机制；原始随机失败没有寄存器快照，不能追溯其具体UART状态位。

NOS UART停止现在先调用原版uart_disable_intr_mask关闭全部外设中断，再调用原版uart_driver_delete。关闭失败时返回真实错误并保留设备所有权，供重试。未修改或替换vendor HAL、路由器和ISR，也未通过禁用全部CPU中断掩盖错误。

回归固件通过链接包装，在每次NOS关闭全部UART中断之前临时屏蔽CPU IRQ并启用真实TX空中断，断言确有待处理状态；执行原版关闭函数后要求状态为0，并在删除入口再次检查。两架构各执行64次经NOS服务模型的启动、读写、停止和重启。包装代码、计数符号和注入只编入回归固件，正常镜像审计要求它们全部缺席。

## 冷启动的第一个 SysTick

扩展快照候选8e2d0ef的C3 raw正式回归在文件系统测试之前失败，实际输出为before=0、during=0、nested=0、after=2、linked=1、deferred=1。固定版本原始IDF `port_systick.c` 的 `SysTickIsrHandler` 在 `s_handled_systicks==0` 时把当前硬件计数设为基线，并强制本次diff=1；首个中断较晚到达时，不能从启动后的50ms推断已分派至少两个tick。

回归现在先用vTaskDelay(1)等待首个实际调度tick，输出systick_ready探针，再进入原有50ms嵌套暂停测试。暂停期间tick冻结、链表节点保留、deferred至少2、恢复重放和3秒任务回收预算全部保留，没有修改原始SDK中断处理或NOS调度器。候选失败、当时镜像与中止的其余部分矩阵保留，最终结果以新的冻结提交为准。

后续acf8ab5候选中，C3四组均通过，S3 raw的生态回归却观察到ready=1、before=2、after=2、deferred=0。配置确认两芯片都使用SYSTIMER；50ms计时器读数推进并不构成中断已执行的证据，不能将缺少测试刺激误报为NOS未重放。

回归改为直接观察pending tick：基础用例等待实际5个tick，64轮定时器用例每轮等待实际3个tick，再验证冻结、内层resume、完整重放及恰好一次回调。刺激阶段有独立3秒硬件计时看门狗，超时输出明确的stimulus-timeout及IRQ状态；原回收/定时器完成的3秒预算不变。没有降低deferred至少2、链表、TLS或资源回收断言，正常固件不包含这些诊断。两次旧候选的原始失败与部分矩阵均保留。
