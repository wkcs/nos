# Legacy content

This directory quarantines unmigrated boards, drivers, and servers that are
not part of the supported golden build.  Top-level builds scan `board/` only;
they must not discover or compile content under `legacy/`.

The supported golden target is `qemu-stm32` (`soc/stm32f1`).  `board/nk60`
(config target `nk60-v2_config`) is active again and uses `soc/stm32f4` for
SPL/CMSIS; the board tree keeps only glue, memory map, linker script, and
USB HAL (same split as qemu-stm32).  Other quarantined boards (for example,
`make zj-v3_config`) may not work until migrated.

Switching boards: `make <board>_config` deletes `out/` build products (`.o`,
images, generated `.ld`) so stale SoC objects are not linked.

## Return criteria

Before moving a legacy component back into the active tree, it must:

1. prefer the `DEVICE_DEFINE` registration path where practical;
2. avoid leaking `board.h` into kernel code; and
3. build successfully under its board configuration.
