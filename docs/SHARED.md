# Shared volume and file handles

This is the default interface for new filesystem consumers. One `FatVolume`
owns the canonical open files over one mounted `FatIdentity`; handles share
metadata and caches while retaining independent positions and access flags.
The assembly layouts are in [fat32.inc](../fat32.inc) and [shared.inc](../shared.inc).
All exports return `F_*` in EAX and use Win64. There are no imports, hidden
allocations, locks supplied by an OS, or required global dispatch tables.

## Mount storage and ownership

`fat_mount(identity, ops, oem_or_null, workspace)` now takes four arguments.
`FatWorkspace = {data, uint32_bytes, uint32_reserved}` describes at least
`3 * ops.sector_bytes` bytes, with reserved zero. Insufficient capacity returns
`F_MEMORY` before provider I/O; invalid reserved/overflowing storage returns
`F_ARGUMENT`. The three regions hold a FAT sector, directory sector and operation
scratch. Mount decodes the primary boot sector in scratch. It retains the data
storage, operations/context and OEM map, but not the workspace descriptor.
Every region must remain valid and separate from other arguments until the
mount is retired. Provider callbacks consume/copy buffers before returning.

Mount reads only the primary BPB and publishes bounded geometry. Directory-open
checks its first cluster and performs no I/O. Optional [structural checks and
checked reads](CHECKING.md) inspect chains, lengths, reserved/status entries,
backup metadata, directory structure/name uniqueness, selected FAT-copy ranges
and optional volume ownership. A successful
basic operation does not certify unexamined filesystem health. The optional
adaptive reader adds bounded file-chain diagnosis after an ordinary read fails;
ordinary operations never repair media.

Initialize `FatVolume` to zero once, then call
`fat_volume_init(volume, identity, object_pool, capacity)`. The identity must be
mounted and idle. Only one shared owner may attach to an identity; another
manager receives `F_BUSY`. Use one owner for all consumers of the same mounted
medium, not multiple independently cached writable identities. Excluding other
drivers/external writers remains the caller's responsibility.
Before reusing a manager for a different identity, call `fat_volume_close`.
Closing an already retired owner reports `F_STALE` and clears its identity
pointer without altering any replacement owner. Keep identity storage alive
until that detach; reinitializing against the same identity while idle is allowed.

The pool has 128-byte objects. Slot zero is the pinned root; add capacity for
each distinct simultaneously open file and each pinned ancestor directory.
Opening another handle of the same file consumes a reference, not another
object. Pool exhaustion returns `F_MEMORY`; create checks capacity before its
transaction. Open directory slots are keyed by their parent object and physical
SFN location. An incarnation prevents a retired object from aliasing a reused
pool/directory slot. Evicting a sector cache never retires an object.

| State | Bytes | Storage/lifetime |
| --- | ---: | --- |
| `FatIdentity` | 152 | Geometry, cache keys/pointers and owner; entire mount |
| Sector workspace | 3 × sector bytes | 1,536–12,288 bytes; entire mount |
| `FatVolume` | 48 | Shared owner; zero before first initialization |
| `FatObject` | 128 each | Caller-sized pool, including root/ancestor pins |
| `FatHandle` | 48 each | Independent consumer position/access; zero before acquisition |
| `FatRecord` | 80 | Name-free metadata result from `fat_handle_info` |
| `FatIterator` | 632 | Optional enumeration state with its own directory reference |
| `FatTransfer` / `FatBuffer` | 24 / 16 | Explicit-offset / sequential request |

Provider state, operation stack, staging, stream maps and validation reports are
separate budgets. No 512-byte name buffer is embedded in a file handle/object.
Do not edit library-owned fields, copy a live handle, or free state while a
provider call is suspended. `FatRecord` results are values, not writable handles.

## Operations

The first four arguments use RCX/RDX/R8/R9; 32-bit access/count arguments use
the corresponding low dword. All required pointers address complete distinct
objects/buffers. Names are one NUL-terminated UTF-16 component. A zero-length
transfer may have null data. Error outputs are unchanged except the explicitly
documented `done`, iterator, and close behavior below. Mount attempts retire the
previous mounted view, including on failure, and leave magic clear on error.

| Function | Arguments | Behavior |
| --- | --- | --- |
| `fat_root` | volume, access, out handle | Acquire another root reference; no I/O |
| `fat_open` | parent handle, name, access, out handle | Lookup LFN/SFN and acquire/reuse canonical object |
| `fat_new` | parent handle, FatCreate, out handle | Create file/directory and return read/write handle |
| `fat_close` | handle | Release reference and any unused ancestor pins; clear handle.volume |
| `fat_volume_close` | volume | Detach only with no handles/iterators/pins other than root; otherwise busy |
| `fat_handle_info` | handle, out FatRecord | Current coherent metadata; no I/O |
| `fat_handle_get_stamp` | handle, out FatStamp | Decode canonical timestamps/attributes for files or directories; no I/O, unchanged output on stale handle |
| `fat_seek` | handle, uint64 position | Set independent position; beyond EOF permitted |
| `fat_read_at` / `fat_write_at` | handle, FatTransfer | Explicit offset; do not move handle position |
| `fat_read_next` / `fat_write_next` | handle, FatBuffer | Use position and advance by completed bytes |
| `fat_handle_resize` | handle, uint64 size | Truncate/zero-fill growth, bounded by 0xFFFFFFFF; always advance chain version because equal-size resize can trim excess allocation |
| `fat_handle_set_info` | handle, FatStamp | Set explicit times/attributes; may clear readonly |
| `fat_handle_rename` | handle, new name | Same-parent rename preserving canonical identity |
| `fat_unlink` | parent handle, name | Remove file/empty directory only when it has no live references |
| `fat_iter_open` | directory handle, out iterator | Acquire directory reference and initialize enumeration |
| `fat_iter_next` | iterator, out FatEntry | One entry; F_END is exhaustion; output usable only on F_OK |
| `fat_iter_close` | iterator | Release its reference, including after stale/error result |

Access is `FH_READ` (1), `FH_WRITE` (2), or both (3). Reading, opening children
and enumeration require read access; mutations require write access. FAT readonly
attributes additionally prevent content changes, rename and removal. Clearing
readonly through set-info requires a writable handle. Root has no ordinary SFN
record and cannot be renamed, resized or have entry metadata set.

Output handles/iterators must be zero or previously closed; acquiring into an
already live output returns `F_BUSY`. `fat_open` excludes `.` and `..`; a path
layer retains its parent handles and handles separators/navigation. Enumeration
may still report dot records. Default name matching/creation follows the UTF-16,
CP437, ASCII case folding and generated-SFN rules in [API.md](API.md).

`FatTransfer = {data, uint64_offset, uint32_length, uint32_done}`.
`FatBuffer = {data, uint32_length, uint32_done}`. Reads clamp at EOF and report a
completed prefix on error. Ordinary reads follow only the requested part of the
chain; they check encountered IDs and bound traversal by volume geometry. They
do not reread the directory record or audit an unused chain tail. A successful
prefix read is not a certificate of whole-file/volume health. Full validation
will be provided by separately selected functions. A short chain returns
`F_CORRUPT` after its readable prefix; sequential position advances by that prefix.
Writes return done=0 on failure and preserve position. Accepted zero-length
writes start no transaction and change no versions or position.

## Visibility and invalidation

Core mutations edit a private 80-byte record and provider savepoint, then
publish canonical metadata only after successful acceptance. All same-file
handles immediately see accepted size/content/chain, including pending staged
bytes before physical commit. A failed operation rolls back without changing
canonical records or versions. A mutation of B does not retire A, even if they
share a FAT or directory cache sector. No automatic lookup/reopen is performed.

Each object has file, chain and directory versions. A content/metadata update
advances the file version; size/first-cluster changes also advance its chain
version. Rename keeps its chain version and handle identity. Namespace edits
advance the affected parent's directory/chain versions, conservatively covering
directory growth. Unrelated directories retain valid iterators; changing the
iterator's own namespace returns `F_STALE`. Ordinary data/attribute updates do
not restart directory iteration. Version/incarnation exhaustion rejects further
mutation/acquisition rather than wrapping.

Open children and iterators pin their parent directory objects. Closing the
parent handle does not break those references. Unlink of any referenced object
returns `F_BUSY`; there is no open-unlinked-file mode. After the last reference
closes, a later open may use a new incarnation. `fat_close` on an already stale
handle returns `F_STALE` and clears its volume pointer without touching pool
references. Other failed operations leave the handle available for diagnosis.

Remount, `fat_invalidate`, or reinitializing this `FatVolume` while idle retires
the affected handles. Reinitialization advances an epoch before pool reuse.
Whole-mount retirement is appropriate for external edits/media replacement or
uncertain physical completion. Do not continue with cached handles after those
events. `fat_volume_close` neither commits/discards staging nor closes a device.

## Serialization and durability

The ordinary functions require caller serialization over the shared identity,
cache/scratch and provider. Hold ownership across the complete operation,
including all yielding/blocking provider callbacks. No callback may reenter the
same owner. Callers can serialize several ordinary functions under one lease.

The optional `fat_call_locked(volume, FatCall)` acquires the volume gate with
one compare/exchange, calls a caller-selected target with four Win64 arguments,
then releases it. `FatCall = {target, arg1, arg2, arg3, arg4}`. The target must
operate on this volume and return normally; all pointers and its descriptor
remain live until return. A busy gate returns `F_BUSY` without calling the
target or changing its outputs. The library never spins over a device wait.
Use this same gate for every overlapping caller, or supply equivalent external
serialization. Direct calls racing a gated call are unsupported. Initialization
and volume close occur only after draining callers, outside the gate.

Accepted staging is not durable completion. The existing SectorOps savepoint
contract still applies. The [optional ordered provider](ORDERING.md) uses caller
storage, fallible commit reports and separately selectable readback verification.
Commit/discard hold the same volume lease, including suspended backend callbacks.
Successful commit preserves coherent handles; uncertain completion invalidates
them. The Win32 `SectorBuffer` continues to be a host testing adapter.

## Compatibility and evidence

An optional [session policy](POLICY.md) can wrap operations from every consumer
under this same volume lease. It preserves known damage across handle close and
reopen, restricts subsequent operations, and invokes caller-selected bounded
diagnosis only after corruption or I/O failure. It adds no shared-volume fields.

The `FatEntry` snapshot operations in [API.md](API.md) remain separate compatibility
entry points. They keep full-file preflight and global snapshot invalidation;
none is a dependency of the shared file operations. Do not mix snapshot writers
with live managed handles: compatibility completion retires the attached owner.
Mount's workspace argument/layout is an ABI change and all consumers must rebuild.

The 95-suite harness includes shared behavior over 512-byte through 256-KiB clusters,
same-sector interleaving, parent lifetimes, slot reuse, remount, pool exhaustion,
per-write rollback injection, partial reads, exact workspace bounds across all
four sector sizes, ABI probes and actual overlapping Windows threads paused in
a provider callback. Feature tests prove removal across 264 MSVC/Lld links,
including separately selected synchronization and [streams](STREAMING.md).
Maps track canonical chain versions and reads observe same-file content changes.
Recovery and physical power-loss behavior require separate qualification.
