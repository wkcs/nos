# NK60 Configuration and NLK Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development for isolated components or superpowers:executing-plans for integration. Steps use checkbox syntax.

**Goal:** Give NK60 V1 and V2 named base, light and macro objects, RAM editing, durable activation and discoverable definitions in the CLI and Studio.

**Architecture:** Preserve the HID and LED pipelines, separate board definitions from codecs and configuration storage, and expose transactional editing inside the unified NLK KEYBOARD 0x0100 v2.0. Publish object and catalog versions through one atomic config.nkc replacement. The web client and both CLI runtimes consume the same byte contract.

**Tech Stack:** NOS C, Kconfig/CMake, LittleFS/VFS, Python host regression tools, C++ native NLK, Rust desktop NLK, TypeScript/React Studio.

- [x] Record baseline builds and flash usage. Run `cmake --preset nk60-v1`, `cmake --preset nk60-v2`, the corresponding builds, and `python3 tests/run_host_regressions.py`.
- [x] Extract the NK60 layout and RGB definitions from both board directories. Test geometry and selected board definitions with the existing LED fixture, plus registry checks for key-to-matrix/LED identity and duplicate IDs. Make the board Kconfig choice link exactly one layout.
- [x] Implement `app/server/keyboard/keyboard_object_store.c` and its internal header. Exercise initial root creation, load/list, versioned save, delete, root-only activation and failure before/after root replacement. Assert that failed save does not publish an orphan. Add VFS persistence checks and managed path protection.
- [x] Add the shared NLK keyboard configuration wire contract, packet-size fixtures, and the new capability. Test paged definitions, malformed input and a device edit session end-to-end through the dispatcher.
- [x] Integrate active base/light/macro codecs, bindings, draft application, preview and filesystem service into the existing keyboard server. Exercise preview while a selection write is pending, reject stale edits, verify save-as activation and test macro/HID release and switch debounce.
- [x] Implement shared transport semantics in standalone C++ CLI, Rust NLK core and TypeScript. Test capability discovery, state and paged reads, draft conflicts, explicit save and nonce retries with fixture transports.
- [x] Connect Studio's keymap, lighting, macros and navigation to a shared editor session. Cover overwrite/save-as, leaving with unsaved edits, lamp preview, registered RGB controls and layout-driven rendering.
- [x] Update the macOS preset producer and installation path to the new object import. Verify host regressions, both NK60 builds, the other board presets, QEMU smoke, C++/Rust/Web tests, build size, and record remaining physical verification.

Implementation limits and physical validation follow-up are recorded in `docs/NK60_CONFIGURATION_DESIGN.md` and `docs/NK60_CONFIGURATION_VERIFICATION.md`. See the latter for the pre-existing NUC472 footprint threshold failure and final build sizes.
