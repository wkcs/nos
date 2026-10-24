# NK60 可组合灯效与按键行为绑定

<!-- Copyright (C) 2026 NOS contributors -->

灯光现在由基础模式、声明式规则、动画实例和最终输出限制组成。JSON配置可以把物理按键、实际HID键码、动作判定、层、修饰键、锁灯、Caps Word、宏和空闲/唤醒事件绑定到不同区域与动画。程序是固定400字节的数据，不执行脚本、不访问任意内存或设备。

## 快速使用

仓库提供三份可直接编辑的配置：

| 文件 | 效果 |
| --- | --- |
| [typing-ripples.json](../examples/nk60/lighting/typing-ripples.json) | 每次按下产生蓝紫波纹，点击原键渐灭，Caps Word显示琥珀色 |
| [behavior-bindings.json](../examples/nk60/lighting/behavior-bindings.json) | Fn层渐变、Shift按键反馈、Mod-Tap敲击/长按不同颜色、Space双击、宏启动和Caps Lock指示 |
| [gaming-wasd.json](../examples/nk60/lighting/gaming-wasd.json) | 纵向背景渐变、WASD按住变色、按下高亮渐灭 |

Mod-Tap规则需要键位配置中有该动作；宏规则需要对应宏实际配置并启动。示例不会自动修改键位或执行宏。

先在电脑离线检查，再连接NLP。写操作保持同一连接，按住Esc+PN+Fn两秒完成当前peer授权：

```sh
python3 scripts/nlp_lighting.py examples/nk60/lighting/typing-ripples.json
python3 scripts/nlp_host.py --usb 0eff:0001 --physical fx-info
python3 scripts/nlp_host.py --usb 0eff:0001 --physical fx-put --file examples/nk60/lighting/typing-ripples.json --save --wait-unlock 60
python3 scripts/nlp_host.py --usb 0eff:0001 --physical fx-get --file /tmp/nk60-effects.json
python3 scripts/nlp_host.py --usb 0eff:0001 --physical fx-clear --save --wait-unlock 60
```

`fx-get --file`拒绝覆盖已有文件；`fx-clear`关闭行为绑定，保留原逐键配色和基础灯光设置。`--save`返回SAVE_PENDING，实际保存须等按键全释放、宏结束及报告队列排空。USB命令用于实板验收，当前已验证的是独立Python客户端与实际C服务的模型互通。

## 配置示例

```json
{
  "version": 1,
  "enabled": true,
  "dark_base": true,
  "frame_ms": 40,
  "zones": {"wasd": [16, 29, 30, 31]},
  "palette": {"ice": "#00cfff", "violet": "#8b22ff"},
  "rules": [
    {
      "when": "press",
      "target": "all",
      "effect": "ripple",
      "color": "@ice",
      "color_to": "@violet",
      "duration_ms": 800,
      "period_ms": 800,
      "blend": "add",
      "priority": 10
    },
    {
      "when": "held",
      "match": {"zone": "wasd"},
      "threshold_ms": 100,
      "target": "source",
      "effect": "solid",
      "color": "#ff5500",
      "priority": 20
    }
  ]
}
```

`dark_base`清空基础模式和旧指示覆盖的底图，再绘制规则；需要Caps Lock等反馈时可显式添加规则。`false`则在现有静态/逐键/层色/动画底图上叠加。`enabled=false`或空规则保留基础模式。总开关off、亮度0、USB未配置/挂起和全局空闲熄灯仍约束所有效果。

## 规则字段

| 字段 | 说明 |
| --- | --- |
| `when` | 触发或持续状态，见下表 |
| `match` | 一个来源过滤器；默认`{"any": true}` |
| `conditions` | 额外层与修饰键条件，与来源过滤器同时满足 |
| `target` | `source`当前来源键、`all`，或`{"keys":[...]}`、`{"rows":[0..4]}`、`{"zone":"名称"}` |
| `effect` | solid/fade/pulse/breathe/rainbow/wave/ripple |
| `color` / `color_to` | `#RRGGBB`、RGB三字节数组、或`@palette名称`；rainbow使用完整色轮 |
| `blend` | replace透明覆盖、add饱和相加、max逐通道最大、multiply按透明度乘色 |
| `priority` | 0..255，小值先绘制；相同优先级保持配置顺序 |
| `opacity` | 0..255；与动画强度共同控制透明度 |
| `duration_ms` | 边沿事件1..60000；持续状态0表示条件满足期间持续，也可设置有限持续时间 |
| `period_ms` | 20..60000；动画周期，独立于总持续时间 |
| `threshold_ms` | tap/double_tap为20..2000；held为20..60000；其他触发必须0 |
| `single` | 边沿触发时重启本规则唯一实例；默认false可有多个来源同时播放 |
| `reverse` | fade改为渐亮，pulse/breathe反相，wave/rainbow/ripple反向；solid不接受此字段为true |
| `axis` | wave的x/y方向；其他效果只能x |
| `name` | 可选电脑侧标签，不写入设备，不影响匹配 |

所有索引是[物理布局](NK60_CAPABILITIES.md)的key ID，不因改键变化。区域与palette只在电脑侧展开，不消耗MCU名称表。`match.keys`、`match.usages`和`match.zone`会展开为多条设备规则，展开后最多12条；超过上限会拒绝上传，不静默截断。

动画含义：solid保持color；fade在两色之间变化并渐灭，reverse时渐亮；pulse从亮到暗再亮，breathe从暗到亮再暗；wave按键的真实x/y坐标作两色渐变；ripple使用按键中心的曼哈顿距离形成菱形扩散。周期短于采样间隔会跳帧，平滑效果可选更长周期。

同一边沿规则的多个实例按时间从旧到新叠加，新实例在上方；最多16个并行动画。过期实例先回收，池满时替换最旧实例，时间相同按轮转顺序处理，`dropped_instances`饱和计数。持续的held+source可以同时点亮全部按住的匹配键；held作用于整板/区域时跟随最近按下且满足条件的一个来源，避免全键长按产生平方级工作量。每帧最多约`(12+16)×61`个着色像素操作，无动态分配或阻塞等待。

## 支持的行为

| `when` | 语义 |
| --- | --- |
| press / release | 防抖确认按下/释放，使用该次按下绑定的来源动作 |
| tap | 普通键在threshold内释放；Mod/Layer-Tap仅在引擎确认为敲击时产生 |
| double_tap | 同一物理键相邻两次有效tap的释放间隔不超过threshold；各次按住时间也不超过threshold，可形成连续相邻双击 |
| held | 物理按住超过threshold期间生效；与Mod/Layer-Tap的语义判定分开 |
| hold_decided | Mod/Layer-Tap达到tapping term或被其他按键提升为长按时触发 |
| layer_enter / layer_leave / layer_active | 层进入、退出、持续激活 |
| host_led_on / host_led_off / host_led_active | 主机Num/Caps/Scroll等LED状态改变或持续有效，索引0..4 |
| caps_word_on / caps_word_off / caps_word_active | Caps Word进入、退出、持续有效 |
| macro_start / macro_end / macro_active | 宏真实启动、自然结束、运行期间；复位/取消清除旧实例 |
| modifiers_on / modifiers_off / modifiers_active | 实际输出修饰键状态改变或持续有效 |
| connect / disconnect | USB可输出状态改变；断开/挂起仍会清除瞬态效果并关灯 |
| idle / wake | 达到`idle_event_ms`或按键活动恢复；0禁用该事件计时，持续按住不进入idle |
| always | 在程序启用期间持续生效 |

`match`可用any、key、usage、action（可带parameter）、layer、host_led、macro、consumer或modifiers。usage支持整数和A、ESCAPE、F1、LSHIFT等标准名称；consumer使用独立数值，避免与键盘usage混淆。action名称对应当前动作类型，包括mod_tap、layer_tap、oneshot、local、grave_escape；默认parameter255通配。Grave Escape保留来源动作身份，同时usage反映实际选择的Esc/Grave；释放沿不重新查询新层。

`conditions.layers`要求列出的层全部激活，`not_layers`要求全部不激活，两者不能重叠。`conditions.modifiers`要求所有指定位，`modifiers_any`只要求任一位，`not_modifiers`要求全部未按；三种模式选一种。支持LCTRL/LSHIFT等单侧名称，通用SHIFT/CTRL/ALT/GUI可用于modifiers_any或not_modifiers，也可用于`match.modifiers`的任意匹配。按键事件捕获同一扫描的修饰键和层，并包含One Shot/Caps Word产生的有效修饰；反馈不会通过重新注入按键来实现。

例如只在Fn层并且按住任一Shift时触发W：

```json
{
  "when": "press",
  "match": {"usage": "W"},
  "conditions": {"layers": [2], "not_layers": [3], "modifiers_any": ["SHIFT"]},
  "target": "source",
  "effect": "fade",
  "color": "#ff0055",
  "duration_ms": 500
}
```

状态型效果在该状态退出时停止。边沿效果持续到duration结束；输入复位、USB断开/挂起和替换程序会清除旧实例及双击历史。程序更新不改变输入映射、HID报告、宏或层本身。事件观察器只做固定上限的匹配/实例更新，渲染留在服务poll。

## NLP与持久化

lighting major1/minor2新增操作：GET_PROGRAM_INFO(11)、READ_PROGRAM(12)、BEGIN_PROGRAM(13)、WRITE_PROGRAM(14)、COMMIT_PROGRAM(15)、ABORT_PROGRAM(16)。旧minor1逐键帧和程序上传共用暂存区，不能同时开启。

| 操作 | body |
| --- | --- |
| GET_PROGRAM_INFO | 请求空；响应32字节：schema:u16,max_rules:u8,max_instances:u8,wire_bytes/min_frame_ms/max_frame_ms/effect_mask各u16,trigger_mask:u32,match_mask:u16,blend_mask:u8,active_instances:u8,dropped:u32,generation:u32,count:u8,flags:u8,reserved:u16 |
| READ_PROGRAM | 请求offset:u16,count:u16,generation:u32（0任意）；响应generation:u32,offset:u16,count:u16,bytes[] |
| BEGIN_PROGRAM | generation:u32；响应transaction_id:u64 |
| WRITE_PROGRAM | transaction_id:u64,offset:u16,count:u16,bytes[]；顺序上传，同内容重复可重试 |
| COMMIT_PROGRAM | transaction_id:u64,generation:u32,persist:u8,reserved[3]；响应generation:u32,persistence:u8,reserved[3] |
| ABORT_PROGRAM | transaction_id:u64 |

所有写操作需要CONFIG_WRITE和当前peer的物理解锁。事务绑定创建会话，固定5秒期限；缺页、内容冲突、错误版本、非法条件/枚举/保留位、无效geometry mask或超过资源上限均明确拒绝，保持当前程序。完整结构验证后才原子替换；generation变化返回CONFLICT。只有有效schema才进入执行路径。

程序wire固定400字节：16字节头`KLFX[4], version:u8=1, count:u8, flags:u16, frame_ms:u16, idle_event_ms:u16, reserved:u32`，后接12个32字节规则，未使用条目全零。头flags bit0启用、bit1暗底图。规则字段为trigger:u8,match:u8,value:u16,target/effect/blend/priority各u8,color[3],color_to[3],duration/period/threshold各u16,target_mask:u64,options:u8,opacity:u8,layer_conditions:u8,modifier_conditions:u8。options bit0 single、1 reverse、2纵向wave、3任意修饰条件、4排除修饰条件；layer_conditions低4位必需层、高4位排除层。枚举顺序和位定义见 [lighting_fx.h](../servers/keyboard/lighting_fx.h)，独立Python编译/反编译会与C校验器交叉检查。

新偏好载荷为624字节：原224字节KBL1加400字节KLFX，与1056字节键位共同使用原配置双槽提交，不占新Flash扇区。opaque扩展上限由256增至640。无扩展的旧记录使用默认参数，224字节旧灯光记录保留设置并关闭规则；未知格式继续禁止自动覆盖。降级旧固件前应备份配置。

非空启用程序允许40..1000ms渲染间隔，即最高25fps；旧基础模式维持100ms默认。亮度、估算电流预算、挂起断灯和空闲策略在所有混合完成后统一执行，远端规则不能绕过限制。当前仍以16/255通道上限和保守电流模型运行，实际波形、功耗及扫描延迟分布需要实板测量。

## 验证

`tests/host_keyboard.py`运行实际输入和渲染源码、事件分类、条件/混合/时间边界、5万次带/不带观察器的输入对照、1万次异常程序、1704个逐字节配置掉电边界，以及独立Python示例与C校验器互通。`tests/host_nk60_service.py`还通过实际NLP字节流检查事务隔离、暂存区互斥、失败不改变当前程序、触发渲染、保存重启，并由独立Python CLI导入/读回三份示例。

完整本次验收使用boards冻结入口及MPU开关两种固件预算检查。没有可访问实板，模型结果不代表USB电气、真实按键/灯珠、实际电流或真实掉电实验已完成。
