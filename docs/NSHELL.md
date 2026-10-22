# nshell

<!-- Copyright (C) 2026 NOS contributors -->

```sh
make PLATFORM=arm64_virt BUILD_MODE=normal qemu-run
```

普通启动执行独立 `/bin/hello` 后进入 `nos> `。使用 `help` 查看当前构建的命令；`runlinux` 仅在 `LINUX_COMPAT=1` 时提供。

```text
mkdir /work
cd /work
pwd
write example "hello world" 'single quoted' escaped\ space
cat example
ls .
run /bin/hello "shell argument"
cd /
rm /work/example
rmdir /work
stats
```

支持 `help`、`pwd`、`cd`、`echo`、`history`、`ls`、`cat`、`write`、`mkdir`、`mv`、`rm`、`rmdir`、`mount`、`sync`、`run`、`stats`、`restart`。`cd` 不带参数回到根目录。`write` 创建或覆盖文件，文本参数之间插入空格，返回实际写入字节数；`cat` 输出内容后换行。`run` 等待程序结束并报告状态。

Tab 输入转为一个空格。单引号保留字面文本；双引号支持反斜杠转义；单引号外反斜杠转义下一个字符。可以传入空参数或拼接引号内外文本。未闭合引号、末尾反斜杠和参数过多会在执行前拒绝，避免截断后执行另一条命令。当前限制为一行 255 个可打印 ASCII 字节、命令及最多 8 个参数；程序装载还受对应构建的 ABI 参数长度限制。

| 按键 | 行编辑 |
|---|---|
| 左/右方向键 | 移动光标，当前位置插入 |
| Home / End、Ctrl-A / Ctrl-E | 行首/行尾 |
| Backspace / Delete、Ctrl-D | 删除光标前/光标位置字符 |
| Ctrl-U / Ctrl-K | 清空整行/删除光标至行尾 |
| 上/下方向键 | 浏览最近 4 条历史，向下可恢复未提交草稿 |
| Ctrl-C | 取消当前输入 |
| Enter | 执行；CRLF 只提交一次 |

输入超长后整行拒绝执行；Ctrl-U 可清空后重新输入。文件读写处理短写与错误。`restart` 会重启 VFS/UART，内存文件、打开的 FD 与工作目录按服务重启语义重置；持久化内容需要重新挂载。

当前没有管道、重定向、变量/通配符展开、后台任务或作业控制。Ctrl-C 只取消正在编辑的行。它是有界的系统维护 shell，不能据此宣称 POSIX shell 兼容。宿主消毒器覆盖解析与编辑，三主架构的 UART 交互测试覆盖实际命令、程序运行与重启。

RISC-V UART 的回归还覆盖输入在 IRQ 屏蔽期间到达的窗口：QEMU GDB stub 在真实重新使能路径暂停 CPU，注入串口数据并核对 PLIC pending，然后由 NOS 正常恢复中断并执行命令。RV64、RV32 MMU/非 MMU 都执行这一检查；QEMU 不需要补丁。
