## Refactoring design

当前架构、接口契约、迁移顺序和验收标准见 [NOS 单体内核重构设计（收口版）](docs/superpowers/specs/2026-10-01-nos-refactor-design.md)。设计完成与实现验收分别跟踪；当前构建限制见该文档第 2.1 节。

## Build (golden board)

```bash
make qemu-stm32_config
make
make qemu-run
```

`qemu-run` 需要包含定制 `nos_stm32` 机型的 QEMU，可用 `qemu-system-arm -machine help` 检查。

## Build (other boards)

```bash
make nk60-v2_config
make
```

NK60 已回迁，硬件运行需单独验证。`zj-v3`、`wl-001` 等未迁移板的状态见 [legacy 说明](legacy/README.md)。
