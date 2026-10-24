# NK60能力增强验收记录

<!-- Copyright (C) 2026 NOS contributors -->

日期：2026-10-24。本页记录 `fc33d7d` 阶段；后续行为绑定规则见 [自定义灯效](NK60_LIGHTING.md)。分支：`dev/nk60-capabilities`，基于 `bc74ed2ef7bbfc5f341037de3a2692c358fb8eaa`。本报告在冻结验收结束后生成，随后未修改工具、测试、协议、文档或构建输入；本报告本身不在测试前源码身份内。主机环境不可访问NK60实板。

## 交付与范围

本次增加61键1/4u布局与逐键RGB，关闭/静态/层色/呼吸/彩虹/波浪/按键响应/自定义模式，速度/空闲/估算电流策略、Caps Lock/Caps Word指示，以及默认Grave Escape与Fn+Caps Word。键位canonical仍为1056字节，新增224字节偏好扩展与键位共同双槽提交，保留现有NOR尾部16KiB布局及其他历史内容。未知偏好schema禁止覆盖保存，显式物理解锁defaults才重置。

NLP增加keyboard_control布局/存储/诊断，以及lighting minor1的策略和整帧事务。独立Python命令行可完成读取、保持当前peer等待物理解锁、设置灯效/策略和61色完整上传。未完成帧在断线/撤权/5秒超时后释放；整帧校验和generation检查通过才应用。扩展组以reserved标记，未注册或未支持操作返回明确错误。

## 冻结验收

命令：

```sh
source /workspace/.nos-env/activate.sh
python3 tests/acceptance.py --section boards --require-frozen-source
```

结果：10/10组通过，所有子进程exit0，源码前后身份一致。完整记录：`out/microkernel/acceptance/1792801964644371126/results.json`。该记录source对象按JSON sort_keys序列化的SHA256：`0f1300ec0835f71b64c5e2e8c2db1ab0c87f8dcb3ae4cb544c816e1a3ff51e6b`。

| 组 | 验证内容 |
| --- | --- |
| NLP格式/注册表 | 17组布局、固定向量、CRC、文档与本地链接检查 |
| NLP实际C/Python互通 | 128/1024两配置，实际会话/路由/能力源码、各30000异常输入、独立Noise/安全TCP/真实主机PTY |
| 键盘引擎 | 实际10万状态扫描、层/宏/背压/全释放、Grave Escape同时修饰、Caps Word终止与超时 |
| 新布局/灯光/配置 | 61键/LED唯一且行宽闭合，所有灯效/预算/idle/suspend/回绕，呼吸周期边界，旧API保留扩展，1304个逐字节掉电边界 |
| USB | 现有复合HID/vendor与DWC2寄存器模型回归 |
| 板级硬件 | 真实HC595/NOR/RGB驱动源码与注入模型 |
| 设备白名单 | UID只读、USB与DMA访问边界 |
| 实际NK60服务 | USB字节流的目录/布局/存储/诊断/物理解锁/改键/逐键帧、重复冲突/断线/超时；偏好保存重启及未知schema保护 |
| 独立NK60 CLI | 实际C服务的回环夹具与独立Python：布局/存储/健康、灯效/策略、61色上传读回、保存与非法文件拒绝。物理I/O和解锁按键由夹具模拟 |
| Flash镜像工具 | 原有ELF/向量/分区校验 |
| 两种MPU固件 | 实际交叉构建、用户text归属、Flash/RAM/用户区和已知核心栈帧预算 |
| Cortex-M启动专项 | MPU开/关实际NK60 ELF，stock QEMU netduinoplus2，GDB明确跳过未建模的F401时钟初始化 |

上表将部分验收组的内部检查拆行展示，总runner组数仍为10。测试使用ASan/UBSan，正常退出的C单测启用LSan；回环夹具进程终止前检查ASan/UBSan日志，不能据SIGTERM终止声称其退出泄漏扫描已经执行。固件构建记录：`out/microkernel/nk60-firmware/1792802053435773294/results.json`。

## 资源与保护范围

| 配置 | bin字节 | 用户静态字节 | 静态结束地址 | MSP余量字节 | 已知加载链字节 | 已知保存核心链字节 |
| --- | --- | --- | --- | --- | --- | --- |
| MPU=0 | 85576 | 14792 | `0x2000b9c8` | 17976 | 2816 | 1800 |
| MPU=1 | 86560 | 14792 | `0x2000b9c8` | 17976 | 2816 | 1800 |

链接器限制应用Flash128KiB、总SRAM64KiB、共享用户静态区16KiB。静态栈帧由实际Cortex-M编译选项加`-fstack-usage`取得，加载已知链包含任务入口/service_init/load_extended/decode，保存核心链包含入口/service_step/persist/save_extended/save_record；不包含全部间接驱动回调，也不是实板高水位或完整最坏调用图证明。

当前MPU配置沿用NOS共享用户静态区的保护范围，不能称完整进程隔离。RGB输出上限16/255，电流采用1mA/灯静态加20mA/满量程通道模型；未测实际灯珠、整板供电和USB电流。10fps与4ms扫描是软件预算/目标，未测实板输入延迟分布。

## 后续扩展与实板边界

布局与产品协议、7组reserved插槽、端口冲突和分区迁移规则见 [NK60能力](NK60_CAPABILITIES.md)。具体显示/旋钮/无线/分体模块、combo/tap dance/多profile资源仓、VIA/Vial GUI兼容和安全OTA尚未接入；不能从reserved字段或上位机模型推断已支持。参照来源的固定提交及SHA256见 [调研](KEYBOARD_RESEARCH.md)。

未执行真实USB枚举、电气/波形、电源/温升或实板断电实验。本次执行boards范围，未重复声称完整POSIX/Linux或MMU全系统验收。原工作树`/workspace/nos`的1230项既有删除未修改、暂存或提交。
