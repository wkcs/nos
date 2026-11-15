# NK60 macOS 键盘配置设计

为 NK60-V1 和 NK60-V2 ANSI 61 键提供独立的 macOS 数据文件。键位、快捷键组合和动作绑定存放在各板型的 `configs/keyboard/<board>/macos.json`，构建时生成现有版本 5 格式的 `macos.nkc`。由 NLK VFS 上传至外置 Flash 的 `/data/keyboard/config/macos.nkc`，再使用现有配置加载接口激活。启动读取 `active.nkc`，方便设备记住上次选择。V1 和 V2 各自校验硬件标识。

底排从左至右为 Control、Option、Command、空格、右 Command、PN、右 Control、Fn。Fn+7/8/9/0/-/= 依次为上一曲、播放暂停、下一曲、静音、减音量和加音量；PN+1 至 PN+= 保留 F1 至 F12。Fn+P 发送 Command+Shift+3，Fn+[ 发送 Command+Shift+4，Fn+L 发送 Control+Command+Q。macOS 根据当前输入源解释字符。

固件实现独立的 Consumer Control 报告 ID 4，把媒体码映射到标准用途页。通用 `key_chord` 动作从配置读取 HID 修饰键位图。按住时组合键和物理修饰键共同参与报告，松开时只撤销对应按键的贡献。媒体键和组合键保留按下时的含义，先松 Fn 也能正确释放。USB 传输结束后复用缓冲区，拥塞时保留待发状态，重连后同步最新键盘和媒体状态。

NLK 动作容量为 24 项，原有六项查询保留固定响应格式，新增分页查询。`config_select` 与 `macro_play` 共用按键动作的目标参数。扫描任务记录短哈希，后台任务检查 Flash 中是否存在唯一匹配的配置 ID，再加载并持久化切换。成功切换时清除已按住的键，等释放后再接受下一次按下。

主机测试读取真实配置文件并验证媒体和快捷键报告、已按住键的释放、动作分页和持久化、哈希碰撞和 USB 状态。Mac 对 HID 的实机行为由设备测试验证。
