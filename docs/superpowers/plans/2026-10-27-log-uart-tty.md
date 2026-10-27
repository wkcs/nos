# NOS Log, UART and TTY Implementation Plan

**Goal:** Implement the reviewed, configurable logging and terminal design for 20 KiB STM32 targets and larger systems.

**Architecture:** Keep kernel logging, console/TTY policy, and UART transport separate. Start with bounded, reentrant formatting and explicit errors, then migrate the STM32 UART to a single owner with configurable buffers and build an optional terminal layer. Larger-platform functionality follows the same interfaces and depends on the existing ARM64 bring-up.

**Tech stack:** C11, CMake, Kconfig, Python host regression probes, ARM GCC and QEMU.

**Existing constraints:** Preserve `docs/LOG_UART_TTY_REVIEW.md` and `.planning/` as user-visible review artifacts. Do not change the fixed UAPI message layout. The user approved the review's staged design and requested implementation in this session.

## Tasks

1. **Formatter:** Add host regressions in `tests/host/`, demonstrate current buffer and integer failures, and correct `libs/vsprintf.c` without losing existing format support. Verify zero- and one-byte destinations, truncation, widths, 64-bit signed/unsigned integers, pointer values and long strings.
2. **Logging safety and memory:** Exercise `kernel/printk/printk.c` and `kernel/printk/log.c` with host probes. Replace shared/unbounded formatting, fix severity filtering and return semantics, make buffer sizes explicit in `Kconfig`, remove unused FIFO storage, and preserve early/panic output. Verify reentrant output and real board map sizes.
3. **UART and terminal:** Exercise byte-stream error and queue behavior before changes. Consolidate STM32 UART ownership in `drivers/serial/uart_stm32.c`, use bounded TX/RX queues and IRQ-safe notification, adapt board consoles and libc, and make the minimum terminal selectable without a full Shell. Preserve polling for early and panic operation.
4. **Larger platforms:** Implement transport-independent console/TTY hooks and PL011 support where the existing ARM64 build infrastructure permits. Fix the caller-owned IRQ state prerequisite before claiming SMP safety. Verify ARM64 with the available cross toolchain; report external toolchain or incomplete platform prerequisites precisely.
5. **Integration:** Configure and build all four STM32 presets, run meaningful host regressions and QEMU boot/interactive smoke checks, measure the WL-001 RAM budget, and document supported configurations and any outstanding hardware validation.

Use a red/green cycle for each behavior and record commands, expected failures, decisions, and results in `.planning/2026-10-27-log-uart-tty-review/progress.md`. Do not create commits unless requested.
