# Implementation and verification

## Style and build

The production implementation is assembly. `common/policy.g` follows the
NEWCOFF, static-RSP, CodeView, and unwind policy in `hexed`. It retains
the installed toolchain's format-finalizer and grouped-USES workarounds. Unlike
an application with a permanent RBX state pointer, each library entry preserves
the complete Win64 nonvolatile contract for arbitrary callers.

The filesystem object includes only fasm2's instruction, structure, and procedure
machinery. Defining `FAT32_WIN32` enables the generated Win32 projection for the
adapter, buffer, and example. The source layout is:

```text
fat32.asm                one filesystem library object
  fat/volume.inc         BPB, geometry, FAT, allocation, transactions
  fat/directory.inc      cursors, LFN/SFN decoding, lookup, slot mapping
  fat/file.inc           byte I/O, resize, metadata, snapshot checks
  fat/create.inc         create, grow directories, rename, remove
fat32.h                  caller-owned identity and filesystem ABI
buffer.asm / buffer.h    format-neutral staged sector versions
win32.asm                handle, sector geometry, lock, seek/read/write/flush
example.asm              small CRT-free client
tests/test.c             independent sparse-media fixtures and fault injection
tests/usb.c              raw/native interoperability harness
tests/api.h              C ABI mirror
```

`nmake` produces `fat32.lib`, `sector.lib`, and `fatdemo.exe`. Test harnesses use
the C runtime; the production libraries do not. `fatdemo.exe` imports only
KERNEL32 and carries hexed's subsystem-10 load configuration. `nmake verify`
prints unwind records, undefined core symbols, and executable imports, and emits
the core disassembly. Structure layouts are asserted in assembly and C.

`locals` containing a structure can change fasmg's dot-label scope. Procedures
place a `body:` anchor after `endl` to keep control-flow labels separate from
fields such as `request.done`. All nonvolatile pushes occur in PROC prologues.

## Invariants

1. All logical sector addresses are relative to the mounted volume, with a
   bounded provider extent. Arithmetic uses 64 bits where products/offsets can
   exceed 32 bits. The BPB cluster size is held in a dword, so 65,536 never wraps.
2. FAT32 classification uses the data-cluster count. Boot signature, sector and
   cluster powers of two, reserved/FAT extents, FAT capacity, root cluster,
   active FAT, and version are checked before publishing a mounted identity.
3. FAT reads mask the high nibble. FAT writes preserve each copy's high nibble,
   update all mirrored copies, and reject disagreement in the low 28 bits of a
   touched entry. Disabled mirroring accesses only the selected FAT.
4. Chains are bounded by volume geometry and checked with Brent cycle detection.
   Reads validate the whole file chain and its minimum length before transferring
   bytes. Directory open validates its chain before iteration.
5. The cached FAT/directory sectors are read-through views, never hidden dirty
   state. A write immediately invalidates both keys. Failed reads never leave a
   usable cache key. All mutations go through the sector transaction.
6. LFN ordinals, checksum, type, zero cluster, termination, padding, length, and
   UTF-16 validity must agree. Otherwise lookup uses the preserved short alias.
   Valid long sets remain associated with their precise SFN slot across boundaries.
7. Allocated clusters are zeroed; newly exposed bytes in an existing partial
   cluster are also zeroed. Partial writes preserve adjacent bytes. Truncation
   releases only the detached chain after validation.
8. A failed mutation leaves the previous overlay and caller entry intact.
   Generation changes force clients to refresh other snapshots after any
   completed operation. This includes an operation that rolled back.
9. FSInfo is advisory. Allocation scans actual FAT entries, never reports full
   based on a hint, and sets both hints unknown in valid primary/backup FSInfo
   sectors after successful mutation. Invalid FSInfo signatures are left alone.
10. The Win32 sector layer contains no filesystem decisions. It never chooses
    a cluster, interprets an entry, repairs a FAT, or applies a partition offset.

## Buffer transactions and memory

SectorBuffer stores a linked stack of sector versions in VirtualAlloc-owned
pages. `begin` records the head as a savepoint. A write can update a version
created since that savepoint; an older version is immutable and gets a new
version instead. Rollback frees the prefix back to the savepoint. Acceptance
keeps it. Neither path needs another allocation or device write.

The default version limit is 131,072, configurable through `buffer.limit` before
use. Each version has a 4 KiB payload plus header and consumes 8 KiB of committed
virtual pages on this host. It is intended for bounded editing sessions, with
explicit commit/discard between batches. Lookup is linear in pending versions;
commit eliminates superseded versions by scanning newer ones. This deliberately
small adapter is not a large-workload block cache. FAT lookup and random file
reads likewise prioritize validation over streaming performance.

## Device lifetime and durability

Raw read mode is a live view; it is not an OS snapshot. Other writers can change
the medium. For a writable session, acquire `win_open(...,1)` **before** mount and
hold it until after commit/close. Locking flushes host filesystem caches. Keep the
filesystem locked through all writes and `FlushFileBuffers` calls; on this FAT32
device an early dismount caused even a flush without writes to fail with
`ERROR_NOT_READY`. Close dismounts while still locked, then releases the handle,
so subsequent Windows access remounts the edited volume. Dismount and handle-close
failures propagate. This follows the lock/change/dismount/release sequence in
[Microsoft's dismount guidance](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_dismount_volume).

A lock failure is an error; no fallback raw write is attempted. The USB harness
retries denied/sharing/lock-conflict writable acquisition up to 20 times with
250 ms between attempts, before staging anything. The adapter itself has no retry
policy. A failed commit is never retried. The example is read-only.

Each backend request transfers exactly one sector through an aligned bounce
buffer. Device seek, read, write, and flush failures propagate, including short
transfers. The file pointer makes the adapter a serialized object.

In-memory operation acceptance is atomic. **Device commit is not**. The generic
buffer has no filesystem ordering/journal policy and may write metadata and data
in any order. Interrupted or partially failed commits can require filesystem
recovery. There is no claim of crash consistency, power-fail safety, automatic
undo, or physical-sector atomicity. Failure poisons the buffer and preserves its
staged pages, preventing an uncertain retry. No automatic repair is attempted.

## Deliberate limits

- FAT32 only, with clusters through 64 KiB. No FAT12/16, exFAT, formatter, MBR,
  GPT, or multi-volume manager.
- Component lookup and same-parent rename. Path policy and cross-parent moves
  belong to a future API; raw callers can traverse through directory entries.
- No volume-wide ownership map, orphan recovery, or cross-link repair. Local
  structural checks are not a certification that a damaged volume is writable.
- ASCII case folding; exact non-ASCII UTF-16 matching. Short-name OEM decoding
  is explicit and independent of the machine's current locale.
- No implicit clock, timezone conversions, current-directory global, threading,
  asynchronous I/O, memory-mapped files, or hidden writeback.

See [VALIDATION.md](VALIDATION.md) for actual coverage and hardware findings.
