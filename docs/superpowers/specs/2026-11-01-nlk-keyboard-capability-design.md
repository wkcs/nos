# NLK 键盘能力与底层驱动重构设计规范

- **日期**：2026-11-01
- **模块**：NLK 协议 (`include/uapi/nos/nlk.h`, `include/nos/nlk_caps.h`), 键盘协议栈 (`app/server/nlk/caps/cap_keyboard.c`), 键盘服务与驱动 (`include/nos/keyboard.h`, `app/server/keyboard/keyboard_server.c`, `board/nk60-v2/`)

---

## 1. 需求与设计目标

为了让 NOS 具备业界顶级的可定制键盘生态支持，全面完善 NLK 协议中关于 `NLK_CAP_KEYBOARD` (0x0100) 的软总线能力，并打通底层键盘硬件驱动：

1. **多重键盘布局支持**：
   - NLK 协议预定义标准形态布局（如 ANSI 100%、TKL 87%、75%、65%、标准 60% ANSI、方向键 60%、HHKB、Alice、分体式、小键盘等）。
   - 由具体 NLK 设备注册方（如 `board/nk60-v2`）注册自身支持的具体布局列表，并支持运行时查询与动态切换当前布局。
2. **键盘多重背光配置与布局**：
   - 背光支持多重背光布局与区域划分（单色背光 Mono、按键独立 RGB 矩阵、周边氛围灯/底灯 Underglow、状态指示灯 Indicators、Logo 灯等）。
   - 由设备注册方注册自身具备的背光区域与规格，NLK 支持查询区域列表并独立控制各区域的开关、特效、亮度、速度与 RGB 色彩。
3. **按键映射与按键层配置**：
   - 支持多层按键配置（具体支持多少层由注册方动态注册，协议扩展至最大 8 层）。
   - 支持层元数据查询、当前活动层/默认启动层查询与切换、以及各层的按键映射（Keymap）与动作映射（Actionmap）分段读写。
4. **键盘宏录制与配置**：
   - 协议提供标准宏数据结构（按键按下/释放/单击/延时等指令步），支持最大 8 个独立宏槽位。
   - 支持离线宏配置（查询/上传指定宏的内容），并支持动态实时宏录制（开始录制、按键时序捕捉、停止录制与持久化）。
   - 按键触发宏时由键盘核心引擎顺序回放。
5. **按键动作绑定与元数据注册**：
   - 动作绑定机制完全解耦，由设备注册方或系统服务注册动作列表，并提供动作名字（如 `"RGB_TOGGLE"`, `"WIN_LOCK"`）与描述信息（如 `"Toggle RGB backlight on/off"`）。
   - 协议提供动作列表查询接口，按键映射表可将任意物理键绑定到指定动作 ID。
6. **键盘参数配置**：
   - 提供完备的键盘参数配置结构（消抖时间 `debounce_ms`、回报率 `poll_rate_hz`、6KRO/NKRO 切换、Win 键锁定、FN 键锁定、休眠超时、激活布局、默认层等）。
   - 支持参数查询、设置以及恢复出厂默认值。
7. **底层键盘驱动与板级适配**：
   - 扩展键盘底层驱动控制命令（防抖设置、回报率配置、统计信息读取等）。
   - 完善 `board/nk60-v2/` 键盘硬件驱动与 profile 注册，完整上报多重布局、多重背光区域、按键层、宏及动作绑定。

---

## 2. 协议 (UAPI) 详细定义 (`include/uapi/nos/nlk.h`)

### 2.1 新增 Opcode
```c
enum nlk_kb_opcode {
    NLK_KB_OP_GET_DESC              = 0x01U,
    NLK_KB_OP_GET_KEYMAP            = 0x02U,
    NLK_KB_OP_SET_KEYMAP            = 0x03U,
    NLK_KB_OP_GET_SET_PARAM         = 0x04U,
    NLK_KB_OP_GET_LIGHT_CFG         = 0x05U,
    NLK_KB_OP_SET_LIGHT_CFG         = 0x06U,
    NLK_KB_OP_SET_INDICATORS        = 0x07U,
    NLK_KB_OP_GET_ANALOG_CFG        = 0x08U,
    NLK_KB_OP_SET_ANALOG_CFG        = 0x09U,
    NLK_KB_OP_GET_NKRO_REPORT       = 0x0AU,
    /* 新增扩展能力 Opcode */
    NLK_KB_OP_GET_LAYOUT_LIST       = 0x0BU, /* 查询支持的布局列表 */
    NLK_KB_OP_SET_LAYOUT            = 0x0CU, /* 切换当前布局 */
    NLK_KB_OP_GET_LIGHT_ZONES       = 0x0DU, /* 获取背光区域列表与布局 */
    NLK_KB_OP_GET_ZONE_LIGHT_CFG    = 0x0EU, /* 获取指定背光区域配置 */
    NLK_KB_OP_SET_ZONE_LIGHT_CFG    = 0x0FU, /* 设置指定背光区域配置 */
    NLK_KB_STREAM_LIGHT_FRAME       = 0x10U,
    NLK_KB_OP_GET_LAYERS            = 0x11U, /* 获取键盘层配置列表 */
    NLK_KB_OP_SET_ACTIVE_LAYER      = 0x12U, /* 切换当前激活层 */
    NLK_KB_OP_GET_MACRO_LIST        = 0x13U, /* 获取宏槽位列表与录制状态 */
    NLK_KB_OP_GET_MACRO             = 0x14U, /* 获取指定宏步骤数据 */
    NLK_KB_OP_SET_MACRO             = 0x15U, /* 设置指定宏步骤数据 */
    NLK_KB_OP_RECORD_MACRO          = 0x16U, /* 控制宏录制 (启动/停止/清空) */
    NLK_KB_OP_GET_ACTIONS           = 0x17U, /* 获取注册方注册的按键动作列表 */
    NLK_KB_OP_GET_PARAMS            = 0x18U, /* 获取完整键盘运行参数 */
    NLK_KB_OP_SET_PARAMS            = 0x19U, /* 设置键盘运行参数 */
    NLK_KB_OP_RESET_PARAMS          = 0x1AU, /* 重置键盘参数为默认值 */

    NLK_KB_EVT_SWITCH_DELTA         = 0x20U,
    NLK_KB_EVT_HID_REPORT           = 0x21U,
    NLK_KB_EVT_LIGHT_SYNC_BEACON    = 0x22U,
    NLK_KB_EVT_NKRO_REPORT          = 0x23U,
};
```

### 2.2 多重布局预定义与结构体
```c
#define NLK_KB_LAYOUT_STD_60_ANSI    0x01U  /* 标准 60% ANSI (61键) */
#define NLK_KB_LAYOUT_STD_60_ISO     0x02U  /* 标准 60% ISO (62键) */
#define NLK_KB_LAYOUT_ARROW_60       0x03U  /* 60% 带独立方向键 (64键) */
#define NLK_KB_LAYOUT_HHKB_60        0x04U  /* HHKB 布局 (60键) */
#define NLK_KB_LAYOUT_STD_65         0x05U  /* 65% 标准紧凑 (68键) */
#define NLK_KB_LAYOUT_STD_75         0x06U  /* 75% 紧凑配列 (84键) */
#define NLK_KB_LAYOUT_TKL_87_ANSI    0x07U  /* 87键 TKL ANSI */
#define NLK_KB_LAYOUT_TKL_88_ISO     0x08U  /* 88键 TKL ISO */
#define NLK_KB_LAYOUT_FULL_104_ANSI  0x09U  /* 104键 全尺寸 ANSI */
#define NLK_KB_LAYOUT_FULL_108_ANSI  0x0AU  /* 108键 全尺寸 ANSI */
#define NLK_KB_LAYOUT_ALICE_ERGO     0x0BU  /* Alice 人体工学布局 */
#define NLK_KB_LAYOUT_SPLIT_68       0x0CU  /* 分体式 68键 */
#define NLK_KB_LAYOUT_NUMPAD_21      0x0DU  /* 独立小键盘 21键 */
#define NLK_KB_LAYOUT_CUSTOM         0xFFU  /* 自定义矩阵 */

#define NLK_KB_MAX_LAYOUTS           4U
#define NLK_KB_LAYOUT_NAME_MAX       16U

struct nlk_kb_layout_info {
    uint8_t  layout_id;
    uint8_t  key_count;
    uint8_t  matrix_rows;
    uint8_t  matrix_cols;
    char     name[NLK_KB_LAYOUT_NAME_MAX];
} __attribute__((packed));

struct nlk_kb_layout_list_resp {
    uint8_t  layout_count;
    uint8_t  active_layout_id;
    uint16_t reserved;
    struct nlk_kb_layout_info layouts[NLK_KB_MAX_LAYOUTS];
} __attribute__((packed));

struct nlk_kb_set_layout_req {
    uint8_t  layout_id;
    uint8_t  reserved[3];
} __attribute__((packed));
```

### 2.3 背光多区域定义
```c
enum nlk_kb_light_type {
    NLK_KB_LIGHT_TYPE_NONE        = 0U,
    NLK_KB_LIGHT_TYPE_MONO        = 1U, /* 单色背光 */
    NLK_KB_LIGHT_TYPE_RGB_MATRIX  = 2U, /* 按键 RGB 矩阵 */
    NLK_KB_LIGHT_TYPE_UNDERGLOW   = 3U, /* 周边氛围灯 / 底灯条 */
    NLK_KB_LIGHT_TYPE_INDICATOR   = 4U, /* 状态指示灯 */
    NLK_KB_LIGHT_TYPE_LOGO        = 5U, /* Logo 标志灯 */
};

#define NLK_KB_MAX_LIGHT_ZONES       4U
#define NLK_KB_ZONE_NAME_MAX         16U

struct nlk_kb_light_zone_desc {
    uint8_t  zone_id;
    uint8_t  type;
    uint16_t led_start;
    uint16_t led_count;
    uint16_t supported_effects;
    char     name[NLK_KB_ZONE_NAME_MAX];
} __attribute__((packed));

struct nlk_kb_light_zones_resp {
    uint8_t  zone_count;
    uint8_t  global_enable;
    uint16_t reserved;
    struct nlk_kb_light_zone_desc zones[NLK_KB_MAX_LIGHT_ZONES];
} __attribute__((packed));

struct nlk_kb_zone_light_cfg {
    uint8_t  zone_id;
    uint8_t  enable;
    uint8_t  effect_id;
    uint8_t  brightness;
    uint8_t  speed;
    uint8_t  color_r;
    uint8_t  color_g;
    uint8_t  color_b;
} __attribute__((packed));
```

### 2.4 按键层配置
```c
#define NLK_KB_LAYER_NAME_MAX       16U
#define NLK_KB_MAX_LAYERS           8U

struct nlk_kb_layer_desc {
    uint8_t  layer_id;
    uint8_t  flags;
    char     name[NLK_KB_LAYER_NAME_MAX];
} __attribute__((packed));

struct nlk_kb_layers_resp {
    uint8_t  layer_count;
    uint8_t  active_layer;
    uint8_t  default_layer;
    uint8_t  reserved;
    struct nlk_kb_layer_desc layers[NLK_KB_MAX_LAYERS];
} __attribute__((packed));

struct nlk_kb_set_active_layer_req {
    uint8_t  active_layer;
    uint8_t  reserved[3];
} __attribute__((packed));
```

### 2.5 宏录制与配置
```c
#define NLK_KB_MAX_MACROS           8U
#define NLK_KB_MAX_MACRO_STEPS      32U
#define NLK_KB_MACRO_NAME_MAX       16U

enum nlk_kb_macro_action_type {
    NLK_KB_MACRO_ACT_NONE         = 0U,
    NLK_KB_MACRO_ACT_KEY_DOWN     = 1U,
    NLK_KB_MACRO_ACT_KEY_UP       = 2U,
    NLK_KB_MACRO_ACT_KEY_TAP      = 3U,
    NLK_KB_MACRO_ACT_DELAY_MS     = 4U,
};

struct nlk_kb_macro_step {
    uint8_t  type;
    uint8_t  key_code;
    uint16_t delay_ms;
} __attribute__((packed));

struct nlk_kb_macro_info {
    uint8_t  macro_id;
    uint8_t  step_count;
    uint8_t  repeat_count;
    uint8_t  flags;
    char     name[NLK_KB_MACRO_NAME_MAX];
} __attribute__((packed));

struct nlk_kb_macro_list_resp {
    uint8_t  macro_count;
    uint8_t  recording_macro_id;
    uint16_t reserved;
    struct nlk_kb_macro_info macros[NLK_KB_MAX_MACROS];
} __attribute__((packed));

struct nlk_kb_macro_cfg_hdr {
    uint8_t  macro_id;
    uint8_t  step_count;
    uint8_t  repeat_count;
    uint8_t  reserved;
    char     name[NLK_KB_MACRO_NAME_MAX];
} __attribute__((packed));

struct nlk_kb_macro_record_req {
    uint8_t  macro_id;
    uint8_t  record_cmd; /* 0 = STOP, 1 = START, 2 = CLEAR */
    uint16_t reserved;
} __attribute__((packed));
```

### 2.6 动作绑定
```c
#define NLK_KB_MAX_ACTIONS          16U
#define NLK_KB_ACTION_NAME_MAX      16U
#define NLK_KB_ACTION_DESC_MAX      32U

struct nlk_kb_action_desc {
    uint8_t  action_id;
    uint8_t  flags;
    uint16_t reserved;
    char     name[NLK_KB_ACTION_NAME_MAX];
    char     desc[NLK_KB_ACTION_DESC_MAX];
} __attribute__((packed));

struct nlk_kb_action_list_resp {
    uint8_t  action_count;
    uint8_t  reserved[3];
    struct nlk_kb_action_desc actions[NLK_KB_MAX_ACTIONS];
} __attribute__((packed));
```

### 2.7 键盘运行参数
```c
struct nlk_kb_params {
    uint8_t  debounce_ms;
    uint8_t  poll_rate_hz_div;
    uint8_t  nkro_enable;
    uint8_t  win_lock;
    uint8_t  fn_lock;
    uint8_t  sleep_timeout_min;
    uint8_t  active_layout_id;
    uint8_t  default_layer;
    uint16_t poll_rate_hz;
    uint16_t reserved;
} __attribute__((packed));
```
