# NLK KEYBOARD 2.0

One capability, type `0x0100`, version `0x20`; hardware description uses CORE
GET_CAP_DESC. All integers are little endian. `str` is a u8 byte length followed
by UTF-8 bytes. `ref` is u8 kind, str name, u64 revision. Configuration names contain 1–27
UTF-8 bytes (valid UTF-8, no NUL/ASCII control chars, no `/\:*?"<>|`, cannot start/end with `.` or space, cannot start with `~`). Revisions are nonzero; UINT64_MAX requests
current revision. There are no separate configuration IDs or virtual/factory configurations.

Object kinds: 1 base, 2 light, 3 macro. Resources: 1 compiled layout, 2 compiled
action, 3 compiled effect, 4 saved base, 5 saved light, 6 saved macro. Sections
currently support only 0 (complete document). A record is u16 tag, u16 length,
then length bytes. READ can slice arbitrary bytes; PATCH splices whole records
at record boundaries. Negative i16 errno is carried by NLK error responses.

| Op | Request | Success response |
| --- | --- | --- |
| 01 INFO | empty | u64 boot, u16 rx_mtu, u16 features, u32 ttl_ms, u64 definition_generation, u16 object_limit, u16 catalog_capacity, u8 light_layer_capacity |
| 02 STATE | empty | u64 generation, u64 persisted_sequence, u64 catalog_revision, u64 base_epoch, u64 light_epoch, u8 flags, base ref, light ref |
| 03 LIST | u8 resource, u64 generation, u32 cursor, u16 max_bytes | u64 generation, u32 next_cursor, then entries: str name, u64 revision, u8 flags (currently 0) |
| 04 READ | u8 resource, str name (compiled ID for resources 1–3; empty permitted for compiled layout), u64 revision, u8 section, u32 offset, u16 max_bytes | u64 revision, u32 total, u32 offset, bytes |
| 05 EDIT_BEGIN | u8 kind, str source name, u64 source_revision, u64 expected_epoch | u64 lease, u64 draft_revision, u32 ttl_ms |
| 06 EDIT_PATCH | u64 lease, u64 expected_draft_revision, u16 offset, u16 remove_length, records | u64 draft_revision |
| 07 Reserved | — | — |
| 08 EDIT_COMMIT | u64 lease, u64 expected_draft_revision, u8 mode, str destination, u64 expected_dest_revision, u64 nonce | u64 operation |
| 09 EDIT_ABORT | u64 lease | empty |
| 0A EDIT_KEEPALIVE | u64 lease | u32 ttl_ms |
| 0B RUN | u8 verb, u64 nonce, optional verb arguments | u64 operation |
| 0C DELETE | u8 kind, str name, u64 expected_revision, u64 nonce | u64 operation |
| 0D RESULT | u64 operation | u8 state, i16 status, u64 generation, ref |
| 0E EDIT_READ | u64 lease, u64 expected_draft_revision, u8 section, u32 offset, u16 max_bytes | u64 draft_revision, u32 total, u32 offset, bytes |
| 0F STORAGE_STATUS | empty | u8 state, i16 storage_errno |

INFO feature bit0 indicates RAM editing. STATE flags are bit0 current RAM light
enable, bit1 pending aggregate persistence, bit2 write error, bit3 unavailable or
read-only storage, bit4 an edit has been applied to RAM, bit5 latest draft is
incomplete/invalid and is not applied. There is no primary-effect field.

LIST cursor0 starts, next_cursor UINT32_MAX ends; generation0 starts a snapshot.
Object resources list only actual catalog entries. Definition resources list
compiled descriptors. max_bytes limits the variable response portion; response
headers still count toward negotiated MTU.

STORAGE_STATUS: 0 starting, 1 ready, 2 unavailable, 3 read-only. INFO, STATE,
status and compiled definitions remain readable without usable storage. Other
requests preserve the underlying errno; read-only mutation returns EROFS.

## Documents

| Tag | Use | Value |
| --- | --- | --- |
| 0x0001 | metadata (exactly once) | str name (<=63 UTF-8 bytes), str description (<=127 bytes) |
| 0x0002 | layout definition | str layout ID, u16 version, u8 keymap layers, u8 rows, u8 cols |
| 0x0003 | layout definition | u16 key, u16 x, u16 y, u16 width, u16 height, u8 row, u8 col, u8 LED index |
| 0x0004 | descriptor field | u16 ID, u8 type, u16 minimum, u16 maximum, u16 default, str name |
| 0x0005 | base header (once) | str layout ID, u16 version, u8 default_layer, u8 debounce_ms, u16 poll_hz, u8 nkro, u8 win_lock, u8 fn_lock, u8 sleep_minutes |
| 0x0006 | base key | u8 layer, u16 key, u32 HID code, u8 binding_count, bindings |
| 0x0011 | light layer | u8 x, u8 y, u8 width, u8 height, i16 z, u8 opacity, u8 activation, str trigger action, effect style |
| 0x0021 | macro step | u8 operation, u8 HID code, u16 delay_ms |

Layout geometry uses 1/100 key-width units. Lighting rectangles use LED matrix
coordinates. There is no tag0x10 primary style. A light document has zero to
INFO.light_layer_capacity equal layers; order is z then document order. Opacity
is 0–255. Activation is 0 always (empty trigger), 1 toggle, 2 hold (known action
ID required). All layer configurations are independent OWN_CFG instances.
Empty scenes render black; the global light switch is separate. There is no
`off` effect.

Binding is str action ID, u16 argument byte count, argument bytes. Action-owned
field descriptors define the ordered encoding: type1 u8, type2 RGB, type3 base
reference str, type4 macro reference str. `config_select{target}` and
`macro_play{target}` retain complete target IDs. Up to four bindings per key.
A base document covers every physical key in every compiled keymap layer.
Layout-specific base records use tags0x1000..0x1fff; layout callbacks validate
and apply their payloads.

Effect style is str effect ID then fields, each u16 ID, u8 type, u8 byte count,
bytes. Common field IDs: 1 brightness(u8), 2 speed(u8), 3 color(RGB). Custom IDs
start at4. Only declared fields are valid; missing fields use firmware defaults.
Each instance has its own parameters and private rendering state.

Macro operations: 1 down, 2 up, 3 tap, 4 delay. Non-delay codes are 1..0xe7.
Limit32 steps, total delay600000ms. A PATCH installs valid macro steps in RAM
without playing or injecting keys; RUN starts playback explicitly.

## RAM editing, persistence, and errors

One lease is shared by all object kinds and belongs to source address, bearer,
and connection epoch. TTL is60000ms; clients renew at15s. Empty BEGIN source
uses the current saved base/light, or a new empty macro. Base/light epochs
reject conflicting physical profile selection. The global light switch leaves
the light epoch and any active edit unchanged.

A complete valid PATCH immediately updates runtime RAM and returns the next
revision. It does not write Flash, change the selected saved reference, or mark
aggregate persistence pending. Multipart transfers may temporarily produce an
incomplete document: retain bytes in the draft, keep last valid runtime and set
STATE bit5 until valid. Allocation failure rolls back draft bytes/revision and
runtime. Clients show validation feedback separately from uncertain transport.

ABORT, expiry and disconnect release the draft before reloading the selected
saved runtime, keeping the current global light switch state.

COMMIT validates the entire document and referenced objects, and synchronizes `metadata.name` to `destination`. mode0 overwrites
exactly expected_dest_revision; mode1 requires a new name and expected revision0.
Success writes `<kind>/<name>.nkc` atomically via `/data/keyboard/object.tmp`, then atomically publishes
`/data/keyboard/config.nkc`, and activates the saved reference. DELETE refuses
active or referenced objects. Managed records are mutated only through these
transactions; obsolete `/data/keyboard/active.nkc` is not protected by this
service and can be removed through normal VFS.

RUN verbs: 1 select_base(str name), 2 select_light(str name), 3 base_next,
4 base_prev, 5 light_next, 6 light_prev, 7 light_set_enabled(u8 enabled),
8 macro_play(str name), 9 macro_stop. Next/previous visit saved configurations
only. An empty macro target plays the current new macro draft; a matching source
name plays its edited RAM bytes. Physical/runtime RUN changes may mark aggregate
state pending. Verb 7 and the physical light switch update global status
immediately and defer its aggregate write. A profile COMMIT does not flush that
status early. The worker writes the final state after five seconds of inactivity,
with a 30-second limit during continuous changes and retry delays on write failure.

RESULT states: 0 queued, 1 runtime applied, 2 committed, 3 failed. RUN remains
applied; observe STATE pending/error for durability. COMMIT/DELETE finish
committed. Empty result ref uses kind0, empty name, revision0. RESULT requests
have a15s client timeout because this MCU executes synchronous Flash work and
GC on the serial NLK worker. Mutation requests are not replayed automatically.

Selection persistence coalesces after5s idle, at30s maximum, with5s minimum
spacing; write retries use5/15/60s delays. Explicit saves merge pending state.
Nonce deduplication and four result slots belong to one connection context.
Manual retries must use identical bytes and nonce. After reconnect/reboot,
read STATE instead of assuming an old operation token is valid.

Limits:8192-byte document,24 saved objects total, configuration name 27 UTF-8 bytes; lighting capacity
comes from Kconfig/INFO, encoded effect fields64 bytes per layer, renderer
private state32 bytes. These are logical limits; allocation failure remains a
reported error on resource-limited boards.
