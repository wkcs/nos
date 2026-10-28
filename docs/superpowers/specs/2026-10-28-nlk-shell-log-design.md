# NLK shell sessions and log backends

Each shell session owns its command editing state, working directory and byte-stream endpoint. The local console and an NLK endpoint execute independent sessions. Commands use the invoking session's output and working directory. NLK v1 exposes one session per capability; reject a second open and invalidate queued input on close. This limitation belongs to the wire capability, not the shell abstraction.

The host terminal forwards raw bytes and restores terminal settings on exit. NLK session open, close, signals and input must be bound to the owning peer. Reopen begins with clean state. Noninteractive clients continue using the existing stream protocol.

Logging fans each record to independently registered, bounded backends. The existing console queue remains responsible for early boot and panic output; NLK maintains its own queue and never consumes the console queue. A slow backend must not stop another backend. Backend callbacks run in a restricted context and enqueue only; transports run later. Keep existing global severity controls and apply NLK subscription filters separately.

Test session isolation, lifecycle and peer ownership, host terminal cleanup, backend fanout, backpressure and both buffered and unbuffered logging. Run host regressions, board builds and QEMU smoke as available.
