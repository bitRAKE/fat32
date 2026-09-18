# Library API

New consumers start with the [shared volume and handle API](SHARED.md). It is
the normal multi-consumer interface. This page retains the lower-level snapshot
API and provider contracts for compatibility and testing. Snapshot writers must
not be mixed with live shared handles. Mount requires four arguments, including
caller-owned workspace, in both interfaces.

The authoritative assembly contracts are [fat32.inc](../fat32.inc) and its
included interface declarations. They list register arguments, widths, pointer
direction, storage lifetime, preconditions, and output validity on success and failure.
All filesystem `public` declarations live in [fat32.asm](../fat32.asm), grouped by
interface with argument summaries. The `.inc` files use assembly syntax;
[fat32.h](../fat32.h) is the public C17 ABI header with layout assertions.
All functions and callbacks use the Windows x64 calling convention.
The [Windows harness](../tests/win32/README.md) documents its separate transport ABI.
Optional [session policy](POLICY.md), [diagnostics](CHECKING.md),
[formatting](FORMATTING.md) and [recovery views/salvage](RECOVERY.md) have separate
contracts and are included only when referenced.
Except the void provider `end` callback, EAX is a status: zero is success.
`fat_dir_next` returns `F_END` (1) at directory end.

All state and output buffers are caller-owned. Zero identities and adapters
before first use. Keep the operations table, its context, and an optional OEM
mapping alive as long as the identity. Serialize access to a buffer and its
identity; separate instances can run independently. Do not modify live identity,
cursor, or entry bookkeeping. All pointer arguments must address their declared
storage; transfer data must address `length` bytes and must not alias library
state. Component strings must be NUL-terminated UTF-16. A zero-length transfer
may use a null data pointer. Other null allowances are stated explicitly.

The backing volume must also remain stable against external writers. Serializing
calls to one identity does not serialize another filesystem owner.
An unlocked raw reader can observe an old cached FAT and a newly written
directory, returning `F_CORRUPT` even though a later consistent view is valid.
Use exclusive ownership or an immutable snapshot for filesystem diagnosis.
After external edits stop, call `fat_invalidate` and reacquire entries/cursors;
remount after geometry or media changes. Repeated invalidation cannot make
concurrent external edits into a consistent snapshot.

Callbacks may overwrite all Win64 volatile registers. Assembly callers must
provide the 32-byte home area and align RSP to 16 bytes before CALL; nonvolatile
registers are preserved. Private `f_*` helpers document any additional register
guarantees beside their definitions. Those guarantees do not extend to providers.

## Sector boundary

`SectorOps` contains:

| Field | Contract |
| --- | --- |
| `context` | Opaque pointer passed to callbacks |
| `read(context,lba,out)` | Read exactly one logical sector; return status |
| `write(context,lba,in)` | Copy a complete sector into the current transaction |
| `begin(context)` | Begin isolated staging; no changes on failure |
| `end(context,accept)` | Accept all staged writes, or restore all on zero; cannot fail |
| `flush(context)` | Backend durability callback; optional ordered commit calls it, ordinary mutation acceptance does not |
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
| `fat_mount` | `SectorOps*, uint16_t *oem_or_null, FatWorkspace*` | Decode bounded geometry from the primary BPB |
| `fat_invalidate` | none | Drop FAT/directory caches, reset allocation hints, and invalidate snapshots/shared ownership |
| `fat_get` | `uint32_t cluster, uint32_t *value` | Masked FAT entry, including reserved entries 0 and 1 |
| `fat_count_free` | `uint32_t *count` | Exact free count in the selected current FAT view; unchanged output on error |
| `fat_chain` | `uint32_t first, uint32_t count_last[2]` | Validate finite chain; return count and final cluster |
| `fat_dir_open` | `uint32_t first, FatCursor*` | Check first cluster and initialize without I/O |
| `fat_dir_next` | `FatCursor*, FatEntry*` | Next file/directory, omitting volume-label and deleted records |
| `fat_lookup` | `uint32_t parent, uint16_t *component, FatEntry*` | Find by long name or short alias |

The optional free scan examines data-cluster entries only, masks their reserved
high bits, and includes accepted pending allocations. It ignores FSInfo hints
and stores no persistent count. Hold the same volume lease throughout the scan.
It is not an ownership or filesystem-health audit.

Use `identity.root_cluster` to begin. Lookup accepts a single component; a caller
resolves a path by repeated lookups and checking the directory attribute. Root
has no ordinary `FatEntry`. Dot/dot-dot records can be enumerated. A dot-dot
cluster of zero denotes the root; callers performing path resolution map it to
`root_cluster`.

`FatIdentity` stores parsed geometry, serial, cache pointers/keys and shared owner
in 152 bytes. `FatWorkspace = {data, uint32_bytes, uint32_reserved}` supplies at
least three sector-sized buffers; reserved is zero. Data outlives the mount,
while the descriptor is borrowed only for mount. The BPB is decoded in scratch
and is not retained as a fourth buffer. Insufficient storage gives `F_MEMORY`
before I/O. Failed mount retires the previous view and leaves magic clear.
All interpretation and cache management are in `fat32.lib`. A supplied OEM table
has 256 UTF-16 entries; null selects CP437. There is no Unicode normalization or
non-ASCII case folding. Damaged LFN sequences fall back to the preserved SFN.

Successful mutations retain the in-memory allocation cursor (`next_free`) and
unaffected sector caches. The cursor starts a bounded, wrapping FAT search; each
candidate is checked against the selected FAT. It is independent of the on-disk
FSInfo hints, which mutations mark unknown when valid FSInfo sectors exist.
Rollback clears both cache keys and resets the cursor to cluster 2. Explicit
`fat_invalidate` also resets that state after external edits or discarded staging;
it advances the generation and retires shared ownership without reloading the BPB.
Snapshot mutation completion advances the generation separately, preserving the
cache/cursor disposition of the completed transaction.

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
Zero-length writes validate the request but do not start a transaction or change
the file. Equal-size resize requests still transact.

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

Every completed snapshot transaction, including rollback, invalidates **all other** entry
snapshots and cursors. Refresh them by lookup/open. `F_STALE` prevents an old
snapshot from targeting a reused slot. Bytewise SFN revalidation additionally
checks the entry against the sector buffer before mutation.
After mutation failure, also reacquire the supplied entry: its unchanged bytes
can contain a generation that the rollback has invalidated.

## Status codes

`F_OK=0`, `F_END=1`, `F_IO=2`, `F_FORMAT=3`, `F_CORRUPT=4`, `F_RANGE=5`,
`F_READONLY=6`, `F_NOSPACE=7`, `F_NOTFOUND=8`, `F_EXISTS=9`, `F_NAME=10`,
`F_STALE=11`, `F_BUSY=12`, `F_MEMORY=13`, `F_NOTEMPTY=14`, `F_ARGUMENT=15`,
`F_LIMIT=16`, `F_ATTENTION=17`, `F_VERIFY=18`. Limit/attention belong to optional diagnostics:
budget exhaustion and dirty/error history respectively, neither proof of
structural damage. See [CHECKING.md](CHECKING.md) for separately callable checks
and checked reads. Basic mount and directory-open no longer walk full chains.

An optional 80-byte [provider extension](ORDERING.md) records mutation phase
boundaries. Base `SectorOps` remains 64 bytes with its reserved field zero.
This extension supplies ordering information for a separate commit module;
operation acceptance itself still performs no physical commit. See that contract
for caller-funded indexed staging, explicit ordered commit and its separately
linked verified alternate (`F_VERIFY` means backend readback differed).

On failed mount the identity remains unmounted. Physical-commit failure follows
the selected provider's contract; see [ordered commit](ORDERING.md). Logical
transaction rollback cannot undo sectors already sent to media.

## FAT update hooks

`fat_put(identity, index, value)` is the basic low-level mutation hook used by
allocation, linking and freeing. It updates every mirrored copy, or only the
selected active copy, preserving each copy's reserved high nibble. Each updated
sector is read once for read/modify/write; previous low bits are not compared.
`fat_put_checked` is a separately linked alternate retaining selected-entry and
mirror-agreement checks. It does not call `fat_put`, so a consumer replacement
of `fat_put` can delegate to it without recursion. See [LINKING.md](LINKING.md).

Both require a valid mounted identity, its operation lease and an already
active provider transaction. They neither begin nor end that transaction.
The outer mutation must roll back any failure (including a later-copy mismatch),
maintain allocation/FSInfo hints, and publish canonical metadata and versions.
These are composition hooks, not standalone arbitrary-FAT mutation operations;
calling one between ordinary file operations does not satisfy those duties.
FAT indices 0 through `cluster_count+1` are addressable; values contain only
low 28 bits. Invalid mount/index returns `F_CORRUPT`, high bits `F_ARGUMENT`,
and absent active transaction `F_BUSY`, all without I/O. Every return invalidates
the FAT cache key. They do not commit physical storage or change mount generation.

The basic interface assumes the normal coherent volume contract. A discovered
mirror conflict belongs to explicit checking/recovery policy; basic update is
not a repair decision. The checked alternate retains the former rejection
behavior for consumers choosing it. Two-FAT cold-cache updates take two reads
in the basic hook and three in the checked hook, plus two staged writes.
