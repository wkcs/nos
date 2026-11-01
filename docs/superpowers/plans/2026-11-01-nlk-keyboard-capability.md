# NLK 键盘能力与底层驱动实施计划

- **目标**：完善 NLK 键盘能力（多重布局、多重背光、按键映射与多层配置、键盘宏录制配置、动作绑定与元数据、键盘参数配置）并完善底层键盘驱动。

---

## 任务拆解与进度

### 阶段 1：UAPI 与内核能力头文件更新
- [ ] 1.1 更新 `include/uapi/nos/nlk.h`：添加新增的 opcode、预定义布局 ID、背光区域结构体、层信息结构体、宏定义与动作描述符、参数结构体等。
- [ ] 1.2 更新 `include/nos/nlk_caps.h`：在 `struct nlk_keyboard_ctx` 中加入多布局、多区域背光、扩展层（支持最大 8 层）、宏结构体、动作绑定注册表、参数配置结构体等，并增加对应的 C 接口声明。
- [ ] 1.3 更新 `include/nos/keyboard.h`：底层驱动控制命令 `KEY_CTRL_*` 扩展（防抖、回报率、统计信息等），`struct keyboard_profile` 扩展多重布局和背光描述，按键动作元数据等。

### 阶段 2：NLK 键盘能力服务端实现 (`app/server/nlk/caps/cap_keyboard.c`)
- [ ] 2.1 布局操作：实现 `NLK_KB_OP_GET_LAYOUT_LIST` 与 `NLK_KB_OP_SET_LAYOUT`。
- [ ] 2.2 背光多区域操作：实现 `NLK_KB_OP_GET_LIGHT_ZONES`、`NLK_KB_OP_GET_ZONE_LIGHT_CFG`、`NLK_KB_OP_SET_ZONE_LIGHT_CFG`。
- [ ] 2.3 按键层操作：扩展支持最大 8 层，实现 `NLK_KB_OP_GET_LAYERS` 与 `NLK_KB_OP_SET_ACTIVE_LAYER`。
- [ ] 2.4 宏录制与配置：实现 `NLK_KB_OP_GET_MACRO_LIST`、`NLK_KB_OP_GET_MACRO`、`NLK_KB_OP_SET_MACRO`、`NLK_KB_OP_RECORD_MACRO`，以及键盘按键扫描中的实时宏录制逻辑与回放分发。
- [ ] 2.5 动作绑定：实现 `nlk_cap_keyboard_register_action` 注册 API 及 RPC `NLK_KB_OP_GET_ACTIONS`，分发触发注册的 handler。
- [ ] 2.6 参数配置：实现 `NLK_KB_OP_GET_PARAMS`、`NLK_KB_OP_SET_PARAMS`、`NLK_KB_OP_RESET_PARAMS`。

### 阶段 3：底层驱动与键盘服务对接
- [ ] 3.1 完善 `board/nk60-v2/key_matrix.c`：支持 `KEY_CTRL_SET_DEBOUNCE`, `KEY_CTRL_SET_POLL_RATE`, `KEY_CTRL_GET_STATS`。
- [ ] 3.2 完善 `board/nk60-v2/nk60_keyboard.c`：注册多重布局（标准 ANSI-61 与 Arrow-64 布局）、多重背光区域（RGB 全键、底光氛围灯、指示灯）、注册自定义动作列表（带名字与描述信息）、默认宏与多层配置。
- [ ] 3.3 完善 `app/server/keyboard/keyboard_server.c`：对接新的 profile 字段，支持参数动态下发底层驱动、多区域背光同步、动作绑定执行回调与宏触发。

### 阶段 4：回归测试与验证
- [ ] 4.1 在 `tests/host/test_nlk.c` 中添加针对全部 6 项新能力的自动化回归测试。
- [ ] 4.2 运行 `python3 tests/host/run_nlk_test.py` 验证功能全绿。
- [ ] 4.3 运行 `python3 tests/run_host_regressions.py` 验证系统未受破坏。
- [ ] 4.4 编译全板级 presets 并检查警告与错误。
