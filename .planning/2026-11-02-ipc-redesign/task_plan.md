# NOS IPC redesign — superpowers workflow

## Goal
Audit message queues, semaphores, synchronous endpoint IPC and notifications;
design predictable MCU behavior and a correct SMP concurrency contract.

## Tasks
1. [complete] Explore current code, architecture, call sites, recent sync/timer work and tests.
2. [complete] Clarify compatibility and success criteria one question at a time.
3. [complete] Compare 2–3 approaches using primary OS documentation/source.
4. [complete] User approved option A and the three-stage order; detailed wait-foundation specification is being written.
5. [complete] First-stage spec and audit saved in the documentation changeset.
6. [complete] Spec self-review: concrete lock order, timer tags/references, wake acknowledgement, legacy coexistence and mutex handoff clarified; local links and placeholders checked.
7. [in_progress] Await written-spec review of docs/superpowers/specs/2026-11-02-ipc-wait-foundation-design.md before implementation planning.
8. [pending] Invoke writing-plans and create staged implementation plans.

Visual companion: not needed for initial semantic and concurrency questions.
Implementation is gated by the explicitly requested brainstorming workflow.

## Decisions
- No runtime code changes during initial audit/design.
- Keep the fixed 64-byte IPC UAPI invariant.
- User permits API changes and migration of every caller.
- User selected IPC concurrency correctness plus host multithread verification;
  actual multicore QEMU boot/runtime verification is a later project.
- Reviewable audit and options: docs/superpowers/reviews/2026-11-02-ipc-audit.md.
- Recommend A: shared wait/lifetime foundation with separate semantic objects.
- Decompose into wait foundation, local primitives/callers, endpoint/capability
  transactions, with a spec/plan/implementation cycle for each.
- User approved the architectural recommendation on 2026-11-02 (reply: 同意).
- First-stage spec includes a dedicated managed deadline timer per task, tagged
  immutable timer events, referenced legacy compatibility wake tickets and
  source-runqueue revalidation before considering cross-CPU placement.
- No runtime source changed during design; baseline tests are from the audit,
  not fresh verification of an implementation.

## Errors
- Read attempt for arch/arm64/kernel/context.S found no such file; discover
  architecture context/switch paths with rg --files instead of guessing paths.
