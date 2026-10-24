# NK60行为绑定灯效验收

<!-- Copyright (C) 2026 NOS contributors -->

日期：2026-10-24。分支：`dev/nk60-lighting-bindings`，父提交`fc33d7d821066b90e118fadb061e7e94d4844a0d`。本报告在冻结验收通过后生成，本报告本身不在测试前源码身份内；没有随后修改其他源码、文档或构建输入。

## 完成内容

- 实际键盘引擎输出只读PRESS/RELEASE/TAP/HOLD/MACRO_START/MACRO_END/RESET观察事件，保留按下时源动作、实际usage、有效修饰键及层；宏有独立真实启动时间，忙时重复按键不重置计时。
- 400字节声明式程序，最多12条规则、16个瞬态实例；26种触发/持续状态、9种来源过滤、层/修饰条件、source/区域/整板目标、7种动画、4种混合、透明度、优先级和方向。
- 老实例先绘制、新实例后绘制，过期先回收，池满替换最旧；宽区域held效果只跟随最近匹配来源，固定最坏着色工作量约28×61像素/帧；没有动态分配或阻塞动画线程。
- lighting minor2程序分页查询和原子上传，与旧逐键帧共用暂存区，当前peer物理解锁/权限/会话/generation/5秒期限均检查；验证失败保留旧程序。
- 严格JSON编译/反编译、区域与palette展开、三套可直接编辑的示例；独立Python CLI提供fx-info/get/put/clear。
- 624字节灯光/规则偏好与键位共同双槽提交；兼容224字节旧偏好，保留未知schema；常用配置解码减少栈临时副本并保留重叠输入的兼容路径。

配置方法、规则和wire字段见 [使用说明](NK60_LIGHTING.md)，[示例目录](../examples/nk60/lighting)。电脑可保存多份JSON，设备当前一次装载一份程序，没有实现设备内多profile目录。

## 冻结运行

```sh
source /workspace/.nos-env/activate.sh
python3 tests/acceptance.py --section boards --require-frozen-source
```

结果：10/10组通过，所有子进程exit0，源码身份前后相同。记录：`out/microkernel/acceptance/1792806867562535072/results.json`。记录中source对象以JSON sort_keys序列化后的SHA256为`6d1242c42611ed130f3563a14baf0213fe38c5a80c35773953996b93fbdc7bd2`。

| 项目 | 实际验证 |
| --- | --- |
| 格式与NLP基线 | 原17组参考检查，实际C栈128/1024两配置、独立Noise/TCP及原能力回归 |
| 输入/灯效 | 实际press/release、tap/double、Mod/Layer-Tap判定、held阈值、同一扫描修饰/层、锁灯/Caps Word/宏生命周期、宏忙时再次按键、混合/优先级/方向/实例顺序/回收、off/suspend/限流 |
| 输入不变性 | 原10万次键盘状态扫描，另5万次带/不带灯效观察器的HID输出对照 |
| 异常程序与互通 | 1万次异常400字节程序、三份独立Python编译示例与C校验器一致、错误字段/保留位/资源上限拒绝 |
| 掉电与迁移 | 1704个扩展配置逐字节断电点、旧API保留扩展、别名/部分重叠解码、旧偏好迁移与未知schema保护 |
| 实际NK60服务 | 真实NLP字节流检查上传缺页/重复/冲突、逐键帧与程序互斥、无效程序原子拒绝、当前会话/断开/超时、按键触发渲染、共同保存及重启读回 |
| 独立主机客户端 | C服务模型的loopback夹具，Python导入/读回三套JSON，错误配置保持原程序，fx-clear及持久状态 |
| 原板级能力 | USB复合HID/vendor、DWC2、HC595、NOR、RGB、设备权限和Flash镜像工具原回归 |
| 固件与启动 | MPU0/1实际交叉构建，链接器Flash/RAM/16KiB用户区约束，实际目标编译器静态帧统计；stock netduinoplus2用户入口专项明确跳过F401时钟初始化 |

上表是10组runner内部检查的归类，不是声称额外执行完整POSIX/Linux或MMU全系统矩阵。正常退出C单测使用ASan/UBSan/LSan；loopback夹具终止前检查ASan/UBSan日志，不将SIGTERM描述成已运行退出泄漏扫描。夹具模拟USB/Flash/物理解锁，不证明电气或真实人在场。

## 资源

固件记录：`out/microkernel/nk60-firmware/1792806978761313580/results.json`。

| 配置 | bin字节 | 用户静态字节 | 静态结束 | MSP余量字节 | 已知加载核心链字节 | 已知保存核心链字节 |
| --- | --- | --- | --- | --- | --- | --- |
| MPU=0 | 93436 | 15880 | `0x2000be08` | 16888 | 2528 | 2584 |
| MPU=1 | 94420 | 15880 | `0x2000be08` | 16888 | 2528 | 2584 |

应用Flash上限128KiB，总SRAM64KiB，共享用户静态区16KiB，用户栈4KiB。新增kb_fx_render/keyboard_event/observe、nk60_link_program和kb_set_event_handler的ELF符号也核对位于用户text范围。

加载/保存数字是已知核心调用链的编译器静态帧之和，排除全部间接硬件回调及罕见的重叠解码临时副本路径，不是完整最坏调用图或实板高水位证明。用户静态区剩余504字节；增加规则、实例或模块需要重新核算，不能直接放大数组。

所有效果经过现有16/255通道上限、估算电流预算和USB/idle策略。启用非空程序可请求最高25fps，未测实板帧率/扫描延迟/功耗。没有可访问实板，真实灯珠波形、电气、电流和断电实验仍待验收。

原始`/workspace/nos`工作树的1230项既有删除未修改、暂存或提交。
