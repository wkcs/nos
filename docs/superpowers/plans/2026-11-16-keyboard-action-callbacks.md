# Unified Keyboard Action Callbacks Implementation Plan

**Goal:** Every compiled action owns its schema and execution callback in one source file, with no built-in/external classification.

**Architecture:** Keep the approved CMake-generated immutable action/effect registries. Replace action verb/argument_type metadata with shared parameter descriptors and mandatory execute callbacks. The configuration worker exposes a typed asynchronous runtime request interface; dispatch resolves a binding then calls execute, without selecting behavior by action name or argument type. Generic schema parsing checks arguments, discovers object dependencies, and emits NLK field descriptors.

- [x] Add failing registry/definition tests: every action has execute, schema replaces argument_type, press/release behavior and full target requests come from the callback; adding/removing an action/effect file updates generated arrays.
- [x] Update `keyboard_definitions.h`, action files, generic validation/dependency/schema/execution in `keyboard_config_service.c`, and focused service tests. Retain NLK byte grammar and deferred persistence behavior.
- [x] Explain configure-time source collection, generated arrays, board effect adapter, runtime action registration and display consumption with source/build paths.
- [x] Run definition/service/lighting and host regressions, both NK60 CMake presets, layer checks and whitespace checks. Record size deltas and keep existing unrelated NUC472 budget limitation visible. No Git commit or hardware flashing.

Verified: definition callback/registry expansion tests, ASan/UBSan service integration, complete host regressions, NK60 V1/V2 + QEMU STM32/WL-001/ZJ-V3 builds, QEMU smoke, layer boundaries and diff checks all passed. Existing staged changes were preserved. No Git commit or hardware flashing.
