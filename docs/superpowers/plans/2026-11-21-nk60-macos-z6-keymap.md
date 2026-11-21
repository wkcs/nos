# NK60 macOS (Keychron Z6 Ultra 风格) 按键映射与 16-bit Consumer 媒体键实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 USB HID Consumer Control 报告升级为 QMK/ZMK 标准 16-bit Consumer Usage Array (`0x0000..0x03FF`)，新增屏幕亮度加减媒体键码，并将 NK60-V1 与 NK60-V2 的 `configs/keyboard/<board>/macos.json` 更新为参考 Keychron Z6 Ultra 的 macOS 按键映射（Fn 层保留 F1~F12，PN 层提供屏幕亮度、调度中心、Spotlight 搜索、灯效切换与开关、曲目与音量、三种截图及立即锁屏）。

**Architecture:**
1. UAPI 层 (`include/uapi/nos/nlk.h`) 在 `enum nlk_kb_function_keycode` 中追加 `NLK_KC_MEDIA_BRIGHTNESS_UP` (`0xAB`) 与 `NLK_KC_MEDIA_BRIGHTNESS_DOWN` (`0xAC`)。
2. 键盘引擎 (`include/nos/keyboard_engine.h`, `app/server/keyboard/keyboard_engine.c`) 将 `consumer_report` 升为 `uint16_t`，通过 `consumer_usages[]` 表将 `NLK_KC_MEDIA_*` 映射为标准 16-bit HID Consumer Usage ID；键盘服务 (`app/server/keyboard/keyboard_server.c`) 与 USB HID 驱动 (`drivers/usb/device/class/hid.c`) 使用 2 字节小端 Consumer Report (Report ID 4) 上报。
3. 配置文件 `configs/keyboard/nk60-v1/macos.json` 与 `configs/keyboard/nk60-v2/macos.json` 定义三层映射及 `caps_lock`、`key_chord`、`light_prev`、`light_next`、`light_toggle` 动作绑定。

**Tech Stack:** C99 / GNU11, CMake + Ninja, Python 3 (`unittest`, `pack_keyboard_config.py`)

## Global Constraints

- C 代码缩进必须为 4 个空格，禁止使用 Tab，采用 K&R 大括号风格，代码注释与文档使用中文。
- `enum nlk_kb_function_keycode` 原有 `0xA5..0xAA` 编码顺序保持不变，新增 `NLK_KC_MEDIA_BRIGHTNESS_UP = 0xABU`、`NLK_KC_MEDIA_BRIGHTNESS_DOWN = 0xACU`。
- `struct nlk_keyboard_ctx` 体积必须满足 `sizeof(keyboard) <= 2000` 断言。
- USB HID Report ID 4 (`HID_REPORT_ID_MEDIA`) 描述符使用 16-bit Consumer Usage Array (`0x0000..0x03FF`, 2 字节负载，小端序)。

---

### Task 1: 升级 16-bit Consumer Control 协议、USB HID 描述符与键盘引擎/服务

**Files:**
- Modify: `include/uapi/nos/nlk.h:1244-1254`
- Modify: `include/nos/keyboard_engine.h:126-130`
- Modify: `app/server/keyboard/keyboard_engine.c:85-165`
- Modify: `app/server/keyboard/keyboard_server.c:55-60,180-195`
- Modify: `drivers/usb/device/class/hid.c:175-197`
- Test: `tests/host/test_keyboard_cap_runtime.c:219-270`
- Test: `tests/host/test_usb_safety.c:1147-1202`

**Interfaces:**
- Produces:
  - `enum nlk_kb_function_keycode` 新增 `NLK_KC_MEDIA_BRIGHTNESS_UP` (`0xABU`), `NLK_KC_MEDIA_BRIGHTNESS_DOWN` (`0xACU`)
  - `struct nlk_keyboard_ctx::consumer_report` 类型为 `uint16_t`
  - `HID_REPORT_ID_MEDIA` 报告负载长度为 2 字节 (`uint8_t media[2]`)

- [ ] **Step 1: 更新 `tests/host/test_keyboard_cap_runtime.c` 与 `tests/host/test_usb_safety.c` 编写失败测试**

在 `tests/host/test_keyboard_cap_runtime.c` 的 `test_hid_reports` 中验证 16-bit `consumer_report` (`0x00E9`, `0x006F`, `0x0070`)：

```c
static void test_hid_reports(void)
{
    struct nlk_keyboard_ctx keyboard;
    struct report_probe probe = {0};
    const uint8_t base[] = {4, 5, 6, 7, 8, 9, 10, 0xe0, NLK_KC_MEDIA_VOL_UP, 0xe8};
    const uint8_t fn[] = {20, 21, 22, 23, 24, 25, 26,
                          NLK_KC_MEDIA_BRIGHTNESS_UP, NLK_KC_MEDIA_BRIGHTNESS_DOWN, 0xe8};
    uint32_t bitmap = 1U << 5;

    assert(nlk_cap_keyboard_init_sized(NULL, &keyboard, 1, 10, 10, 2, capture_hid_report, &probe) ==
           0);
    keyboard.nkro_cb = capture_nkro_report;
    assert(nlk_cap_keyboard_load_layout(&keyboard, 0, base, sizeof(base)) == 0);
    assert(nlk_cap_keyboard_load_layout(&keyboard, 1, fn, sizeof(fn)) == 0);
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.last_report.keys[0] == 9);
    bitmap |= 1U;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.last_report.keys[0] == 9 && keyboard.last_report.keys[1] == 4);
    bitmap = 0x1ffU;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.last_report.report_id == 1 && keyboard.last_nkro.report_id == 2);
    assert(keyboard.last_report.modifiers == 1 && keyboard.last_nkro.modifiers == 1);
    assert(keyboard.consumer_report == 0x00E9U);
    for (uint8_t key = 4; key <= 10; key++) {
        assert((keyboard.last_nkro.bitmap[key >> 3] & (1U << (key & 7U))) != 0);
    }
    assert(!report_has_key(&keyboard, NLK_KC_MEDIA_VOL_UP));
    /* Fn preserves a held media key, then the next press uses the selected layer. */
    bitmap = (1U << 8) | (1U << 9);
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.active_layer == 1 && keyboard.consumer_report == 0x00E9U);
    bitmap = (1U << 7) | (1U << 9);
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.consumer_report == 0x006FU);
    bitmap = 1U << 9;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.consumer_report == 0U);
    bitmap = (1U << 8) | (1U << 9);
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.consumer_report == 0x0070U);
    bitmap = 1U << 9;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    bitmap |= 1U;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(report_has_key(&keyboard, 20));
    /* Replacing a configuration suppresses held keys until their release. */
    nlk_cap_keyboard_release_held_keys(&keyboard);
    assert(!report_has_key(&keyboard, 20));
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.last_report.keys[0] == 0);
    bitmap = 0;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    bitmap = 1;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.active_layer == 0 && report_has_key(&keyboard, 4));
    assert(probe.hid_count == probe.nkro_count && probe.hid_count >= 10);
    assert(memcmp(&probe.hid, &keyboard.last_report, sizeof(probe.hid)) == 0);
    assert(memcmp(&probe.nkro, &keyboard.last_nkro, sizeof(probe.nkro)) == 0);
    nlk_cap_keyboard_deinit(&keyboard);
}
```

在 `tests/host/test_usb_safety.c` 的 `test_hid_consumer_and_keyboard_share_endpoint` 中验证 16-bit Consumer 描述符与 2 字节负载：

```c
static void test_hid_consumer_and_keyboard_share_endpoint(void)
{
    struct udcd dcd = { .ops = &mock_udcd_ops };
    struct udevice device = { .dcd = &dcd, .state = USB_STATE_CONFIGURED };
    struct ufunction func = { .device = &device };
    struct uendpoint ep_in = { .ep_desc = (struct uendpoint_descriptor *)&_hid_comm_desc.ep_in_desc };
    struct uendpoint ep_out = { .ep_desc = (struct uendpoint_descriptor *)&_hid_comm_desc.ep_out_desc };
    struct hid_s hid = { .func = &func, .ep_in = &ep_in, .ep_out = &ep_out };
    const uint8_t consumer_collection[] = {
        0x05U, 0x0CU, 0x09U, 0x01U, 0xA1U, 0x01U, 0x85U, 0x04U,
        0x05U, 0x0CU, 0x15U, 0x00U, 0x26U, 0xFFU, 0x03U,
        0x19U, 0x00U, 0x2AU, 0xFFU, 0x03U, 0x75U, 0x10U, 0x95U, 0x01U, 0x81U, 0x00U
    };
    uint8_t keys[8] = { 8U, 0U, 4U };
    uint8_t volume[2] = { 0xE9U, 0x00U };
    uint8_t brightness[2] = { 0x6FU, 0x00U };
    uint8_t release[2] = { 0U, 0U };
    uint32_t first_session;
    uint32_t second_session;
    bool has_consumer = false;
    size_t i;

    for (i = 0; i + sizeof(consumer_collection) <= sizeof(_report_desc); i++) {
        if (memcmp(_report_desc + i, consumer_collection, sizeof(consumer_collection)) == 0) {
            has_consumer = true;
            break;
        }
    }
    assert(has_consumer);
    assert(_hid_comm_desc.intf_desc.bInterfaceSubClass == 0U);
    mutex_init(&hid.tx_lock);
    func.user_data = &hid;
    assert(hid_function_enable(&func) == 0);
    assert(_hid_control(&hid.dev, USB_HID_CTRL_GET_SESSION, &first_session) == 0);
    reset_mock_dcd_stats();
    assert(_hid_write(&hid.dev, HID_REPORT_ID_KEYBOARD1, keys, sizeof(keys)) == sizeof(keys));
    assert(mock_dcd_stats.write_len == 9U);
    assert(_hid_write(&hid.dev, HID_REPORT_ID_MEDIA, volume, sizeof(volume)) == -EAGAIN);
    assert(mock_dcd_stats.write_calls == 1);
    assert(((uint8_t *)mock_dcd_stats.write_buf)[0] == HID_REPORT_ID_KEYBOARD1);
    assert(hid_ep_in_handler(&func, 9U) == 0);
    mock_write_reject = true;
    assert(_hid_write(&hid.dev, HID_REPORT_ID_MEDIA, volume, sizeof(volume)) == -EAGAIN);
    mock_write_reject = false;
    assert(_hid_write(&hid.dev, HID_REPORT_ID_MEDIA, volume, sizeof(volume)) == sizeof(volume));
    assert(mock_dcd_stats.write_len == 3U);
    assert(((uint8_t *)mock_dcd_stats.write_buf)[0] == HID_REPORT_ID_MEDIA);
    assert(((uint8_t *)mock_dcd_stats.write_buf)[1] == 0xE9U);
    assert(((uint8_t *)mock_dcd_stats.write_buf)[2] == 0x00U);
    assert(hid_ep_in_handler(&func, 3U) == 0);
    assert(_hid_write(&hid.dev, HID_REPORT_ID_MEDIA, brightness, sizeof(brightness)) == sizeof(brightness));
    assert(((uint8_t *)mock_dcd_stats.write_buf)[1] == 0x6FU);
    assert(hid_ep_in_handler(&func, 3U) == 0);
    assert(_hid_write(&hid.dev, HID_REPORT_ID_MEDIA, release, sizeof(release)) == sizeof(release));
    assert(((uint8_t *)mock_dcd_stats.write_buf)[1] == 0U);
    assert(((uint8_t *)mock_dcd_stats.write_buf)[2] == 0U);

    assert(hid_function_disable(&func) == 0);
    assert(_hid_control(&hid.dev, USB_HID_CTRL_GET_SESSION, &second_session) == -ENOTCONN);
    assert(hid_function_enable(&func) == 0);
    assert(_hid_control(&hid.dev, USB_HID_CTRL_GET_SESSION, &second_session) == 0);
    assert(second_session != first_session);
    assert(_hid_write(&hid.dev, HID_REPORT_ID_MEDIA, release, sizeof(release)) == sizeof(release));
    assert(hid_function_disable(&func) == 0);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python3 tests/host/run_keyboard_cap_runtime_test.py`
Expected: FAIL (`NLK_KC_MEDIA_BRIGHTNESS_UP` undeclared)

- [ ] **Step 3: 实现 UAPI、引擎、服务与 USB HID 16-bit Consumer Control**

1. 修改 `include/uapi/nos/nlk.h:1244-1254`：
```c
/* Keymap codes in a reserved range of the HID Keyboard/Keypad page.
 * These are NOS functions and are never sent as USB keyboard usages. */
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

2. 修改 `include/nos/keyboard_engine.h:128`：
```c
    uint16_t consumer_report;
```

3. 修改 `app/server/keyboard/keyboard_engine.c:85-165`：
```c
static bool kb_is_function_key(uint8_t code)
{
    return (code >= NLK_KC_MEDIA_NEXT && code <= NLK_KC_MEDIA_BRIGHTNESS_DOWN) ||
           code == KB_KEY_CHORD;
}
```
并在 `kb_rebuild_hid_report()` 中将 `consumer_bits` 替换为 16-bit `consumer_usages`：
```c
    /* 标准 USB HID Consumer Page (0x0C) 16 位用途码映射表 */
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
并将循环内的媒体键处理改为：
```c
        if (kc >= NLK_KC_MEDIA_NEXT && kc <= NLK_KC_MEDIA_BRIGHTNESS_DOWN) {
            ctx->consumer_report = consumer_usages[kc - NLK_KC_MEDIA_NEXT];
            continue;
        }
```

4. 修改 `app/server/keyboard/keyboard_server.c:55-60` 与 `180-195`：
```c
struct ks_hid_state {
    uint8_t keys[8];
    uint8_t media[2];
    uint8_t pending;
};
```
在 `ks_on_hid_report()` 中：
```c
#ifdef CONFIG_USB_DEVICE_HID_MEDIA
    {
        uint8_t media[2] = {
            (uint8_t)(ks->kb_cap.consumer_report & 0xFFU),
            (uint8_t)(ks->kb_cap.consumer_report >> 8U),
        };
        if (memcmp(ks->hid_state.media, media, sizeof(media)) != 0) {
            memcpy(ks->hid_state.media, media, sizeof(media));
            pending |= KS_HID_MEDIA;
        }
    }
#endif
```
并在 `ks_flush_usb_reports()` 中将 `&state->media` 改为 `state->media`：
```c
        const uint8_t *buf = pending == KS_HID_KEYS ? state->keys : state->media;
```

5. 修改 `drivers/usb/device/class/hid.c:175-197`：
```c
    // Media Control
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
    INPUT(1),           0x00,             // Input (Data, Array, Absolute)
    END_COLLECTION(0),
#endif
```

- [ ] **Step 4: 运行主机测试验证通过**

Run: `python3 tests/host/run_keyboard_cap_runtime_test.py && python3 tests/run_host_regressions.py`
Expected: PASS

---

### Task 2: 更新 NK60-V1 / NK60-V2 的 `macos.json` 配置文件与打包回归测试

**Files:**
- Modify: `configs/keyboard/nk60-v1/macos.json`
- Modify: `configs/keyboard/nk60-v2/macos.json`
- Modify: `tests/test_keyboard_config_packer.py:28-39`

**Interfaces:**
- Consumes: `NLK_KC_MEDIA_NEXT..NLK_KC_MEDIA_BRIGHTNESS_DOWN` (`0xa5..0xac`)
- Produces: `configs/keyboard/nk60-v1/macos.json` 和 `configs/keyboard/nk60-v2/macos.json`

- [ ] **Step 1: 在 `tests/test_keyboard_config_packer.py` 中编写针对 Keychron Z6 Ultra 风格 macOS 配置的校验测试**

```python
    def test_board_presets_are_complete_documents(self):
        for board in ('nk60-v1', 'nk60-v2'):
            config = json.loads((ROOT / f'configs/keyboard/{board}/macos.json').read_text())
            doc = records(PACKER.pack(config))
            self.assertEqual(sum(tag == 1 for tag, _ in doc), 1)
            self.assertEqual(sum(tag == 5 for tag, _ in doc), 1)
            self.assertEqual(sum(tag == 6 for tag, _ in doc), 61 * 3)
            header = next(value for tag, value in doc if tag == 5)
            self.assertTrue(header.startswith(b'\x06ansi61\x01\x00'))
            key_records = {
                (value[0], struct.unpack_from('<H', value, 1)[0]): value
                for tag, value in doc if tag == 6
            }
            # Layer 1 (Fn): 1..= 为标准 F1..F12 (0x3a..0x45)
            for idx, expected_kc in enumerate(range(0x3a, 0x46), start=1):
                code = struct.unpack_from('<I', key_records[(1, idx)], 3)[0]
                self.assertEqual(code, expected_kc)
            # Layer 2 (PN): 亮度减/加 (0xac/0xab)、媒体与音量 (0xa6/0xa7/0xa5/0xa8/0xaa/0xa9)
            self.assertEqual(struct.unpack_from('<I', key_records[(2, 1)], 3)[0], 0xac)
            self.assertEqual(struct.unpack_from('<I', key_records[(2, 2)], 3)[0], 0xab)
            self.assertEqual(struct.unpack_from('<I', key_records[(2, 7)], 3)[0], 0xa6)
            self.assertEqual(struct.unpack_from('<I', key_records[(2, 8)], 3)[0], 0xa7)
            self.assertEqual(struct.unpack_from('<I', key_records[(2, 9)], 3)[0], 0xa5)
            self.assertEqual(struct.unpack_from('<I', key_records[(2, 10)], 3)[0], 0xa8)
            self.assertEqual(struct.unpack_from('<I', key_records[(2, 11)], 3)[0], 0xaa)
            self.assertEqual(struct.unpack_from('<I', key_records[(2, 12)], 3)[0], 0xa9)
            # Layer 2 (PN): 调度中心(Key 3)、搜索(Key 4)、灯效切换(Key 5/6)、三种截图(Key 24/25/26)、锁屏(Key 37)、灯光开关(Key 47)
            self.assertTrue(key_records[(2, 3)].endswith(b'key_chord\x01\x00\x01'))
            self.assertTrue(key_records[(2, 4)].endswith(b'key_chord\x01\x00\x08'))
            self.assertTrue(key_records[(2, 5)].endswith(b'light_prev\x00\x00'))
            self.assertTrue(key_records[(2, 6)].endswith(b'light_next\x00\x00'))
            self.assertTrue(key_records[(2, 24)].endswith(b'key_chord\x01\x00\x0a'))
            self.assertTrue(key_records[(2, 25)].endswith(b'key_chord\x01\x00\x0a'))
            self.assertTrue(key_records[(2, 26)].endswith(b'key_chord\x01\x00\x0a'))
            self.assertTrue(key_records[(2, 37)].endswith(b'key_chord\x01\x00\x09'))
            self.assertTrue(key_records[(2, 47)].endswith(b'light_toggle\x00\x00'))
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python3 -m unittest tests/test_keyboard_config_packer.py`
Expected: FAIL (`0x3f != 0x40` on Layer 1 key 7)

- [ ] **Step 3: 更新 `configs/keyboard/nk60-v1/macos.json` 与 `configs/keyboard/nk60-v2/macos.json`**

将两个文件更新为以下内容：

```json
{
  "name": "macOS",
  "description": "Keychron Z6 Ultra style macOS keymap with media, brightness and screenshots",
  "debounce_ms": 10,
  "poll_rate_hz": 100,
  "layers": [
    [
      "29 1e 1f 20 21 22 23 24 25 26 27 2d 2e 2a",
      "2b 14 1a 08 15 17 1c 18 0c 12 13 2f 30 31",
      "39 04 16 07 09 0a 0b 0d 0e 0f 33 34 28",
      "e1 1d 1b 06 19 05 11 10 36 37 38 e5",
      "e0 e2 e3 2c e7 e9 e4 e8"
    ],
    [
      "35 3a 3b 3c 3d 3e 3f 40 41 42 43 44 45 4c",
      "00 00 52 00 00 00 00 00 00 00 00 00 00 49",
      "00 50 51 4f 00 00 46 47 48 00 00 4d 00",
      "e1 00 00 00 00 00 00 00 4b 4e 52 e5",
      "e0 e2 e3 00 50 51 4f 00"
    ],
    [
      "35 ac ab 52 2c 00 00 a6 a7 a5 a8 aa a9 4c",
      "00 00 52 00 00 00 00 00 00 00 20 21 22 49",
      "00 50 51 4f 00 00 46 47 48 14 00 4d 00",
      "e1 00 00 00 00 00 00 00 00 00 00 e5",
      "e0 e2 e3 00 e7 00 e4 00"
    ]
  ],
  "actions": [
    {
      "layer": 0,
      "key": 28,
      "name": "caps_lock"
    },
    {
      "layer": 2,
      "key": 3,
      "name": "key_chord",
      "arguments": {
        "modifiers": 1
      }
    },
    {
      "layer": 2,
      "key": 4,
      "name": "key_chord",
      "arguments": {
        "modifiers": 8
      }
    },
    {
      "layer": 2,
      "key": 5,
      "name": "light_prev"
    },
    {
      "layer": 2,
      "key": 6,
      "name": "light_next"
    },
    {
      "layer": 2,
      "key": 24,
      "name": "key_chord",
      "arguments": {
        "modifiers": 10
      }
    },
    {
      "layer": 2,
      "key": 25,
      "name": "key_chord",
      "arguments": {
        "modifiers": 10
      }
    },
    {
      "layer": 2,
      "key": 26,
      "name": "key_chord",
      "arguments": {
        "modifiers": 10
      }
    },
    {
      "layer": 2,
      "key": 37,
      "name": "key_chord",
      "arguments": {
        "modifiers": 9
      }
    },
    {
      "layer": 2,
      "key": 47,
      "name": "light_toggle"
    }
  ],
  "layout": "ansi61",
  "layout_version": 1
}
```

- [ ] **Step 4: 运行 Python 单元测试与板级构建验证**

Run: `python3 -m unittest discover -s tests -p 'test_*.py' && cmake --preset nk60-v2 && cmake --build --preset nk60-v2`
Expected: PASS 且零编译错误、零警告。
