# NK60 macOS (Keychron Z6 Ultra 风格) 按键映射与 16-bit Consumer 媒体键设计

## 1. 背景与目标

为 NK60-V1 与 NK60-V2 (ANSI 61 键) 完善 macOS 键盘配置文件 `configs/keyboard/<board>/macos.json`，参考渴创 (Keychron) Z6 Ultra 的 macOS 键位设计，支持丰富的多媒体与系统功能（屏幕亮度加/减、调度中心、Spotlight 搜索、键盘灯效切换与开关、曲目控制、音量与静音、全屏/区域/菜单三种截图以及立即锁屏）。

同时，将 USB HID Consumer Control 报告（Report ID 4）从原有的 8-bit 固定位图升级为主流开源固件（QMK / ZMK / Keychron）标准的 **16-bit Consumer Usage Array (`0x0000`..`0x03FF`, 2 字节小端序)**，以原生支持标准 HID Consumer Page (`0x0C`) 中的屏幕亮度与多媒体 Usage ID，并为后续扩展任意 Consumer Usage 提供免改描述符的通用能力。

## 2. 三层按键映射与动作绑定设计 (`configs/keyboard/<board>/macos.json`)

在 `ansi61` (5x14 矩阵，61 键，3 层) 配列下，由于 `Fn` (Layer 1) + 数字键已用于 `F1`~`F12`，所有参考 Keychron Z6 Ultra 的 macOS 多媒体、亮度、系统快捷与截图功能统一放置在 **`PN` 层 (Layer 2)**：

### 2.1 Layer 0（基础层 - macOS 修饰键布局）
- **第 0~3 行**：标准 ANSI 61 键基础键位。
- **第 4 行（底排从左至右）**：
  - Key 53: `KC_LCTRL` (`0xe0`)
  - Key 54: `KC_LALT` (`0xe2`, macOS Left Option)
  - Key 55: `KC_LGUI` (`0xe3`, macOS Left Command)
  - Key 56: `KC_SPACE` (`0x2c`)
  - Key 57: `KC_RGUI` (`0xe7`, macOS Right Command)
  - Key 58: `KC_PN` (`0xe9`, 瞬时切换至 Layer 2)
  - Key 59: `KC_RCTRL` (`0xe4`)
  - Key 60: `KC_FN` (`0xe8`, 瞬时切换至 Layer 1)
- **动作绑定**：
  - Layer 0, Key 28 (`Caps Lock`): 绑定 `"caps_lock"`，用于同步主机 Caps Lock 指示灯状态并驱动指示灯图层。

### 2.2 Layer 1（Fn 层 - 标准 F1~F12 与导航/方向键）
- **顶行 (Key 0..13)**：
  - Key 0 (`Esc`): `KC_GRAVE` (`0x35`)
  - Key 1..12 (`1`..`=`): `KC_F1` (`0x3a`) .. `KC_F12` (`0x45`)
  - Key 13 (`Backspace`): `KC_DELETE` (`0x4c`)
- **方向与编辑导航键**：
  - Key 16 (`W`), Key 29 (`A`), Key 30 (`S`), Key 31 (`D`): `Up (0x52)`, `Left (0x50)`, `Down (0x51)`, `Right (0x4f)`
  - Key 27 (`\`): `Insert (0x49)`
  - Key 34 (`H`), Key 35 (`J`), Key 36 (`K`): `PrintScreen (0x46)`, `ScrollLock (0x47)`, `Pause (0x48)`
  - Key 39 (`'`): `End (0x4d)`
  - Key 49 (`,`), Key 50 (`.`): `PageUp (0x4b)`, `PageDown (0x4e)`
  - Key 51 (`/`), Key 57 (`RGUI`), Key 58 (`PN`), Key 59 (`RCTRL`): 右下方向键簇 `Up (0x52)`, `Left (0x50)`, `Down (0x51)`, `Right (0x4f)`
  - 左右修饰键位置 (`LSHIFT`, `RSHIFT`, `LCTRL`, `LALT`, `LGUI`) 保留对应修饰键码，支持带修饰键的 Fn 组合。

### 2.3 Layer 2（PN 层 - Keychron Z6 Ultra 风格 macOS 媒体键与快捷功能）
- **顶行多媒体与系统控制 (Key 0..13)**：
  - Key 0 (`PN + Esc`): `KC_GRAVE` (`0x35`)
  - Key 1 (`PN + 1`): **屏幕亮度减** — 键码 `NLK_KC_MEDIA_BRIGHTNESS_DOWN` (`0xac`)
  - Key 2 (`PN + 2`): **屏幕亮度加** — 键码 `NLK_KC_MEDIA_BRIGHTNESS_UP` (`0xab`)
  - Key 3 (`PN + 3`): **调度中心 (Mission Control)** — 键码 `KC_UP` (`0x52`) + 绑定 `"key_chord"` (`modifiers: 1`, 即 `Left Ctrl + Up`)
  - Key 4 (`PN + 4`): **Spotlight 搜索** — 键码 `KC_SPACE` (`0x2c`) + 绑定 `"key_chord"` (`modifiers: 8`, 即 `Left Cmd + Space`)
  - Key 5 (`PN + 5`): **切换上一灯效** — 键码 `0x00` + 绑定 `"light_prev"`
  - Key 6 (`PN + 6`): **切换下一灯效** — 键码 `0x00` + 绑定 `"light_next"`
  - Key 7 (`PN + 7`): **上一曲** — 键码 `NLK_KC_MEDIA_PREV` (`0xa6`)
  - Key 8 (`PN + 8`): **播放 / 暂停** — 键码 `NLK_KC_MEDIA_PLAY_PAUSE` (`0xa7`)
  - Key 9 (`PN + 9`): **下一曲** — 键码 `NLK_KC_MEDIA_NEXT` (`0xa5`)
  - Key 10 (`PN + 0`): **静音** — 键码 `NLK_KC_MEDIA_MUTE` (`0xa8`)
  - Key 11 (`PN + -`): **音量减** — 键码 `NLK_KC_MEDIA_VOL_DOWN` (`0xaa`)
  - Key 12 (`PN + =`): **音量加** — 键码 `NLK_KC_MEDIA_VOL_UP` (`0xa9`)
  - Key 13 (`PN + Backspace`): `KC_DELETE` (`0x4c`)
- **截图、锁屏、灯光开关与辅助导航**：
  - Key 24 (`PN + P`): **全屏截图** — 键码 `KC_3` (`0x20`) + 绑定 `"key_chord"` (`modifiers: 10`, 即 `Left Cmd + Left Shift + 3`)
  - Key 25 (`PN + [`): **区域截图** — 键码 `KC_4` (`0x21`) + 绑定 `"key_chord"` (`modifiers: 10`, 即 `Left Cmd + Left Shift + 4`)
  - Key 26 (`PN + ]`): **截图与录屏菜单** — 键码 `KC_5` (`0x22`) + 绑定 `"key_chord"` (`modifiers: 10`, 即 `Left Cmd + Left Shift + 5`)
  - Key 37 (`PN + L`): **立即锁屏** — 键码 `KC_Q` (`0x14`) + 绑定 `"key_chord"` (`modifiers: 9`, 即 `Left Ctrl + Left Cmd + Q`)
  - Key 47 (`PN + B`): **键盘背光总开关** — 键码 `0x00` + 绑定 `"light_toggle"`
  - 保留 `PN + W/A/S/D`、`PN + \` (`Insert`)、`PN + H/J/K`、`PN + '` (`End`) 以及修饰键位。

## 3. USB HID 16-bit Consumer Control 协议与固件设计

### 3.1 UAPI 功能键码定义 (`include/uapi/nos/nlk.h`)
在 `enum nlk_kb_function_keycode` 中保持原有 `0xA5`..`0xAA` 顺序不变，追加屏幕亮度键码：
```c
enum nlk_kb_function_keycode {
    NLK_KC_MEDIA_NEXT = 0xA5U,
    NLK_KC_MEDIA_PREV,
    NLK_KC_MEDIA_PLAY_PAUSE,
    NLK_KC_MEDIA_MUTE,
    NLK_KC_MEDIA_VOL_UP,
    NLK_KC_MEDIA_VOL_DOWN,
    NLK_KC_MEDIA_BRIGHTNESS_UP,
    NLK_KC_MEDIA_BRIGHTNESS_DOWN,
};
```

### 3.2 USB HID 报告描述符 (`drivers/usb/device/class/hid.c`)
将 `HID_REPORT_ID_MEDIA` (Report ID 4) 的报告描述符改为标准 16-bit Consumer Control Array (`0x0000`..`0x03FF`)：
```c
#ifdef CONFIG_USB_DEVICE_HID_MEDIA
    USAGE_PAGE(1),      0x0C,
    USAGE(1),           0x01,
    COLLECTION(1),      0x01,
    REPORT_ID(1),       HID_REPORT_ID_MEDIA,
    USAGE_PAGE(1),      0x0C,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(2), 0xFF, 0x03,
    USAGE_MINIMUM(1),   0x00,
    USAGE_MAXIMUM(2),   0xFF, 0x03,
    REPORT_SIZE(1),     0x10,
    REPORT_COUNT(1),    0x01,
    INPUT(1),           0x00,
    END_COLLECTION(0),
#endif
```

### 3.3 键盘引擎与服务层适配
1. **`include/nos/keyboard_engine.h`**：
   - 将 `struct nlk_keyboard_ctx` 中的 `uint8_t consumer_report;` 改为 `uint16_t consumer_report;`。
2. **`app/server/keyboard/keyboard_engine.c`**：
   - `kb_is_function_key(uint8_t code)` 检查范围更新为 `code >= NLK_KC_MEDIA_NEXT && code <= NLK_KC_MEDIA_BRIGHTNESS_DOWN`（以及 `KB_KEY_CHORD`）。
   - `kb_rebuild_hid_report()` 中使用 16-bit 标准 Consumer Usage 查找表：
     ```c
     static const uint16_t consumer_usages[] = {
         0x00B5U, /* NLK_KC_MEDIA_NEXT: Scan Next Track */
         0x00B6U, /* NLK_KC_MEDIA_PREV: Scan Previous Track */
         0x00CDU, /* NLK_KC_MEDIA_PLAY_PAUSE: Play/Pause */
         0x00E2U, /* NLK_KC_MEDIA_MUTE: Mute */
         0x00E9U, /* NLK_KC_MEDIA_VOL_UP: Volume Increment */
         0x00EAU, /* NLK_KC_MEDIA_VOL_DOWN: Volume Decrement */
         0x006FU, /* NLK_KC_MEDIA_BRIGHTNESS_UP: Display Brightness Increment */
         0x0070U, /* NLK_KC_MEDIA_BRIGHTNESS_DOWN: Display Brightness Decrement */
     };
     ```
   - 当检测到按键码落在 `NLK_KC_MEDIA_NEXT..NLK_KC_MEDIA_BRIGHTNESS_DOWN` 时，将 `ctx->consumer_report` 设为对应的 16-bit Usage ID（若同时按住多个媒体键，取最后一个有效媒体键 Usage ID；全部松开时为 `0x0000U`）。
3. **`app/server/keyboard/keyboard_server.c`**：
   - `struct ks_hid_state` 中的 `uint8_t media;` 改为 `uint8_t media[2];`（小端编码：`media[0] = (uint8_t)(consumer & 0xFFU)`，`media[1] = (uint8_t)(consumer >> 8U)`）。
   - `ks_on_hid_report()` 比较 2 字节 `media` 是否变化，变化时置位 `KS_HID_MEDIA`。
   - `ks_flush_usb_reports()` 向 `hidd` 设备写入 `HID_REPORT_ID_MEDIA` 及 2 字节 `state->media`。

## 4. 错误处理与兼容性保证

- **按键跨层释放安全**：`kb_is_function_key()` 包含新增的亮度键码，用户在按住 `PN + 1/2` 调节亮度或按住 `PN + P/[/]` 截图时，即使先松开 `PN` 键再松开目标键，引擎也会保持 `active_kc` 并正常发送 `0x0000` 释放报告，绝不卡键。
- **配置尺寸与验证**：`macos.json` 打包后的 TLV 文档包含 1 条 Metadata、1 条 Base Header 和 183 条 Base Key 记录，总大小约 2.1 KiB，远低于 `KB_STORE_MAX_BODY` (8192 字节) 上限。

## 5. 测试验证计划

1. **配置打包测试 (`tests/test_keyboard_config_packer.py`)**：
   - 验证 `nk60-v1` 和 `nk60-v2` 的 `macos.json` 能正常打包为完整 TLV 文档。
   - 验证 Layer 1 为标准 `F1`~`F12`，Layer 2 包含亮度、媒体、音量键码及 `key_chord`、`light_prev`、`light_next`、`light_toggle` 动作绑定。
2. **键盘引擎运行时测试 (`tests/host/test_keyboard_cap_runtime.c`)**：
   - 验证 `NLK_KC_MEDIA_VOL_UP`、`NLK_KC_MEDIA_BRIGHTNESS_UP`、`NLK_KC_MEDIA_BRIGHTNESS_DOWN` 生成正确的 16-bit `consumer_report` (`0x00E9`, `0x006F`, `0x0070`) 及释放归零。
3. **USB HID 安全测试 (`tests/host/test_usb_safety.c`)**：
   - 验证 `_report_desc` 包含 16-bit Consumer Control 描述符片段，且 `HID_REPORT_ID_MEDIA` 正常收发 2 字节负载（共 3 字节 USB 包）。
4. **全量回归与板级构建**：
   - 运行 `python3 tests/run_host_regressions.py`、`python3 -m unittest discover -s tests -p 'test_*.py'`。
   - 构建 `nk60-v2` 等预设并确保零错误、零警告。
