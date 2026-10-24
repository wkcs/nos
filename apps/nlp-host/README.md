# NLP host tools

<!-- Copyright (C) 2026 NOS contributors -->

Run commands from the NOS repository root. C requires a native C compiler,
OpenSSL development libraries and Linux libutil/PTY. Python 3.10+ and
`cryptography>=40` are required for authenticated mode (the regression runner
uses Python 3.11+). PyUSB/libusb is optional for USB; UART uses POSIX termios.
Dependencies are host tools, not firmware libraries.

```sh
make nlp-tools
python3 scripts/nlp_keygen.py --private /tmp/host.key --public /tmp/host.pub
python3 scripts/nlp_keygen.py --private /tmp/device.key --public /tmp/device.pub
out/microkernel/nlp-host/nlp-server --secure-loopback 38447 /tmp/device.key /tmp/host.pub
# Another terminal: explicitly pin the public key obtained from the device owner.
python3 scripts/nlp_host.py --tcp 127.0.0.1:38447 --key /tmp/host.key --peer-pin /tmp/device.pub caps
python3 scripts/nlp_host.py --tcp 127.0.0.1:38447 --key /tmp/host.key --peer-pin /tmp/device.pub topology
```

Keygen uses OS entropy, creates new raw32-byte files exclusively (private0600),
prints public identity/fingerprint only, and refuses to overwrite existing keys.
Keep private files outside the repository. Pair by checking public fingerprints
out of band. The server is a single-client loopback fixture with keyboard/log
capabilities; it is not a deployed network daemon or multiport coordinator.
`--physical-loopback PORT` explicitly selects the unencrypted test profile.

To exercise the actual host shell backend, restart the secure fixture with
`--shell` after the key and pin arguments, then use:

```sh
python3 scripts/nlp_host.py --tcp 127.0.0.1:38447 --key /tmp/host.key --peer-pin /tmp/device.pub shell --exec "printf 'hello\n'; exit 0"
```

This explicitly authorizes the pinned fixture client to start a local `/bin/sh`
PTY with the server user's privileges. Disconnect closes the backend and kills
its process group. CLI returns the remote exit status. NOS-native nshell is a
separate platform backend; NK60 does not advertise shell.

NK60 preserves USB HID interface0 and exposes NLP on vendor interface1/EP02/82.
The client claims only the NLP interface; OS/libusb access permissions must
permit it. These commands are ready for first-board acceptance, but were not
run on a physical keyboard in this task:

```sh
python3 scripts/nlp_host.py --usb 0eff:0001 --physical caps
python3 scripts/nlp_host.py --usb 0eff:0001 --physical --identity /tmp/nlp-host.json state
python3 scripts/nlp_host.py --usb 0eff:0001 --physical --identity /tmp/nlp-host.json monitor --count 10
python3 scripts/nlp_host.py --usb 0eff:0001 --physical --identity /tmp/nlp-host.json logs --count 10
python3 scripts/nlp_host.py --usb 0eff:0001 --physical --identity /tmp/nlp-host.json config-get --file /tmp/nk60.config
```

The configuration is the existing canonical1056-byte keyboard blob, not JSON.
`config-put --file FILE --save` validates SHA256, generation and semantics,
then reports APPLIED_VOLATILE/SAVE_PENDING/PERSISTED without equating an accepted
save request to durable storage. CONFIG_WRITE requires a physical Esc+PN+Fn
unlock bound to the currently connected host UID/boot. Use `config-put --file FILE --save --wait-unlock 60` to keep one connection
alive while waiting for the physical chord, then upload on that same connection.
Unlock before running a separate CLI invocation cannot authorize that new boot.
The model tests cover the actual peer-bound service flow. Never grant the next
unknown peer an old unlock.

`--uart /dev/tty... --physical` supports dedicated NLP UART devices at115200;
NK60's existing UART is maintenance text and cannot accept these frames.
No CLI automatically injects keyboard input into the host OS. Capability handlers
for firmware/display/radio are available in the C SDK; this CLI does not provide
a firmware flasher, display renderer or radio provisioning UI.

Validation and exact platform limits: [NLP implementation](../../docs/protocols/nlp/implementation.md).

NK60 product commands `info`, `layout`, `storage`, `health` read the typed
keyboard_control capability. `lighting`, `lighting-set`, `lighting-policy`,
`leds-get`, `leds-put` configure persistent per-key RGB. `leds-put --file FILE`
requires exactly61 RGB byte triples and commits a complete frame atomically;
`--wait-unlock 60` keeps the same peer connected for the physical chord.
Examples, power limits and expansion boundaries are in
[NK60 capabilities](../../docs/NK60_CAPABILITIES.md).

Lighting minor2 adds `fx-info`, `fx-get`, `fx-put --file FILE --save --wait-unlock 60`,
and `fx-clear`. Offline validation uses `python3 scripts/nlp_lighting.py FILE`.
The bounded JSON compiler supports named zones/palettes, behavior triggers,
layer/modifier conditions, blend priority and overlapping animations. See
[lighting configuration](../../docs/NK60_LIGHTING.md) and the three ready-to-edit
[examples](../../examples/nk60/lighting).
