# Legacy content

This directory quarantines unmigrated boards, drivers, and servers that are
not part of the supported golden build.  Top-level builds scan `board/` only;
they must not discover or compile content under `legacy/`.

The supported golden target is `qemu-stm32`.  Board configuration targets for
quarantined boards (for example, `make zj-v3_config`) may no longer work until
the board is migrated back to `board/`.

## Return criteria

Before moving a legacy component back into the active tree, it must:

1. use the `DEVICE_DEFINE` registration path;
2. avoid leaking `board.h` into kernel code; and
3. build successfully under its board configuration.
