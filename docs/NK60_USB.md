# NK60 v2 USB keyboard service

The current NK60 v2 profile runs its USB device protocol and STM32F401 DWC2 FS
controller driver in the trusted user device service. It uses no legacy RTOS,
STM32 HAL, kernel HID stack, allocator, or interrupt handler. Register access
passes through the board IO callbacks; NOS validates the device capability and
MMIO whitelist. The service polls controller events and uses the board's
microsecond timer. The initial board clock must provide SYSCLK84 MHz and USB48
MHz from HSE8 MHz. `nk60_usb_init()` enables GPIOA and OTGFS clocks, disconnects
PA11/12 briefly, and configures both pins as AF10 push-pull/high-speed/no-pull.
VBUS sensing is disabled because PA9 is the board UART TX pin.

## Host contract

The device retains the historical NOS development VID/PID `0eff:0001`.
This preserves the repository's identity; it does not establish a USB-IF vendor
allocation. Device release is 2.00. It exposes one boot-capable HID interface,
one interrupt IN endpoint `0x81` (64 bytes, 1ms interval), and EP0 (64 bytes).
NLP adds vendor interface1 (class FF/subclass4E/protocol1), bulk OUT02 and IN82
(64 bytes each); the composite configuration descriptor is57 bytes. If NLP boot
identity cannot be initialized, enumeration uses the original HID-only descriptor.
The configuration is bus-powered, declares 500mA, and advertises host-controlled
remote wake. The service keeps RGB off before configuration and during suspend,
and limits configured brightness separately (see the board service policy).

A single HID interface prevents reporting the same physical key through two
independent keyboards. `SET_PROTOCOL(0)` selects boot protocol: an eight-byte
6KRO keyboard packet with no report ID, and no consumer packets.
`SET_PROTOCOL(1)` selects report protocol (also the reset default):

| Report ID | Payload | Direction |
| --- | --- | --- |
| 1 | Modifier byte + 256 usage bits (33 bytes) | Input: NKRO |
| 1 | Num/Caps/Scroll/Compose/Kana LEDs, padded to one byte | Output |
| 2 | Little-endian 16-bit consumer usage, range 0..1023 | Input: media |
| 3 | Standard eight-byte 6KRO packet | Input: optional 6KRO mode |

Report protocol packets include their leading report ID. The service selects
ID1 or ID3, never both for the same state. `usb_keyboard_set_nkro()` queues a
release for the previous ID before the current state under the new ID; its
`-EAGAIN` means retry later. Modifier usages E0..E7 appear only in the modifier
byte; their NKRO bitmap bits stay zero. Usage0 bits are not set by the engine.

`SET_REPORT(Output, ID1)` uses the two-byte report `[1, leds]`. Boot LED output
uses ID0 and one byte. `GET_REPORT` returns current input state or LED state.
Unsupported IDs, features, interface numbers, lengths and alternate settings other than0
STALL EP0. A fresh SETUP cancels a previous control transfer and clears EP0
STALL. Standard descriptors, address/configuration, status, remote wake,
endpoint halt, and alternate setting0 are implemented. Address changes commit
only after the status IN completion. LED/protocol/configuration changes are
also staged until that acknowledgement. HID idle is stored for each input ID;
SET_IDLE ID0 applies to every report, including media. Idle duration units are
4ms and zero disables repeats. Only the active keyboard report ID is repeated.

## Queues, suspend and bounded hardware access

`usb_keyboard_submit()` atomically accepts a keyboard/consumer snapshot.
Queued press/release packets are retained in order until IN completion. A full
queue returns `-EAGAIN`, and the keyboard service retains its source snapshot.
The queue never overwrites an in-flight packet or silently drops a release.
GET_REPORT exposes the latest accepted logical state. Offline snapshots update
that state for the next configuration; configuration sends a synchronization
snapshot. Service policy releases and inhibits keys across configuration and
suspend transitions until the physical switches are all up.

Suspend prevents transmitting queued interrupt packets. Remote wake requires
an explicit service request, a configured device, host SET_FEATURE opt-in,
and at least5ms suspend. There is no unsolicited remote wake. The hardware
adapter asserts resume for2ms using `delay_us` and clears it before returning,
so a delayed caller cannot hold resume beyond the USB15ms limit. The portable
protocol layer also clears its wake state after2ms or on resume/reset.

The DWC2 driver allocates128 words of RX FIFO plus64 words each for EP0/EP1/EP2 TX,
exactly the controller's320-word budget. It drains RX word padding, bounds all
copies, handles SETUP and OUT completion independently, clears stale IN
completion on replacement SETUP, and applies DATA0 when an endpoint halt is
cleared. FIFO/reset/disable waits have both10ms elapsed-time and100000-iteration
bounds. Controller failures disconnect and disable interrupts; initialization
returns a negative errno. The service must continue polling promptly while
configured; it must not perform blocking flash operations in that state.

## Automated checks and remaining physical acceptance

Run `python3 tests/host_usb_keyboard.py`. The test compiles the actual portable
protocol and actual DWC2 driver with address/undefined-behavior sanitizers.
LeakSanitizer is enabled; in a traced cloud sandbox the runner must execute
outside the traced wrapper. These modules do not allocate memory. Checks cover descriptor walks and HID
bit-layout parsing, multi-packet control IN, descriptor truncation, replacement
SETUP, address deferral, STALL recovery, LED control OUT and short packets,
boot/report selection, ID switching releases, queue saturation/release order,
endpoint halt, per-report idle including media, reset/unconfigure,
host-controlled wake and wraparound timing, plus10000 malformed SETUP sequences.
The controller model implements W1C interrupts, RX status/FIFO consumption,
separate TX FIFOs, endpoint enables and completions, GPIO/clock writes,
invalid oversized RX drains and bounded reset timeout.

No physical NK60 v2 is accessible in this task. The register model proves
software behavior under its modeled controller events; it does not establish
USB electrical timing, host interoperability, board component identity,
clock accuracy, suspend current, or wake behavior on real silicon. Physical
acceptance still requires Linux/Windows/macOS enumeration and descriptor
inspection, BIOS boot mode, >6 simultaneous keys, media release, Caps/Num LED
output, repeated plug/unplug, endpoint/host stalls, suspend/resume with host wake
enabled and disabled, and measured configured/suspend current with RGB load.

The NLP vendor interface uses `u16le_length | PDU`, independent endpoint busy
flags and bounded OUT backpressure. Interface/endpoint resets invalidate NLP
sessions and pending bytes without resetting the HID report protocol. The model
tests cover composite descriptors, EP2 traffic, backpressure, alternate resets
and HID independence. No physical host enumeration is claimed; see
[NLP implementation](protocols/nlp/implementation.md).
