# Keyboard Memory Budget Fix

User reports storage startup and definition reads returning ENOMEM (-12).

- [x] Reproduce using the real configuration worker with an8KiB residual heap and4KiB single-allocation ceiling; keep the protocol body limit8192 unchanged.
- [x] Add bounded size-only encoding and allocate defaults/definitions by exact size, eliminating speculative8KiB buffers.
- [x] Keep drafts at actual length and grow only for validated PATCH demand; preserve old draft/lease/revision when allocation fails.
- [x] Cache only compact action bindings at runtime and release full base documents after apply/commit.
- [x] Verify low-memory boot, schema reads during edits, save/reload, fault recovery and allocator cleanup; run board builds and host regressions. Record measured peaks and physical validation limits.

Validated under ASan/UBSan with a measured6805-byte configuration heap peak, zero tracked allocations left at teardown, passing full host regressions and five board builds. No changes to client protocol or filesystem document format; no hardware flashing performed.


2026-11-17 physical follow-up: removed production legacy metadata and duplicated effect strings; reclaim unused managed VFS cache with borrowed-reader guards; budgeted V1 scan/NLK stacks from measured high-water marks. Actual USB edits, schema reads during drafts, light preview/save, delay-only macro, reset recovery and seven-switch coalescing passed. Host configuration peak is now4542B under a6144B residual budget. Detailed limits and hardware results are in NK60_CONFIGURATION_VERIFICATION.md; earlier no-flash statements above describe the previous phase.
