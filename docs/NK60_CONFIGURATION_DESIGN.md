# NK60 configuration system: proposed design

Status: the current named-object codec, filesystem store, NLK file operations,
and non-blocking timed macro engine are implemented alongside the legacy
four-slot protocol.

## References and existing constraints

QMK represents keymaps as layers of hardware-layout-specific keys. VIA exposes
layout identifiers, switch-matrix coordinates, macros, lighting and save/load
to a graphical client. See [QMK keymaps](https://docs.qmk.fm/keymap),
[VIA layouts](https://caniusevia.com/docs/layouts/) and
[VIA custom UI](https://www.caniusevia.com/docs/custom_ui/).

NK60-V2 has 61 physical keys and three default layers. `/data` is the
external-flash partition, mounted as LittleFS by default. The root filesystem
can instead be RAMFS. A successful configuration save must not be reported as
persistent unless the configured persistent filesystem is actually mounted.
The current NK60 LED driver drives 61 key LEDs; the capability advertises one
key-light zone, rather than unimplemented underglow and dedicated indicators.
NK60 uses size-optimized compilation to leave room within its 240 KiB
internal-flash link limit without removing its shell and protocol services.

## Implemented interface and current limits

`NLK_KB_OP_CONFIG_FILES_INFO` reports the hardware identity, runtime limits and
the independently selected keyboard and lighting object IDs. `LIST` returns one
entry per request and a 32-bit cursor; continue until `eof` is set. `SAVE`,
`LOAD` and `DELETE` use `nlk_kb_config_file_req`; `SAVE` creates a new object
and returns `EEXIST` for an existing ID. IDs contain 1–23 ASCII letters, digits,
underscores or hyphens; display names are separate NUL-terminated UTF-8 byte
strings of at most 31 bytes. Loading a macro copies it into `macro_id`; keyboard
profiles embed the current macro contents. To bind a macro slot, set the keycode
to `NLK_KB_MACRO_KEYCODE_BASE + slot` and bind the `macro_play` action. The
legacy `macro_play` binding with another keycode continues to select slot zero.

Named objects are stored in three subdirectories under `/data/keyboard`:
`/data/keyboard/config/<id>.nkc`, `/data/keyboard/light/<id>.nkc` and
`/data/keyboard/macro/<id>.nkc`. NK60-V1 and NK60-V2 builds produce separate macOS
profile for installation in this filesystem. The currently active configuration
(including the active keyboard and
lighting object IDs, keyboard state and lighting state) is stored in
`/data/keyboard/active.nkc`, restored on boot and updated automatically when
the user modifies runtime configuration or saves/loads a named profile. The
NK60 backend checks that `/data` is the expected external-flash LittleFS or FatFs mount;
it does not use RAMFS as a substitute. The active named object cannot be
deleted until another one is selected. Failed `active.nkc` updates during
loading restore the previous runtime configuration. Storage is subject to
filesystem and available-space limits, rather than a fixed count of saved
profiles.

Macro files are the authoritative catalog. Runtime macro objects and embedded
profile snapshots are caches, not additional device macros. Deleting a macro
removes its cached steps and matching `macro_play` bindings from the active state
and all saved keyboard profiles before unlinking the library file. Loading a
snapshot also discards cached macros whose library file no longer exists.

Delayed active-state writes run between NLK requests on the existing service
stack, rather than reserving another task stack. The keyboard scanner retains
its higher priority, and flash I/O runs without the keyboard lock. The stack
regression measures metadata compaction, wear leveling, and exception context
for both persistence and macro deletion.

Each object kind has one current layout, selected by `keyboard_config_version()`:
keyboard profiles use version 5, active snapshots use version 4, and lighting
profiles and macros use version 3. Readers accept only the current version for
the requested kind; historical parsing and fallback paths are removed. Existing
device files must be converted once or regenerated and uploaded before loading.

The header and fixed-width fields use little-endian layout. Files
carry model/revision/layout identity, a name, dimensions, length and CRC-32.
Key entries contain a keycode and only the action names actually bound; macros
contain only their actual steps. Lighting effects are stored by name and mapped
to the receiver's local IDs. A file is validated before it is applied.

Macros execute asynchronously on a 1 ms tick state machine without blocking
matrix scanning or lighting rendering. They support `KEY_DOWN`, `KEY_UP`,
`KEY_TAP`, `DELAY_MS`, modifier chords, repeat counts, real elapsed delay
recording (splitting delays above 65535 ms into `DELAY_MS` steps and setting
`NLK_KB_MACRO_FLAG_RECORD_OVERFLOW` on step exhaustion), `NLK_KB_OP_MACRO_CONTROL`
(`PLAY`/`STOP`), `NLK_KB_OP_MACRO_STATUS`, and pressing a macro key again to
cancel an active playback.

Layout geometry rendering is handled by the Web client based on the layout ID
reported by the keyboard (`NLK_KB_OP_GET_LAYOUT_LIST` / `active_layout_id`), so
firmware does not store visual keycap geometry tables. Legacy slot files
(`profile_0..3.bin`, `active_slot.bin`) remain manual to clean up or archive.

## Objects and identity

There are three independently named file types: keyboard profiles, lighting
profiles and macros. Each contains a version, type, display name, stable object
identifier, hardware compatibility identifier, length and checksum. The
compatibility identifier identifies the keyboard model, hardware revision and
physical layout schema, not an individual unit: compatible NK60-V2 keyboards
can share files. A different revision or layout must be explicitly declared
compatible; a matching key count alone is insufficient. IDs and display names
are distinct, so names can be changed without breaking references.

Keyboard profiles contain parameters, selected physical layout, default layer,
per-layer key mappings and action bindings, as well as the macro content needed
by those bindings. A macro may also be exported as an independent named file
and imported into another keyboard profile. Import copies the macro content;
editing or deleting the independent file cannot silently change an existing
profile. Lighting profiles contain global and per-zone settings and refer to
device effect names, not registration-order-dependent numeric IDs. Each object
is validated completely before any live state changes. Unknown versions,
unavailable actions or effects, malformed lengths, checksum failures and
incompatible hardware are errors, not reasons to partially apply a file.

File payloads contain only actual keys, layers, populated bindings, zones and
macro steps. They do not embed `NLK_KB_MAX_KEYS` or a fixed number of profile
slots. Object counts are bounded by available filesystem space and filesystem
limits, not an in-memory table. Separate resource limits, such as the number of
simultaneously active macros or layers, must be discoverable by the client and
must not be confused with the number of saved files.

## NLK and graphical clients

Retain legacy slot commands for old clients and add versioned keyboard capability
operations to discover supported file operations, hardware compatibility and
resource limits; enumerate objects by type with a continuation cursor; inspect
their IDs, names, versions and sizes; load, delete and save the current keyboard
or lighting state as a new named object; and import/export individual macros.
The file count must not be represented by an eight-bit mask. The client can use
NLK VFS for chunked file transfer; the keyboard capability must provide the
type-aware listing and validation rather than requiring a GUI to interpret an
arbitrary filesystem directory.

Expose the layout identifier, logical key indices, matrix dimensions,
available actions, layers, lighting zones and effect parameters so a Web client
can construct the controls without NK60-specific constants. A successful edit
updates the current runtime state; saving as a new object is an explicit action.
The protocol must distinguish an unsaved working state from the most recently
loaded or saved object. Large replies and objects require bounded chunks and
pagination; transfer state must be scoped to a connection and cleaned up when
the client disconnects. Browser transport support, including USB descriptors
and platform permissions, needs an end-to-end WebUSB compatibility test rather
than assuming that an existing WinUSB interface is sufficient.

## Persistence and failure handling

Write and validate a new temporary object before making it visible in listings;
do not truncate an existing valid profile while saving. Preserve the previous
active object and working state if the filesystem fills, a transfer is cut
short, validation fails or a commit fails. Recovery after interruption must be
tested against the actual FAT and flash implementation; do not assume rename is
power-loss atomic. The active keyboard and lighting selections are independent
and their updates must survive reboot only after the corresponding objects are
durable. If persistent storage is unavailable, return a meaningful error rather
than silently selecting RAMFS.

Provide explicit import of existing version 2 slot files. Their mixed lighting
and keyboard data must be split during migration, and their dimensions and
hardware compatibility checked before use. Existing slot files do not contain
macro data or a hardware identity, so migration cannot reconstruct missing
macros or prove their original hardware provenance; it must report those limits
and leave the source file untouched.

## 宏与配置解耦及 32 位键码/清单索引（Version 2 规范）

### 1. 核心改进背景与设计原则
在 NK60 初始实现（Version 1）中，宏与键盘配置文件存在强耦合关系（宏被内嵌在键盘配置文件中），且动作触发基于固定槽位编号（Slot），存在以下局限性：
1. **槽位限制**: 用户受限于固定的宏槽位，无法跨配置共享宏定义。
2. **标识信息不完整**: 配置和宏缺乏明确的机器唯一标识（ID）、用户可读名称（Name）和详细功能说明（Desc）。
3. **检索性能瓶颈**: 键盘按键触发动作（如跳转指定配置、播放特定宏）若实时扫描文件系统遍历 `.nkc` 文件，在微控制器（MCU）与低速存储介质（如 SPI Flash）上会导致显著的输入延迟；若全量缓存又会耗尽受限的 RAM 资源。

为此，Version 2 设计全面解耦宏与配置，引入 32 位通用键码与常驻内存的轻量级清单索引表（Manifest）。

---

### 2. 对象标识三元组体系与灯效规范
在系统内，键盘配置、灯效配置和宏均统称为具名对象（Named Object），统一采用结构化三元组标识：
- **`id` (1–23 字符 ASCII)**: 机器与文件系统级别的唯一标识符。对应外部存储路径文件名：
  - 键盘配置: `/data/keyboard/config/<id>.nkc`
  - 灯效配置: `/data/keyboard/light/<id>.nkc`
  - 宏定义: `/data/keyboard/macro/<id>.nkc`
- **`name` (最长 31 字节 UTF-8 字符串)**: 面向人机界面的友好展示名称（如 `"Programming Mode"`、`"Vim Copy"`）。
- **`desc` (最长 63 字节 UTF-8 字符串)**: 对象的详细功能说明（如 `"Caps mapped to Ctrl, Arrow keys enabled on layer 1"`），帮助用户明确该配置或宏的具体用途。

#### 灯效体系解耦
- 灯效配置独立于按键配置，用户可在不同按键布局下保持相同的灯效氛围，或在不同场景下独立切换。
- 灯效效果（Effect）通过字符串名称解析绑定到接收端的本地效果驱动，不再依赖枚举注册顺序。

---

### 3. 32 位通用键码（32-bit Keycode）与 24 位 FNV-1a 短哈希
为了在单个键码中完整携带动作参数，微内核将按键流转与按键映射表（Keymap）从 8 位扩展为 32 位：

```text
 31                          8 7                   0
+-----------------------------+---------------------+
|      Action Param / Hash    |    Base HID Keycode |
|           (24 bits)         |        (8 bits)     |
+-----------------------------+---------------------+
```

- **低 8 位 (`kc & 0xFFU`)**: 标准 USB HID 键码（如 `KC_A`, `KC_LCTRL`）。当按键未绑定自定义动作时，系统向主机上报该基础键码，确保硬件透明性与标准兼容。
- **高 24 位 (`kc >> 8U`)**: 动作参数扩展字段。对于宏触发和配置切换动作，高 24 位存放目标对象 `id` 的 **24-bit FNV-1a 短哈希**：
  - `target_hash = nlk_fnv1a_24(target_id)`
  - 宏播放动作 (`macro_play`): 高 24 位哈希非 0 时，直接以此短哈希检索宏；若为 0，则兼容回退为槽位索引或当前激活宏。
  - 配置切换动作 (`config_select`): 高 24 位哈希用于精确定位目标键盘配置，实现“一键直达指定配置”。

---

### 4. 内存轻量级清单索引表（Manifest）与 $O(1)$ 常数时间检索
为兼顾实时性与低 RAM 占用，键盘服务在内存中常驻一份轻量级清单索引表（`struct nlk_kb_manifest`）：

```c
struct nlk_kb_manifest_entry {
    uint32_t hash24;               /* 24-bit FNV-1a 目标 ID 哈希 */
    char id[NLK_KB_CONFIG_ID_SIZE]; /* 对象文件名 ID (24 字节) */
};

struct nlk_kb_manifest {
    uint8_t config_count;
    uint8_t macro_count;
    struct nlk_kb_manifest_entry configs[NLK_KB_MANIFEST_MAX_CONFIGS]; /* 最多 16 个配置 */
    struct nlk_kb_manifest_entry macros[NLK_KB_MANIFEST_MAX_MACROS];   /* 最多 16 个宏 */
};
```

#### 工作流与维护策略
1. **启动与挂载初始化**:
   - 在系统启动挂载 `/data` 文件系统时，调用 `keyboard_config_manifest_build()` 快速扫描 `/data/keyboard/config` 与 `/data/keyboard/macro` 目录，提取文件 ID 并计算 24 位哈希，填充至 `manifest`。
2. **动态保存与删除同步**:
   - 当上位机通过 NLK RPC 执行 `SAVE` 或 `DELETE` 操作时，存储模块调用 `keyboard_config_manifest_update()` 同步原子更新清单，保证内存索引与文件系统始终一致。
3. **按键常数时间检索**:
   - `macro_play` 在已装载的宏中匹配目标。`config_select` 将哈希交给后台任务，在 Flash 中校验目标并切换，键盘扫描可继续运行。

---

### 5. NLK MTU 契约与 RPC 通信安全
由于底层 NLK 报文严格受限于硬件单包传输上限（`CONFIG_NLK_PACKET_MTU = 256` 字节），任何 RPC 结构体严禁打破此上限：
- **宏列表查询 (`NLK_KB_OP_GET_MACRO_LIST`)**:
  - `struct nlk_kb_macro_info` 仅保留 `id[12]`、`name[14]` 及元数据（单项共 30 字节），8 个宏总长 240 字节，加上响应头严格限制在 244 字节内。
  - 详细描述（`desc`）通过单项宏查询 `NLK_KB_OP_GET_MACRO` 或文件系统分页查询 `NLK_KB_OP_CONFIG_FILES_LIST` 获取，杜绝栈与堆缓冲区写穿溢出。
- **动作查询 (`NLK_KB_OP_GET_ACTIONS`)**:
  - 键盘可注册 24 个动作。`GET_ACTIONS` 保留 244 字节的原有响应并返回前 6 项。
  - `NLK_KB_OP_GET_ACTIONS_PAGE`（`0x32`）接收 4 字节请求，首字节是从零开始的索引，其余置零。响应头包含总数、当前页项目数和两个保留字节，随后是当前页实际返回的动作描述符。将索引加上返回项目数，直至达到总数。

## 安装 NK60-V1 / NK60-V2 的 macOS 配置

在 NOS 根目录构建并上传数据文件：

```sh
cmake --preset nk60-v2
cmake --build --preset nk60-v2
nlk push build/nk60-v2/data/keyboard/config/macos.nkc /data/keyboard/config/macos.nkc
nlk keyboard config load macos
```

NK60-V1 使用 `nk60-v1` preset，上传
`build/nk60-v1/data/keyboard/config/macos.nkc`。两个版本的文件分别带有
`NK60-V1-ANSI61-v1` 和 `NK60-V2-ANSI61-v1` 硬件标识，加载器会校验匹配关系。
连接多台 NOS 设备时在 `nlk` 后加 `-s <序列号>` 选择目标。

`/data` 由 NK60 的外置 Flash 提供。更新键位时可编辑
对应板型的 `configs/keyboard/nk60-v1/macos.json` 或
`configs/keyboard/nk60-v2/macos.json`，重新构建数据文件并上传。JSON 的三层依次是基础、Fn 和 PN。每一项是十六进制键码，按 ANSI 61 的物理键顺序排列。
对于 `key_chord`，最低字节是键码，下一字节是 HID 修饰键位图，例如 `0a20` 表示 Shift + Command + 3。

Mac 底排为 Control、Option、Command、空格、右 Command、PN、右 Control、Fn。
Fn + 7 / 8 / 9 用于上一曲、播放或暂停、下一曲。Fn + 0 / - / = 用于静音、减小和增大音量。
Fn + P 截取全屏，Fn + [ 选择区域，Fn + L 锁屏。PN + 1 到 PN + = 对应 F1 到 F12。
字符由 macOS 当前输入源解释，可直接使用系统的 ANSI 键盘布局。
截屏和锁屏组合采用 Apple 的默认系统快捷键，修改过系统快捷键时也应相应编辑配置。
参考 [Apple 截屏说明](https://support.apple.com/en-us/102646) 和
[Mac 键盘快捷键](https://support.apple.com/en-us/102650)。

可以在当前配置把 Fn + M（层 1，索引 48）绑定到 Flash 中名为 `macos` 的配置：

```sh
nlk keyboard bind 1 48 config_select --target macos
```

先将常用布局保存为一个命名配置，再用相同方式把按键绑定到其 ID，即可切回。
`config_select` 与 `macro_play` 共用 `BIND_ACTION` 的目标参数。固件按配置文件 ID 的 FNV-1a 24 位哈希查找目标，匹配多个记录时保持当前配置。

---

## Verification required before release

Host tests should cover round trips for all three object types, name and ID
handling, more than four files, pagination, cross-profile macro import, exact
hardware-sized data, invalid dimensions and incompatible identities, corrupt or
truncated inputs, insufficient flash, interrupted saves and rollback. Exercise
the same operations through NLK including multiple chunks and reconnects. Test
boot selection, storage unavailability and power interruption on NK60 hardware,
then check all board builds, internal-flash headroom and a real browser client.
