# NK60 standalone NLK Bootloader

Bootloader occupies STM32F401 Sector 0 (`0x08000000`, 16 KiB). It exposes NLK v1 over USB WinUSB (`0x0eff:0x0001`, interface 0, Bulk OUT `0x01`, IN `0x81`). Normal firmware uses a different composite USB interface, so hosts must rediscover endpoints and capability IDs after reboot.

The fixed primary node (`0x0001`, local alias `0xfffe`) implements Core device description, capability enumeration/description, topology, ping, session reset and reboot, plus Flash query/prepare/write/commit/abort. RPC and Bulk responses preserve sequence, capability, operation and mode, and report signed 16-bit errno on failure. Bounded ordered L3 fragments are reassembled up to 500 payload bytes. Unsupported operations/capabilities return protocol errors; unsupported versions and malformed headers are discarded.

Flash discovery reports `run_mode=BOOT_BOOTLOADER`, a read-only `bootloader` partition and writable `kernel` partition (240 KiB, 256-byte chunks). Chunks must be ordered and word aligned; only the last chunk may have a partial word. The first chunk must contain the Cortex-M vectors. Identical acknowledged chunks and a successful COMMIT can be retried after lost ACKs. COMMIT validates partition, size and CRC32, reads Flash back, then publishes the saved MSP word. Reset and abort invalidate the session.

USB IRQ callbacks only assemble requests and complete transfers. The main loop executes Flash operations and serializes replies; full-size IN responses terminate with a ZLP. A reboot executes after the reply finishes and stores the standard boot-flag magic. A USB reset invalidates pending responses from the previous USB session.

With the matching NLK CLI, a directly connected Bootloader supports:

```text
nlk devices
nlk info
nlk flash kernel nos.bin --no-reboot
nlk reboot normal
nlk reboot bootloader
```

Web connects through its normal device selector and opens firmware update for Flash-only nodes. It reports reconnect, erase, acknowledged write progress, CRC verification and reboot separately. Bootloader now uses serial `320219198301`, matching the deployed NK60 composite runtime, so browser WebUSB grants survive mode changes. Legacy images used `32021919830108` or `00000001`; host alias matching can identify already-authorized legacy devices but cannot grant browser permission to a different serial. Web shows an explicit reauthorization entry when needed and requires an unambiguous candidate. Multiple keyboards sharing these legacy identities require disconnecting the other devices during a mode transition.

Build without installing firmware:

```sh
cmake --preset nk60-v2
cmake --build --preset nk60-v2
```

Artifacts are in `build/nk60-v2/bootloader.{bin,hex,elf}`. Updating the deployed Bootloader requires installing this artifact separately; a Web-only update cannot change the Bootloader already stored on the keyboard.

Native regression tests use actual C engine and USB class sources with simulated Flash/USB. The optional interoperability test compiles the actual NLK C++ client against that engine, including lost write/commit ACKs. POSIX hosts are supported; macOS uses x86_64 low-address Flash emulation (Rosetta is required on Apple Silicon).

```sh
cmake -S tests -B build/host-tests -G Ninja -DNLK_CLI_SOURCE_DIR=/path/to/nlk/cli
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

Host tests do not validate electrical USB timing or replace a physical keyboard flash/reboot test.
