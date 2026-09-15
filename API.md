# Library API

The authoritative assembly declarations are `fat32.h` and `buffer.h`.
`tests/api.h` is a C ABI mirror with layout assertions. All functions and callbacks
use the Windows x64 calling convention. Except the void `end` callback, EAX is a
status: zero is success. `fat_dir_next` returns `F_END` (1) at directory end.

All state and output buffers are caller-owned. Zero identities and adapters
before first use. Keep the operations table, its context, and an optional OEM
mapping alive as long as the identity. Serialize access to a buffer and its
identity; separate instances can run independently. Do not modify live identity,
cursor, or entry bookkeeping. All pointer arguments must address their declared
storage; transfer data must address `length` bytes and must not alias library
state. Component strings must be NUL-terminated UTF-16.

## Sector boundary

`SectorOps` contains:

| Field | Contract |
| --- | --- |
| `context` | Opaque pointer passed to callbacks |
| `read(context,lba,out)` | Read exactly one logical sector; return status |
| `write(context,lba,in)` | Copy a complete sector into the current transaction |
| `begin(context)` | Begin isolated staging; no changes on failure |
| `end(context,accept)` | Accept all staged writes, or restore all on zero; cannot fail |
| `flush(context)` | Backend durability callback; FAT32 never calls it |
| `sectors` | Available extent, in logical sectors |
| `sector_bytes` | 512, 1024, 2048, or 4096 |

Sector zero is the FAT32 boot sector. A physical-disk provider must select and
bound its partition before supplying this interface. The FAT32 library does not
add BPB hidden sectors. Direct read-only backends may leave `write/begin/end` null.
Mutations then return `F_READONLY`. `write` must copy its input before returning.
Callbacks must not reenter the same identity.

## Mount, volume, and directories

| Function | Arguments after identity | Result |
| --- | --- | --- |
| `fat_mount` | `SectorOps*, uint16_t *oem_or_null` | Validate and cache volume geometry |
| `fat_invalidate` | none | Drop FAT/directory caches and invalidate snapshots |
| `fat_get` | `uint32_t cluster, uint32_t *value` | Masked FAT entry, including reserved entries 0 and 1 |
| `fat_chain` | `uint32_t first, uint32_t count_last[2]` | Validate finite chain; return count and final cluster |
| `fat_dir_open` | `uint32_t first, FatCursor*` | Validate chain and initialize enumeration |
| `fat_dir_next` | `FatCursor*, FatEntry*` | Next file/directory, omitting volume-label and deleted records |
| `fat_lookup` | `uint32_t parent, uint16_t *component, FatEntry*` | Find by long name or short alias |

Use `identity.root_cluster` to begin. Lookup accepts a single component; a caller
resolves a path by repeated lookups and checking the directory attribute. Root
has no ordinary `FatEntry`. Dot/dot-dot records can be enumerated. A dot-dot
cluster of zero denotes the root; callers performing path resolution map it to
`root_cluster`.

`FatIdentity` stores parsed geometry, serial, the complete boot sector, a FAT
sector cache, a directory sector cache, and scratch space. It is 16,504 bytes.
All interpretation and cache management are in `fat32.lib`. A supplied OEM table
has 256 UTF-16 entries; null selects CP437. There is no Unicode normalization or
non-ASCII case folding. Damaged LFN sequences fall back to the preserved SFN.

## Files and metadata

`FatEntry` is 592 bytes and includes a generation, parent and exact SFN location,
first cluster, byte size, raw 32-byte entry, and a checked 255-unit UTF-16 name.
The raw entry exposes attributes, creation time/date/tenths, access date, write
time/date, NT case flags, and the OEM short alias without conversion losses.

| Function | Arguments after identity | Behavior |
| --- | --- | --- |
| `fat_read` | `FatEntry*, FatTransfer*` | Read an arbitrary byte range; clamp at EOF |
| `fat_write` | `FatEntry*, FatTransfer*` | Overwrite/extend; zero-fill any gap |
| `fat_resize` | `FatEntry*, uint64_t new_size` | Grow with zeroes or truncate/free trailing clusters |
| `fat_set_info` | `FatEntry*, FatStamp*` | Set caller-supplied packed timestamps and nonstructural attributes |
| `fat_create` | `uint32_t parent, FatCreate*, FatEntry*` | Create empty file or directory |
| `fat_rename` | `FatEntry*, uint16_t *new_component` | Rename in the same parent, including case-only rename |
| `fat_remove` | `FatEntry*` | Delete a file or empty directory and its associated valid LFNs |

`FatTransfer = {data_pointer, uint64_offset, uint32_length, uint32_done}`.
Read errors can leave a successfully read prefix in the destination and report
its length in `done`. A failed write leaves `done=0`, caller entry unchanged, and
the operation's staged sectors rolled back. A successful write/resize/rename/
set-info refreshes the supplied entry. File sizes are limited to `0xFFFFFFFF`.

`FatCreate = {UTF16_name_pointer, uint32_directory, uint32_reserved}`. Directory
is 0 or 1; reserved is zero. All new names get LFN records and a collision-checked
`Fxxxxxxx` short alias. New timestamps are zero/unspecified. Data writes set the
archive bit and preserve timestamps: the caller owns clock and timezone policy.

`FatStamp` holds five packed FAT words (creation time/date, access date, write
time/date), followed by creation tenths and attributes. Creation tenths must be
0–199. The caller is responsible for civil-date/time validity; packed fields are
otherwise copied exactly, including unspecified zero dates. Directory, volume,
and reserved attribute bits cannot change through this API. Read-only attributes
block content changes, deletion, and rename; set-info can clear read-only.

Every completed transaction, including rollback, invalidates **all other** entry
snapshots and cursors. Refresh them by lookup/open. `F_STALE` prevents an old
snapshot from targeting a reused slot. Bytewise SFN revalidation additionally
checks the entry against the sector buffer before mutation.

## Supplied Win32 components

```c
WinVolume volume = {0};
SectorBuffer buffer = {0};
FatIdentity identity = {0};

win_open(&volume, L"\\\\.\\X:", 0);       /* check each returned status */
sb_init(&buffer, &volume.ops);
fat_mount(&identity, &buffer.ops, NULL);
/* enumerate/lookup/read; mutations stage in RAM even on a read-only backend */
sb_discard(&buffer);                     /* explicitly discard pending sectors */
win_close(&volume);
```

`win_open(...,1)` requires read/write access and locks the volume before returning.
It retains that lock through writes and explicit flushes. `win_close` dismounts
the volume while still locked, then closes the handle and frees its bounce buffer.
Check its status: failed dismount/handle close returns `F_IO` and records the
Win32 error, even though cleanup is still attempted. Close never commits pending
sectors. A failed open also cleans up and returns an error; it does not retry.
Reopen and remount when changing access mode; do not carry snapshots across it.
The adapter uses a
page-aligned bounce buffer for unbuffered I/O, reports short transfers as errors,
and records the Win32 error in `volume.error`. `win_read`, `win_write`, and
`win_flush` implement the backend callbacks.

`sb_commit(&buffer)` writes the latest version of each pending sector and flushes
the backend. `sb_discard(&buffer)` frees all staged changes. Call
`fat_invalidate(&identity)` after an external discard or direct buffer alteration;
remount if boot geometry changed. Commit preserves the same logical view, so it
does not require invalidation. Neither close nor discard commits implicitly.

## Status codes

`F_OK=0`, `F_END=1`, `F_IO=2`, `F_FORMAT=3`, `F_CORRUPT=4`, `F_RANGE=5`,
`F_READONLY=6`, `F_NOSPACE=7`, `F_NOTFOUND=8`, `F_EXISTS=9`, `F_NAME=10`,
`F_STALE=11`, `F_BUSY=12`, `F_MEMORY=13`, `F_NOTEMPTY=14`, `F_ARGUMENT=15`.

On failed mount the identity remains unmounted. On failed device commit, the
buffer retains its pages and is poisoned: reads, staging, and retries fail.
Discard it, close the device, and establish/repair the actual volume state before
reopening. Logical transaction rollback cannot undo sectors already sent to media.
