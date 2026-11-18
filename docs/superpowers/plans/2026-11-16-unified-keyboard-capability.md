# Unified Keyboard Capability and Startup Recovery

User confirmed one KEYBOARD capability, no separate keyboard-config capability.

- [x] Attach the transaction worker RPC callback to the keyboard engine capability0x0100, promote to version0x20, preserve EVENT/STREAM and core hardware descriptor, remove second capability registration. Test exactly one registered capability through actual engine RPC dispatch.
- [x] Decouple compiled-definition queries from persistent volume readiness. Add STORAGE_STATUS15 (state0starting,1ready,2unavailable,3readonly; signed i16errno), retain the real startup/mount error, expose diagnostics while unavailable, and keep persistence gated on the real mounted data volume. Cover startup/error/recovery and readonly loading.
- [x] Unify Web route/sidebar and load readiness before opening edit leases. Reject old capability versions before opcode1; support explicit retry and safe automatic startup recovery.
- [x] Update both C++ and Rust SDK/CLI discovery/constants/diagnostics/tests to0x0100v2.0; no alias or second advertised capability.
- [x] Verify host regressions, NK60 builds, client suites and boundary checks, document remaining physical-device validation.

Final: full host regressions passed; five preset builds and QEMU smoke passed; client suites58/186/41 passed. No hardware flash or destructive data-volume action. Actual on-device storage remains unverified until matching2.0firmware is installed; startup errors are now observable rather than hidden by ENODEV.
