# Findings

- NOS 起始工作区干净，master 比 origin/master 超前 2 个已有提交，HEAD 14f678b。
- 外部客户端在 `/Volumes/nick-disk/projects/nk60/nlk_cli` 与 `/Volumes/nick-disk/projects/nk60/nk60_web`；Web 仓含原生 CLI 子模块和 Rust desktop workspace。
- QMK RGB Matrix 可模块扩展并提供不写 EEPROM 的接口；VIA Custom UI 绑定类型和字段 ID，区分 get/set/save；ZMK 灯效快捷键采用延迟保存；littlefs 提供单操作原子性及动态磨损均衡，NOS VFS 是否具备对应 API 需要核实。
- `keyboard_server.c` 的 `config_dirty`/5 秒后台任务会编码 `active.nkc` 的运行态快照；已存在独立的基础、灯效、宏文件，但 v5 基础快照仍可内联宏，`config_select` 使用 24 位 FNV，灯效 ID 依赖注册顺序。
- `nos_fsync` 与 littlefs 同卷覆盖 rename 已有；旧落盘代码仍在 `EEXIST` 后删目标重试。NK60 数据挂载底层已有 W25Q wear leveling，littlefs `block_cycles=500`。LittleFS 挂载失败后的自动格式化策略需核查限制。
- Web 现有 ANSI 61 硬编码、物理预览和 key/action/light 控件直接调用旧 setter。原生 C++ CLI 在独立仓，Studio 含自有 Rust 协议实现和 TypeScript；三者都需协议向量。
- NK60 V1/V2 是 240 KiB app Flash / 64 KiB RAM，已有本地产物的粗略 Flash 余量约 2.3 / 3.6 KiB；现有 NLK 工作区生成配置为 256 B payload，VFS 单个路径分量仅 31 B。
- 选择新配置 capability + 小型类型化记录 + 单入口事务。备选为扩展旧 op 或通用整机 JSON 镜像；前者语义冲突，后者不适合 NK60 资源约束。
- 用户明确两条新约束：不提供旧协议、旧文件兼容；每个固件由 Kconfig 选择并只编译一种配列。配列 READ 为单例、无 setter；客户端依同一契约发布，旧文件不迁移或自动删除。
