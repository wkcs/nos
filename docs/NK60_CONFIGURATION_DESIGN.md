# NK60 configuration system: proposed design

Status: the version 1 named-object codec, filesystem store, NLK file operations,
and non-blocking timed macro engine are implemented alongside the legacy
four-slot protocol.

## References and existing constraints

QMK represents keymaps as layers of hardware-layout-specific keys. VIA exposes
layout identifiers, switch-matrix coordinates, macros, lighting and save/load
to a graphical client. See [QMK keymaps](https://docs.qmk.fm/keymap),
[VIA layouts](https://caniusevia.com/docs/layouts/) and
[VIA custom UI](https://www.caniusevia.com/docs/custom_ui/).

NK60-V2 has 61 physical keys and three default layers. `/assets` is the
external-flash FAT filesystem when its board mount succeeds. The root filesystem
can instead be RAMFS. A successful configuration save must not be reported as
persistent unless the configured persistent filesystem is actually mounted.
The current NK60 LED driver drives 61 key LEDs; the capability advertises one
key-light zone, rather than unimplemented underglow and dedicated indicators.
NK60 uses size-optimized compilation to leave room within its 240 KiB
internal-flash link limit without removing its shell and protocol services.

## Implemented interface and current limits

`NLK_KB_OP_CONFIG_FILES_INFO` reports the hardware identity, runtime limits and
the independently selected keyboard and lighting object IDs. `LIST` returns one
entry per request and a 32-bit cursor; continue until `eof` is set. `SAVE`,
`LOAD` and `DELETE` use `nlk_kb_config_file_req`; `SAVE` creates a new object
and returns `EEXIST` for an existing ID. IDs contain 1–23 ASCII letters, digits,
underscores or hyphens; display names are separate NUL-terminated UTF-8 byte
strings of at most 31 bytes. Loading a macro copies it into `macro_id`; keyboard
profiles embed the current macro contents. To bind a macro slot, set the keycode
to `NLK_KB_MACRO_KEYCODE_BASE + slot` and bind the `macro_play` action. The
legacy `macro_play` binding with another keycode continues to select slot zero.

Named objects are stored in three subdirectories under `/assets/keyboard`:
`/assets/keyboard/config/<id>.nkc`, `/assets/keyboard/light/<id>.nkc` and
`/assets/keyboard/macro/<id>.nkc`. No default named objects are pre-generated
on boot. The currently active configuration (including the active keyboard and
lighting object IDs, keyboard state and lighting state) is stored in
`/assets/keyboard/active.nkc`, restored on boot and updated automatically when
the user modifies runtime configuration or saves/loads a named profile. The
NK60 backend checks that `/assets` is the expected external-flash FAT mount;
it does not use RAMFS as a substitute. The active named object cannot be
deleted until another one is selected. Failed `active.nkc` updates during
loading restore the previous runtime configuration. Storage is subject to
filesystem and available-space limits, rather than a fixed count of saved
profiles.

The version 1 header and fixed-width fields use little-endian layout. Files
carry model/revision/layout identity, a name, dimensions, length and CRC-32.
Key entries contain a keycode and only the action names actually bound; macros
contain only their actual steps. Lighting effects are stored by name and mapped
to the receiver's local IDs. A file is validated before it is applied.

Macros execute asynchronously on a 1 ms tick state machine without blocking
matrix scanning or lighting rendering. They support `KEY_DOWN`, `KEY_UP`,
`KEY_TAP`, `DELAY_MS`, modifier chords, repeat counts, real elapsed delay
recording (splitting delays above 65535 ms into `DELAY_MS` steps and setting
`NLK_KB_MACRO_FLAG_RECORD_OVERFLOW` on step exhaustion), `NLK_KB_OP_MACRO_CONTROL`
(`PLAY`/`STOP`), `NLK_KB_OP_MACRO_STATUS`, and pressing a macro key again to
cancel an active playback.

Layout geometry rendering is handled by the Web client based on the layout ID
reported by the keyboard (`NLK_KB_OP_GET_LAYOUT_LIST` / `active_layout_id`), so
firmware does not store visual keycap geometry tables. Legacy slot files
(`profile_0..3.bin`, `active_slot.bin`) remain manual to clean up or archive.

## Objects and identity

There are three independently named file types: keyboard profiles, lighting
profiles and macros. Each contains a version, type, display name, stable object
identifier, hardware compatibility identifier, length and checksum. The
compatibility identifier identifies the keyboard model, hardware revision and
physical layout schema, not an individual unit: compatible NK60-V2 keyboards
can share files. A different revision or layout must be explicitly declared
compatible; a matching key count alone is insufficient. IDs and display names
are distinct, so names can be changed without breaking references.

Keyboard profiles contain parameters, selected physical layout, default layer,
per-layer key mappings and action bindings, as well as the macro content needed
by those bindings. A macro may also be exported as an independent named file
and imported into another keyboard profile. Import copies the macro content;
editing or deleting the independent file cannot silently change an existing
profile. Lighting profiles contain global and per-zone settings and refer to
device effect names, not registration-order-dependent numeric IDs. Each object
is validated completely before any live state changes. Unknown versions,
unavailable actions or effects, malformed lengths, checksum failures and
incompatible hardware are errors, not reasons to partially apply a file.

File payloads contain only actual keys, layers, populated bindings, zones and
macro steps. They do not embed `NLK_KB_MAX_KEYS` or a fixed number of profile
slots. Object counts are bounded by available filesystem space and filesystem
limits, not an in-memory table. Separate resource limits, such as the number of
simultaneously active macros or layers, must be discoverable by the client and
must not be confused with the number of saved files.

## NLK and graphical clients

Retain legacy slot commands for old clients and add versioned keyboard capability
operations to discover supported file operations, hardware compatibility and
resource limits; enumerate objects by type with a continuation cursor; inspect
their IDs, names, versions and sizes; load, delete and save the current keyboard
or lighting state as a new named object; and import/export individual macros.
The file count must not be represented by an eight-bit mask. The client can use
NLK VFS for chunked file transfer; the keyboard capability must provide the
type-aware listing and validation rather than requiring a GUI to interpret an
arbitrary filesystem directory.

Expose the layout identifier, logical key indices, matrix dimensions,
available actions, layers, lighting zones and effect parameters so a Web client
can construct the controls without NK60-specific constants. A successful edit
updates the current runtime state; saving as a new object is an explicit action.
The protocol must distinguish an unsaved working state from the most recently
loaded or saved object. Large replies and objects require bounded chunks and
pagination; transfer state must be scoped to a connection and cleaned up when
the client disconnects. Browser transport support, including USB descriptors
and platform permissions, needs an end-to-end WebUSB compatibility test rather
than assuming that an existing WinUSB interface is sufficient.

## Persistence and failure handling

Write and validate a new temporary object before making it visible in listings;
do not truncate an existing valid profile while saving. Preserve the previous
active object and working state if the filesystem fills, a transfer is cut
short, validation fails or a commit fails. Recovery after interruption must be
tested against the actual FAT and flash implementation; do not assume rename is
power-loss atomic. The active keyboard and lighting selections are independent
and their updates must survive reboot only after the corresponding objects are
durable. If persistent storage is unavailable, return a meaningful error rather
than silently selecting RAMFS.

Provide explicit import of existing version 2 slot files. Their mixed lighting
and keyboard data must be split during migration, and their dimensions and
hardware compatibility checked before use. Existing slot files do not contain
macro data or a hardware identity, so migration cannot reconstruct missing
macros or prove their original hardware provenance; it must report those limits
and leave the source file untouched.

## Verification required before release

Host tests should cover round trips for all three object types, name and ID
handling, more than four files, pagination, cross-profile macro import, exact
hardware-sized data, invalid dimensions and incompatible identities, corrupt or
truncated inputs, insufficient flash, interrupted saves and rollback. Exercise
the same operations through NLK including multiple chunks and reconnects. Test
boot selection, storage unavailability and power interruption on NK60 hardware,
then check all board builds, internal-flash headroom and a real browser client.
