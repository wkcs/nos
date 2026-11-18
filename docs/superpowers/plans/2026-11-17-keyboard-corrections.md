# Keyboard configuration corrections

**Goal:** Address the ten requested corrections using one production implementation and real RAM editing.

**Architecture:** Retain normal VFS semantics and remove keyboard-specific cache eviction/read guards. Remove legacy protocol implementations rather than hiding them behind host-test macros. Lighting documents contain only equal layer records (tag 0x11); global enable belongs to aggregate runtime state. Successful valid draft PATCHes apply to RAM, never flash; abort/expiry restores saved runtime, save publishes the active draft. Invalid intermediate multipart drafts never replace the last valid runtime.

**Acceptance:** Five tabs (按键配置/灯光配置/宏配置/按键测试/配置档案), switch settings inside key configuration; automatic RAM application; no device-preview button; no off effect, primary effect, or virtual ~ archives; animated composed lighting visualization. Managed filesystem operations must report actionable protection/in-use reasons; ordinary deletes work. New/changed C and UI code uses readable multiline formatting.

- [x] Remove keyboard legacy host branches and migrate engine/discovery tests to production paths; keep host macros only for platform stubs.
- [x] Roll back cache-pruning/read-guard changes only; preserve independent VFS correctness fixes. Reproduce and fix deletion routing. Re-budget memory within keyboard/storage/application layers.
- [x] Define and implement layer-only documents and automatic valid-draft activation/rollback; remove virtual template resources/off effect; synchronize packer, C++/Rust SDKs and wire documentation.
- [x] Update five-tab UI/session, automatic writes and composed effect visualization with focused regression tests.
- [x] Run production-path host suite, clients, required board builds, QEMU, and package Studio.
- [x] Complete NK60 V1 USB/ST-Link validation. Converted the saved lighting, flashed and verified the production image, exercised RAM editing and abort, verified persistence after reset, and tested deletion at the obsolete path.

Existing staged and unstaged task edits are preserved; no commits or unrelated resets.
