# Integrating FAT32 into an x64 OS

This guide is for an assembly-language OS developer starting through UEFI.
It follows the library in this repository, including its actual register and
transaction contracts.

The integration point is **a bounded, synchronous sector provider**. Link
`fat32.lib` into your loader or kernel, supply `SectorOps`, and keep one
caller-owned `FatIdentity` for that provider. All FAT32 interpretation stays in
the library. Your OS supplies device discovery, memory, serialization, and the
policy for making staged writes durable.

The companion [reader.asm](reader.asm) implements the read-only adapter and file
reader described here; [reader.inc](reader.inc) defines their caller-owned context
and parameter contracts. This is a linkable example module, not a complete
bootloader or firmware driver.

Read [fat32.inc](../../fat32.inc) for the exact filesystem ABI and
[API.md](../../API.md) for its function index. [DEVELOPING.md](../../DEVELOPING.md)
describes the implementation; [VALIDATION.md](../../VALIDATION.md) records tests
and their limits.

The library supports clusters through 64 KiB for this x86-64 target. See
[64 KiB support](../../CLUSTER64.md) for arithmetic and validation coverage.
Keep the backing volume stable against other writers while an identity caches
its metadata; after external edits stop, invalidate and reacquire snapshots.

Contents: [provider](#2-establish-the-sector-boundary),
[ABI](#3-respect-the-abi-at-both-sides-of-the-call),
[UEFI adapter](#4-a-small-read-only-uefi-adapter),
[file access](#5-state-memory-and-file-access),
[writes](#6-add-writes-through-a-transactional-provider),
[handoff](#7-plan-the-uefi-to-kernel-transition),
[errors](#8-map-errors-and-metadata-deliberately),
[verification](#9-bring-up-the-integration-in-observable-stages).

## 1. Choose the lifetime of the mount

There are two useful starting points:

| Design | Provider | What crosses the loader/kernel handoff |
| --- | --- | --- |
| Load everything before leaving firmware | UEFI Block I/O, read-only | Loaded file bytes and your boot-information structures |
| Continue filesystem access in the kernel | Kernel block driver, optionally behind staging | Device/partition identity; kernel constructs a fresh provider and mount |

```text
loader or kernel: paths, handles, permissions, timestamps
                          |
                 fat32.lib + FatIdentity
          boot geometry / FAT cache / directory cache
                          |
                       SectorOps
                          |
           optional transactional sector overlay
                          |
           UEFI Block I/O     OR     kernel block driver
```

UEFI Block I/O belongs to the boot-services environment. After successful
`ExitBootServices`, firmware device protocols cannot service the kernel's disk
requests. Merely retaining their pointers does not extend their lifetime.
[UEFI boot-services transition](https://uefi.org/specs/UEFI/2.10_A/07_Services_Boot_Services.html#efi-boot-services-exitbootservices).

The FAT32 core itself has no firmware or Win32 imports. Its lifetime is limited
by the code, data, provider, and mappings you keep alive. Start with a read-only
loader mount; add a fresh kernel mount when your block driver is ready.

### What to link

Build the core from the repository root:

```cmd
build.cmd example\uefi\reader.obj fat32.lib
build.cmd examples-test
```

This builds `fat32.lib` and `example\uefi\reader.obj`. Link the example object
and library with your own entry point and firmware setup. Both are AMD64 COFF.
`examples-test` separately runs a host C mock of the firmware callback; it never
accesses a physical device. Assembly declarations use `.inc`; `tests/api.h` is
the separate C ABI mirror used by the host tests.
The [Win32 demo](../win32/README.md), `win32.asm`, and
`buffer.asm` are Windows-host components. In particular, the supplied sector
overlay uses `VirtualAlloc`/`VirtualFree`; it needs an allocator port or a
replacement before use in an OS.

For an x64 UEFI PE/COFF image, integrate the object into your existing EFI image
link. For an ELF kernel, adapt the assembly object policy and linker integration,
or use a supported COFF conversion path that you verify. Object conversion does
not change the calling convention. Resolve relocations and keep required code
and constant-data sections; `.obj`/`.lib` files are not flat executable images.
The core should still have no undefined external symbols after assembly.

## 2. Establish the sector boundary

The following are library integration rules, independent of the controller:

| `SectorOps` field | Value to supply |
| --- | --- |
| `context` | Your live provider state pointer |
| `read` | Synchronous one-sector callback |
| `write`, `begin`, `end` | All zero for a direct read-only mount; transactional callbacks for a writable mount |
| `flush` | Optional backend durability operation; FAT32 never calls it |
| `sectors` | Count of accessible **volume-relative** logical sectors |
| `sector_bytes` | 512, 1024, 2048, or 4096, matching the FAT32 BPB |
| `reserved` | Zero |

**Provider LBA 0 must be the FAT32 boot sector.** The library neither parses a
partition table nor adds the BPB hidden-sector count. Keep three units distinct:
file byte offsets, volume sector numbers, and device block numbers. A cluster
number belongs entirely inside FAT32 and is never a device LBA.

For a UEFI partition Block I/O handle, use its partition-relative origin. With
a whole-device handle, your partition layer adds the selected partition's base
exactly once. `LogicalPartition`, `BlockSize`, `LastBlock`, and `IoAlign` describe
the handle; `LastBlock` is inclusive. A transfer buffer must satisfy `IoAlign`.
[UEFI Block I/O media definition](https://uefi.org/specs/UEFI/2.11/13_Protocols_Media_Access.html#efi-block-io-protocol).

For the simple adapter below, require one provider sector per firmware block:

```text
firmware_lba = selected_base_lba + volume_lba
transfer_bytes = media_block_size = SectorOps.sector_bytes
0 <= volume_lba < selected_sector_count
```

Validate addition, extent, and allocation arithmetic before publishing the
provider. Reject unsupported block sizes or add a separate byte/block translation
layer. A 512-byte BPB does not become compatible with a 4096-byte backend merely
by changing `SectorOps.sector_bytes`.

Choose the intended device and partition explicitly in the loader. Do not mount
the first enumerated Block I/O instance. A removable boot medium can use FAT12
or FAT16; this library accepts only FAT32.
[UEFI filesystem formats](https://uefi.org/specs/UEFI/2.11/13_Protocols_Media_Access.html#file-system-format).

Close firmware file handles on the selected volume before direct block access.
Firmware file access and this raw-sector view must not run as competing owners;
the firmware file interface may hold cached state.
[UEFI file-protocol access rules](https://uefi.org/specs/UEFI/2.11/13_Protocols_Media_Access.html#efi-file-protocol).

## 3. Respect the ABI at both sides of the call

All public FAT32 functions and provider callbacks use the x64 Microsoft-style
integer calling convention:

| Purpose | Location |
| --- | --- |
| First four arguments | RCX, RDX, R8, R9 |
| 32-bit scalar arguments | Corresponding low register, such as EDX |
| Library result | EAX, one of `F_*`; zero is `F_OK` |
| Callback `end` result | Void; it must not fail |
| Provider `read`/`write` arguments | RCX=context, RDX=64-bit LBA, R8=one-sector buffer |
| Nonvolatile GPRs | RBX, RBP, RSI, RDI, R12–R15 |

Reserve the 32-byte home area and align RSP to 16 bytes before CALL. At callee
entry, the pushed return address leaves RSP eight bytes off that alignment.
The first stack argument is at caller `[rsp+32]`, hence callee-entry `[rsp+40]`.
Preserve the ABI's nonvolatile SIMD state if your callback uses it, and enter
with DF clear. The x64 UEFI calling convention is compatible with these calls.
[UEFI x64 calling convention](https://uefi.org/specs/UEFI/2.10_A/02_Overview.html#detailed-calling-conventions).

A kernel with a different internal ABI needs wrappers at **both** boundaries:
kernel-to-library and library-to-provider. A callback is not exempt because it
runs inside your kernel. Keep interrupts and other contexts from reentering the
same mount while a callback is servicing it.

The examples use the repository's `proc`, `uses`, `locals`, and `fastcall`.
`fastcall` assigns arguments left to right; it is not a parallel register move.
Save any source that an earlier argument assignment would overwrite. Stack
argument setup can also use RAX. A saved register is useful across a call;
reloading its value into an unchanged incoming register before that call is not.
Use one epilogue for a framed procedure, and preload an error status before an
adjacent comparison and conditional branch.

These examples are procedures, not EFI image entry points. Return `F_*` to your
own code. Translate to `EFI_STATUS` if returning from a firmware-facing entry
point; FAT32 status numbers are not EFI status values.

## 4. A small read-only UEFI adapter

The following three assembly blocks are the routines in [reader.asm](reader.asm).
Use that source and its header as the buildable example. Protocol discovery,
allocation, and `ExitBootServices` remain in your loader.

The context is **our own structure**, not an EFI structure overlay. Initialize
it from your normal UEFI bindings:

| Context field | Initialization and lifetime |
| --- | --- |
| `protocol` | Selected live `EFI_BLOCK_IO_PROTOCOL*` |
| `read_blocks` | Its `ReadBlocks` function pointer |
| `base_lba` | Zero for a partition handle; selected base for a whole-device handle |
| `sectors` | Validated selected extent, nonzero |
| `bounce` | Exclusive buffer of at least `block_bytes`, aligned for the protocol |
| `last_status` | Initially zero; full native status for diagnostics |
| `media_id` | Snapshot of the selected medium's ID |
| `block_bytes` | Validated 512/1024/2048/4096 |
| `online` | 1 only while this firmware session can be used |
| `reserved` | Zero |

The firmware call is `ReadBlocks(This, MediaId, LBA, BufferSize, Buffer)`.
The buffer is a fifth, stack-passed argument. Preserve the full 64-bit returned
status; this adapter accepts only `EFI_SUCCESS` and translates other results to
`F_IO`. The original status remains in the context.
[EDK II Block I/O declarations](https://github.com/tianocore/edk2/blob/master/MdePkg/Include/Protocol/BlockIo.h).

Use a bounce buffer even if your loader's own file buffer is aligned: the FAT32
identity's internal sector arrays do not promise controller alignment. Retain
the allocator's original allocation separately if alignment requires rounding
the usable address upward.

```asm
include '../../common/policy.g'
include 'reader.inc'

extrn fat_mount
extrn fat_lookup
extrn fat_read

; RCX=initialized GuideEfiRead*, RDX=volume LBA, R8=sector output.
; EAX=F_*; output changes only after a successful complete firmware read.
; Caller serializes the context/bounce buffer and keeps boot services usable.
proc guide_efi_read uses rbx rsi rdi
    mov rbx, rcx
    mov rdi, r8
    mov eax, F_IO
    cmp dword [rbx + GuideEfiRead.online], 1
    jne .done
    mov eax, F_RANGE
    cmp rdx, [rbx + GuideEfiRead.sectors]
    jae .done
    add rdx, [rbx + GuideEfiRead.base_lba]
    jc .done
    mov r10, rdx                  ; survives earlier RCX/EDX setup
    fastcall [rbx + GuideEfiRead.read_blocks], \
        [rbx + GuideEfiRead.protocol], [rbx + GuideEfiRead.media_id], \
        r10, [rbx + GuideEfiRead.block_bytes], [rbx + GuideEfiRead.bounce]
    mov qword [rbx + GuideEfiRead.last_status], rax
    test rax, rax                 ; EFI_STATUS is 64 bits on x64
    jnz .failed
    mov rsi, qword [rbx + GuideEfiRead.bounce]
    mov ecx, dword [rbx + GuideEfiRead.block_bytes]
    rep movsb
    xor eax, eax
.done:
    ret
.failed:
    mov dword [rbx + GuideEfiRead.online], 0
    mov eax, F_IO
    jmp .done
endp
```

On firmware failure, the adapter disables further device reads. The caller must
also retire the mount before accepting another filesystem request: a cached
FAT/directory access might otherwise avoid the callback. Diagnose the native
status, rediscover the medium as needed, and mount afresh. Do not silently adopt
a new media ID under cached geometry.

This wrapper publishes a read-only operations table and mounts it:

```asm
; RCX=zeroed, unused FatIdentity*; RDX=validated GuideEfiRead*;
; R8=caller-owned SectorOps* output, not attached to another live mount.
; All three objects are distinct and remain live for the entire mount.
; EAX=mount status; context validation/allocation is the loader's job.
proc guide_mount_readonly
    mov qword [r8 + SectorOps.context], rdx
    lea rax, [guide_efi_read]
    mov qword [r8 + SectorOps.read], rax
    mov qword [r8 + SectorOps.write], 0
    mov qword [r8 + SectorOps.begin], 0
    mov qword [r8 + SectorOps.end], 0
    mov qword [r8 + SectorOps.flush], 0
    mov rax, qword [rdx + GuideEfiRead.sectors]
    mov qword [r8 + SectorOps.sectors], rax
    mov eax, dword [rdx + GuideEfiRead.block_bytes]
    mov dword [r8 + SectorOps.sector_bytes], eax
    mov dword [r8 + SectorOps.reserved], 0
    fastcall fat_mount, rcx, r8, 0 ; RDX gets ops before R8 becomes null OEM
    ret
endp
```

`fat_mount` checks geometry and the root chain, then caches the BPB. A failed
mount leaves the identity unmounted. Do not infer writability or general media
health from a successful mount: this is a local structural check, not an
ownership scan of every cluster.

## 5. State, memory, and file access

Allocate identity storage outside a small kernel stack. Current sizes are:

| Object | Bytes | Intended lifetime |
| --- | ---: | --- |
| `SectorOps` | 64 | Entire mount |
| `FatIdentity` | 16,504 | Entire mount; zero before first use |
| `FatEntry` | 592 | Until invalidated, removed, or replaced |
| `FatCursor` | 576 | One enumeration at the current generation |
| `FatTransfer` | 24 | One transfer request; caller owns data storage |

Use `sizeof` and field constants from the header, rather than duplicating these
sizes in code. Identity, provider context, optional OEM map, callbacks, and their
backing pages must remain valid. The identity contains pointers and is not a
serialized on-disk object. Copying its bytes to a new virtual address does not
repair its provider pointers or update external users.

The core allocates no heap memory, but helpers use stack locals and nested calls.
Budget stack for library **plus provider/driver** call chains. Existing test
coverage is not a measured worst-case kernel stack bound. Do not place a 16 KiB
identity on a small interrupt stack and then enter the library.

### Read one component in the root

Initialize `FatTransfer.data`, `.offset` in bytes, and `.length` in bytes. The
destination must have that capacity and must not alias identity/provider state.
The component is NUL-terminated UTF-16, for example `du 'kernel.bin',0`.

```asm
; RCX=mounted identity*, RDX=UTF-16 root component, R8=FatTransfer*.
; EAX=F_*; done=0 if lookup fails, otherwise fat_read's completed byte count.
; Entry is private to this call; output data may contain a prefix on read error.
proc guide_read_root_range uses rbx rsi
    locals
        entry FatEntry
    endl
body:
    mov rbx, rcx
    mov rsi, r8
    mov r9, rdx                  ; preserve name before EDX becomes parent
    mov dword [rsi + FatTransfer.done], 0
    fastcall fat_lookup, rcx, [rcx + FatIdentity.root_cluster], r9, addr entry
    test eax, eax
    jnz .done
    fastcall fat_read, rbx, addr entry, rsi
.done:
    ret
endp
```

On `F_OK`, advance by `.done`, not by the requested length. A zero count means
EOF for a nonzero request at/past file end. A zero-length request also completes
with zero. On failure, preserve the reported prefix for diagnostics or discard
it according to your loader's policy; never treat it as a complete executable.
Executable format checks and image authentication belong to the loader.

For repeated reads, keep the entry and call `fat_read` directly with successive
offsets; the wrapper above intentionally repeats lookup for a simple example.
The current library validates the full file chain for each public read. Many
tiny reads of a large fragmented file repeat traversal work; choose a useful
caller buffer size and measure your workload before adding an OS cache.

### Resolve paths and enumerate directories

`fat_lookup` handles one component. To resolve `EFI\MYOS\kernel.bin`:

1. Start with `identity.root_cluster`.
2. Look up `EFI`; require `entry.raw[11] & 0x10`, then use `entry.cluster`.
3. Look up `MYOS` in that directory and apply the same check.
4. Look up `kernel.bin`; ordinary-file reads reject directory entries.

Your path layer handles separators, root selection, `.`/`..`, and normalization.
A dot-dot cluster of zero denotes root and must be translated to `root_cluster`.
The root itself has no ordinary `FatEntry`.

To enumerate, call `fat_dir_open(identity, first_cluster, cursor)` and then
`fat_dir_next(identity, cursor, entry)` until `F_END`. Only `F_OK` supplies a
usable entry; `F_END` is not an I/O error. Other errors may leave partial cursor
or entry state. Reopen the cursor after diagnosing the failure.

Names are UTF-16. ASCII case folds; non-ASCII code units compare exactly. Default
SFN decoding uses CP437; another 256-word OEM mapping can be supplied at mount.
Do not assume host-locale Unicode case folding or normalization.

## 6. Add writes through a transactional provider

A writable FAT32 mount needs a stronger contract than a device's write command.
**Do not connect `SectorOps.write` directly to firmware `WriteBlocks` or a kernel
disk write and implement `end` as a no-op.** An operation can fail after staging
several data, FAT, directory, and FSInfo sectors. The library depends on rollback
restoring all changes made since its successful `begin`.

Implement or port a format-neutral overlay with these rules:

| Operation | Required effect |
| --- | --- |
| `read(lba,out)` | Return the newest staged version, or read the backend if absent |
| `begin()` | Establish one savepoint; failure leaves none active |
| `write(lba,in)` | Copy a whole sector before return; preserve the pre-savepoint version |
| `end(0)` | Restore the previous overlay; no allocation or fallible I/O |
| `end(nonzero)` | Keep the new overlay; no physical commit; cannot fail |
| Explicit OS commit | Write selected staged sectors, then request backend durability |
| Explicit discard | Drop staged changes, then invalidate attached FAT32 caches |

The supplied `buffer.asm` demonstrates versioned sector pages. Porting it means
replacing host allocation/release calls and keeping its savepoint, rollback, and
failure semantics. Allocation belongs in operations that can return `F_MEMORY`,
not in the void `end` callback. Its linear version lookup and commit scans suit
bounded editing sessions; assess them before adopting the design for sustained
kernel workloads.

Public mutations own their `begin`/`end` pair. Do not surround `fat_create` and
`fat_write` with a separate provider `begin`: the resulting nesting returns
`F_BUSY`. You may retain several accepted operations in one overlay before an
explicit OS commit, but that does not make them one public filesystem operation.

A useful sequence for an existing file is:

```text
lookup current entry
fat_write(identity, entry, transfer)       -> staged bytes and refreshed entry
fat_set_info(identity, entry, stamp)       -> staged metadata and refreshed entry
OS commit overlay + backend durability    -> report durable completion
```

If metadata staging fails, the earlier accepted write can remain pending.
Choose whether to commit that earlier operation or discard the whole overlay.
One rollback does not undo earlier accepted operations. Similarly, create
followed by write is not a single atomic create-with-data operation.

### Snapshot rules are part of your handle design

Every completed mutation transaction, including rollback, advances the identity
generation. The successful mutation refreshes its supplied entry where the API
promises that; other entries and cursors become stale. A failed mutation leaves
caller bytes unchanged but may invalidate their generation. Reacquire after
failure. A zero-length write validates without starting a transaction.

`FatEntry` is a snapshot, not a durable POSIX-like open-file handle. Do not update
its generation manually. Your VFS needs a refresh policy and serialization for
name changes, slot reuse, and deletion. There is no library-provided stable inode
number or open-unlinked-file lifetime. `fat_remove` frees the chain on acceptance.

### Durable completion and failure

`F_OK` from a mutation means **accepted into the sector provider**. It does not
mean durable on the device. FAT32 never calls `SectorOps.flush`.

The supplied overlay's commit is unordered and not crash-atomic. A partially
completed physical commit cannot be undone by the logical rollback mechanism.
On write/flush failure, poison the session, retain diagnostics, reject retries,
and establish the actual on-media state before a new writable session. A journal
or stronger commit policy is separate work; changing allocators does not add it.

Allow only one owner of a writable volume, including other mounts in your own
OS. Serializing one identity does not make two independently cached identities
coherent. Local chain validation does not detect all cross-linked ownership.

## 7. Plan the UEFI-to-kernel transition

Finish firmware-backed filesystem work and release or preserve its allocations
before obtaining the final memory map. Call `ExitBootServices` with the current
map key. If it fails because the key changed, follow the specified memory-map
retry procedure; do not return to disk or console activity after the first exit
attempt. Firmware can already be partly shut down. Block I/O also has TPL limits;
keep this loader integration at application TPL.
[UEFI boot-services rules](https://uefi.org/specs/UEFI/2.10_A/07_Services_Boot_Services.html#efi-boot-services-exitbootservices).

For this library, a practical handoff sequence is:

1. Stop new filesystem requests and wait for active calls to finish.
2. Commit and flush, or deliberately discard, any accepted overlay while its
   backend is still available. Never hand off an unresolved transaction.
3. Keep loaded kernel/module data in memory your kernel will preserve. Record
   device/partition identity and geometry as values, not firmware handles.
4. Mark firmware provider sessions offline before beginning the final exit
   sequence. Do not use their mounted identities again.
5. In the kernel, initialize your controller/block driver and independently
   identify the same partition. Check logical block geometry again.
6. Allocate a kernel `SectorOps`, context, and zeroed `FatIdentity`; mount again.
   Re-resolve names and open fresh cursors. Retire every loader snapshot.

Fresh mount is a deliberate integration policy: it avoids retaining firmware
pointers and cached bytes while ownership, addressing, and device transport
change. `fat_invalidate` only drops caches and advances generation; it does not
reload BPB geometry or turn a firmware provider into a kernel driver.

If you change virtual mappings, ensure all code, constants, stack, data pointers,
and callback targets are valid at their new addresses. Controller DMA address
translation, pinning, cache maintenance, completion waits, and timeouts belong
to the kernel provider. The FAT32 callback must still complete synchronously
before returning, even if the underlying driver uses interrupts or DMA.

There is no `fat_unmount` export. Your mount manager drains callers, resolves
pending staging, retires snapshots, and releases the provider/state. Do not
equate zeroing `FatIdentity` with committing or discarding an overlay.

## 8. Map errors and metadata deliberately

| Result | Typical integration response |
| --- | --- |
| `F_END` | Normal directory exhaustion |
| `F_NOTFOUND`, `F_EXISTS`, `F_NAME` | Report pathname/create error |
| `F_STALE` | Reacquire under the mount lock; do not patch snapshot fields |
| `F_BUSY` | Find nesting/concurrent use; fix ordering |
| `F_FORMAT`, `F_CORRUPT` | Reject the operation/mount; retain diagnostics |
| `F_IO` | Consult provider's native status and operation/LBA context |
| `F_READONLY` | Check file attribute and provider capabilities |
| `F_NOSPACE`, `F_MEMORY` | Distinguish exhausted clusters from staging allocation |
| `F_RANGE`, `F_ARGUMENT`, `F_NOTEMPTY` | Return the specific caller/operation failure |

The file-size limit is `0xFFFFFFFF` bytes, even though transfer offsets are
64-bit. Growth and gaps are zero-filled. New timestamps are zero; writes do not
consult a clock. Supply packed local-civil-time fields through `FatStamp` and
decide your OS's timezone/unspecified-date policy. The library checks creation
fractions and structural attribute restrictions, not full calendar validity.

Supported rename is within one parent; there is no cross-directory move.
Removal requires an empty directory. There is no formatter, partition manager,
volume-wide ownership map, repair engine, security-descriptor model, or implicit
concurrency policy in the core. These boundaries matter when mapping it to a VFS.

## 9. Bring up the integration in observable stages

1. **Read-only provider:** log selected device, base, count, block size, alignment,
   native status, and failing LBA. Read sector zero and confirm that the provider
   starts at the intended volume before mounting.
2. **Mount and directories:** compare geometry, enumerate root, resolve a nested
   path, and check `F_END` separately from errors.
3. **File bytes:** compare complete files with an independent reader/hash oracle;
   cover empty files, sector/cluster boundaries, fragmented chains, and EOF.
4. **ABI faults:** overwrite volatile registers in callbacks, verify nonvolatile
   preservation and call-stack alignment, and exercise error returns.
5. **Staged mutations:** inject failure at each staging write/allocation; require
   unchanged caller metadata and restoration of the previous overlay.
6. **Physical commit:** use a disposable image/device; reopen with fresh state and
   compare bytes, metadata, and chains. Exercise write/flush failure policy.
7. **Handoff:** make an attempted firmware-provider call after handoff fail in
   your mount manager; demonstrate kernel access through the new provider.

The repository's `tests/test.c`, `tests/abi.asm`, and host USB harnesses provide
examples and independent oracles. Existing evidence covers the FAT32 core with
synthetic providers and Win32 media access. It does **not** establish that a new
UEFI adapter, controller driver, or kernel allocator works.

`reader.asm` was assembled with the installed fasm2 policy and its call setup
and unwind data inspected. [test.c](test.c) checks it through a host mock:
five-argument firmware dispatch, 64-bit LBA/status, aligned bounce with unaligned
caller output, mount and cross-sector reads, EOF, bounds, read-only behavior,
and offline/media-change errors. Run it with `build.cmd examples-test`.

This is host verification, not a UEFI boot test. Discovery, allocation, exclusive
ownership, and the actual handoff remain the OS developer's responsibility.
