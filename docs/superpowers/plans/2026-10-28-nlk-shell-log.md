# NLK Shell Sessions and Log Backends Implementation Plan

**Goal:** Give each shell independent state and route kernel logs to modular, independent backends.

**Architecture:** Shell instances bind to TTY endpoints and own editing and filesystem state; the NLK capability controls its session lifecycle. Log backends enqueue independent copies when records are published, while the existing console path retains early and panic support.

**Tech Stack:** C11, Python 3, CMake, host regression tests.

1. Write host regressions for log fanout when console is blocked and for independent registered backends. Run them red, add a bounded registration and dispatch API, and run them green in buffered and unbuffered configurations.
2. Migrate NLK log to a registered backend with independent queue and subscription filtering. Remove its consumption of the console queue. Exercise NLK tests with a blocked console and reconnect.
3. Write NLK capability tests for peer ownership, duplicate open, close/reopen and stale input; implement lifecycle and run tests.
4. Extract independent shell session state and endpoint interfaces, run two simultaneous sessions in host tests, then attach console and NLK. Ensure command output and working directory are per session.
5. Make host shell use terminal byte mode with cleanup and test terminal behavior and disconnects.
6. Run host regressions, Python unit tests, architecture checks, four board builds and QEMU smoke. Document unavailable checks and actual results.
