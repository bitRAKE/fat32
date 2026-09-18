; FAT32 library ABI. Every public entry uses the Windows x64 calling convention.
; No ambient register state, allocation, OS handles, clock, or imports in fat32.obj.
; All LBAs are relative to the start of the volume, never the physical disk.
; New consumers use the shared interface in shared.inc / docs/SHARED.md. The entry
; snapshot functions documented below are compatibility operations, not the
; managed handle contract. Do not mix their writers with live shared handles.
;
; CALL CONTRACT (all fat_* exports; private f_* helpers document exceptions):
;   RCX, RDX, R8, R9 carry arguments 1..4; use EDX/R8D for uint32_t values.
;   EAX = F_* status. Other volatile registers/flags are unspecified on return.
;   Preserve Win64 nonvolatiles; caller provides 32-byte home space, aligned RSP,
;   and a clear direction flag. Mount retains ops/context/OEM/workspace data;
;   the shared interface also retains its caller-owned owner/pool/handle state.
;   Required pointers must address their complete declared object/buffer; no
;   null-pointer probing is performed. Mount's OEM pointer and zero-length
;   FatTransfer.data may be null; structure/name/output pointers may not.
;   Inputs/outputs must not overlap each other or identity/cache/provider state.
;   The caller owns all storage. Keep identity.ops, context, and OEM map alive
;   through unmount/replacement; serialize all use of one identity/provider.
;   The backing volume must stay stable against external writers while mounted.
;   Serialization here cannot exclude another filesystem driver. After external
;   edits stop, invalidate/reacquire entries; remount if geometry/media changed.
;   Except mount/invalidate, calls require a successfully mounted identity.
;   An entry/cursor must come from this identity at its current generation.
;   Do not edit bookkeeping, raw entry bytes, or cached geometry directly.
;
; SNAPSHOT TRANSACTIONS: compatibility mutations use begin/write/end; no flush.
; A zero-length write only validates; all other accepted mutations transact.
; Acceptance and rollback both invalidate other snapshots.
; A failed mutation preserves its caller's entry/output and prior staged data,
; but may make that unchanged snapshot stale: reacquire it after any failure.
; A successful write/resize/set_info/rename refreshes only its supplied entry.
; create publishes a new entry; remove leaves its deleted input snapshot stale.
; No global ownership map: a structurally valid but cross-linked volume must
; not be mutated. Power-loss atomicity belongs to the provider, not this API.
;
; PUBLIC ENTRY POINTS (pointer arrows describe access, not ownership):
;
; fat_mount: RCX=FatIdentity* out/in-out, RDX=const SectorOps*,
;            R8=const uint16_t OEM[256] or 0 (CP437), R9=const FatWorkspace*.
;   Workspace data holds at least three provider sectors and outlives the mount;
;   descriptor itself is borrowed only for this call. Reserved must be zero.
;   Zero identity before first use; do not remount during a transaction. Reads
;   primary BPB only. F_OK publishes geometry; failure leaves magic=0 and all
;   snapshots unusable. Provider extent begins at the FAT32 boot sector.
;
; fat_invalidate: RCX=FatIdentity* in-out; always F_OK, no I/O.
;   Clear FAT/directory cache keys, increment generation, reset allocation hints.
;   Use after external sector edits/discard. Does not reload boot geometry;
;   remount when geometry changes. Not valid during an operation/callback.
;
; fat_get: RCX=FatIdentity*, EDX=cluster (0..cluster_count+1), R8=uint32_t* out.
;   F_OK stores the masked 28-bit FAT value, including reserved FAT[0]/FAT[1].
;   A free/bad/EOC value is data here; use fat_chain to validate a chain.
;   Output is unchanged on error.
;
; fat_chain: RCX=FatIdentity*, EDX=first cluster or 0, R8=uint32_t[2]* out.
;   F_OK stores {count,last}; first=0 means an empty chain {0,0}. Nonzero chains
;   must contain valid data IDs and terminate at EOC without cycles. Output is
;   zeroed before traversal and remains {0,0} on error.
;
; fat_count_free: RCX=FatIdentity*, RDX=uint32_t* out. Exact current selected
;   FAT-view count, no FSInfo trust or cached promise. Unchanged output on error.
;
; fat_dir_open: RCX=FatIdentity*, EDX=first directory cluster (not 0),
;               R8=FatCursor* out. Use identity.root_cluster for the root.
;   Check the first cluster ID, without I/O; initialize only on F_OK. Unchanged
;   on error. A numeric first cluster does not itself prove directory ownership.
;
; fat_dir_next: RCX=FatIdentity*, RDX=FatCursor* in-out, R8=FatEntry* out.
;   F_OK yields one live non-label entry, including dot/dot-dot if present.
;   F_END means exhausted; other errors are distinct. Entry is usable only on
;   F_OK; cursor/output may be partially changed on failure. Reopen after error.
;
; fat_lookup: RCX=FatIdentity*, EDX=parent directory cluster,
;             R8=const uint16_t* NUL-terminated component, R9=FatEntry* out.
;   Match LFN or short alias; ASCII folds case, other UTF-16 matches exactly.
;   No path traversal/normalization. F_NOTFOUND if absent. Output may contain
;   intermediate enumeration results on failure; use only on F_OK.
;
; fat_read: RCX=FatIdentity*, RDX=const FatEntry* ordinary file,
;           R8=FatTransfer* in-out (data writable; offset/length unchanged).
;   Validate snapshot/full chain; clamp at EOF. done=0 at/past EOF, otherwise
;   completed bytes, including a prefix on I/O failure. Does not edit metadata.
;
; fat_write: RCX=FatIdentity*, RDX=FatEntry* in-out ordinary file,
;            R8=FatTransfer* in-out (data readable; offset/length unchanged).
;   offset+length must fit 0xFFFFFFFF. Nonzero write extends/zero-fills gaps and
;   sets archive; readonly attribute rejects it. F_OK: done=length and refreshed
;   entry; error: done=0 and input entry unchanged. Zero length validates but
;   does not grow, set archive, or start a transaction; data is not accessed.
;
; fat_resize: RCX=FatIdentity*, RDX=FatEntry* in-out ordinary file,
;             R8=uint64_t byte size, at most 0xFFFFFFFF.
;   Extend with zeros or free trailing clusters; set archive, refresh entry.
;   Readonly attribute rejects it. Equal size still performs a transaction.
;
; fat_set_info: RCX=FatIdentity*, RDX=FatEntry* in-out,
;               R8=const FatStamp* (file or directory, including readonly).
;   May clear readonly; cannot change directory/volume/reserved attribute bits.
;   Creation fraction must be 0..199; caller validates other civil-time fields.
;   No implicit time update. F_OK refreshes the entry; failure preserves it.
;
; fat_create: RCX=FatIdentity*, EDX=parent directory cluster,
;             R8=const FatCreate*, R9=FatEntry* out.
;   New component: 1..255 well-formed UTF-16 units, no forbidden separators,
;   control characters, trailing dot/space, '.' or '..'. Creates empty file or
;   directory with checked LFN/SFN, zero timestamps, and dot records as needed.
;   Output is assigned only on F_OK; existing LFN/alias gives F_EXISTS.
;
; fat_rename: RCX=FatIdentity*, RDX=FatEntry* in-out,
;             R8=const uint16_t* new NUL-terminated component (create rules).
;   Same parent only; case-only rename allowed. Preserve data chain/metadata;
;   clear NT case bits because LFN supplies case. Reject dot records/readonly.
;   On F_OK refresh entry; on failure preserve it and prior staged sectors.
;
; fat_remove: RCX=FatIdentity*, RDX=const FatEntry* file or empty directory.
;   Delete associated valid LFN/SFN records and free the chain. F_NOTEMPTY for
;   a directory with non-dot children; reject readonly, dot, root/self links.
;   Input memory is unchanged; after success the snapshot is invalid/stale.
if ~ definite FAT32_INC
FAT32_INC := 1

F_OK          := 0
F_END         := 1
F_IO          := 2
F_FORMAT      := 3
F_CORRUPT     := 4
F_RANGE       := 5
F_READONLY    := 6
F_NOSPACE     := 7
F_NOTFOUND    := 8
F_EXISTS      := 9
F_NAME        := 10
F_STALE       := 11
F_BUSY        := 12
F_MEMORY      := 13
F_NOTEMPTY    := 14
F_ARGUMENT    := 15
F_LIMIT       := 16           ; optional diagnostic stopped at caller's budget
F_ATTENTION   := 17           ; dirty/error history, not proof of structural damage
F_VERIFY      := 18           ; optional backend readback disagreed with written data

F_MASK        := 0FFFFFFFh
F_EOC         := 0FFFFFF8h
F_LAST        := 0FFFFFFFh
F_MAGIC       := 32335446h
F_MAX_SECTOR  := 4096
F_MAX_CLUSTER := 262144

; read/write: RCX=context, RDX=uint64 LBA, R8=sector buffer, EAX=F_* status.
; read buffer is writable; write buffer is readable. Exactly sector_bytes on
; F_OK. Buffers need no device alignment and may alias identity scratch/cache;
; callbacks must consume/copy them before returning, never retain their address.
; LBA must be < sectors. Failed read output is unspecified by this interface.
; begin: RCX=context -> EAX=status. Failure must leave no active transaction.
; end: RCX=context, EDX=accept (0 rollback, nonzero accept) -> VOID, cannot fail.
; A successful begin isolates all writes until end; end(0) restores them.
; No recursion into the same identity/provider while a callback is executing.
; flush: RCX=context -> EAX=status; backend durability, never called by FAT32.
; read must be present; write/begin/end may be null for a read-only identity.
; A writable FAT32 provider must implement all three transaction callbacks.
; The backend below SectorBuffer instead writes media directly; it need not
; implement begin/end. SectorBuffer supplies the transaction to FAT32.
struct SectorOps
	?context        dq ?
	?read           dq ?
	?write          dq ?
	?begin          dq ?
	?end            dq ?
	?flush          dq ?
	?sectors        dq ?        ; bounded extent; sectors [0,sectors) exist
	?sector_bytes   dd ?        ; power of two: 512, 1024, 2048, or 4096
	?reserved       dd ?        ; 0 for base, or sizeof.FatOrderedOps (see order.inc)
ends

; Three sector-sized regions: FAT cache, directory cache, operation scratch.
; Mount borrows this descriptor for the call and retains data, not descriptor.
struct FatWorkspace
	?data dq ?
	?bytes dd ?
	?reserved dd ?
ends

; Geometry and cache ownership; three sector buffers are caller-sized and
; supplied at mount. Only the library edits these fields. Zero before first
; mount. One identity/shared owner per provider, serialized multiple consumers.
struct FatIdentity
	?ops            dq ?
	?generation     dq ?
	?fat_lba        dq ?
	?dir_lba        dq ?
	?oem            dq ?        ; optional caller-owned 256-entry UTF-16 map
	?magic          dd ?
	?sector_bytes   dd ?
	?cluster_sectors dd ?
	?cluster_bytes  dd ?
	?total_sectors  dd ?
	?fat_start      dd ?
	?fat_sectors    dd ?
	?data_start     dd ?
	?cluster_count  dd ?
	?root_cluster   dd ?
	?fat_count      dd ?
	?active_fat     dd ?
	?mirrored       dd ?
	?fsinfo         dd ?
	?backup         dd ?
	?next_free      dd ?        ; bounded FAT search cursor; retained on acceptance
	?free_hint      dd ?        ; advisory count, unknown after mutation
	?serial         dd ?
	?transaction    dd ?
	?completions    dd ?        ; private transaction counter for snapshot wrappers
	?fat            dq ?
	?directory      dq ?
	?scratch        dq ?        ; primary boot sector uses scratch during mount
	?shared         dq ?        ; attached FatVolume, or zero; library-owned
ends

; An entry is a snapshot, including all 32 on-disk SFN bytes and a checked
; UTF-16 name. generation and raw bytes protect writes from stale snapshots.
; Names have at most 255 UTF-16 code units plus NUL. raw preserves OEM bytes.
struct FatEntry
	?generation     dq ?
	?sector         dq ?       ; relative LBA containing the short-name record
	?index          dq ?       ; SFN slot index in the parent directory
	?parent         dd ?       ; first cluster of containing directory
	?offset         dd ?       ; SFN byte offset in sector, multiple of 32
	?cluster        dd ?       ; first data cluster; 0 allowed for an empty file
	?size           dd ?       ; exact on-disk byte length (not allocated bytes)
	?lfn_count      dd ?       ; validated LFN records belonging to this SFN
	?name_length    dd ?       ; UTF-16 code units excluding trailing NUL
	?raw            db 32 dup ?
	?name           dw 256 dup ?
ends

; Compact metadata prefix used by the shared owner. Names are enumeration
; results, not permanent per-handle state. Field offsets match FatEntry.
struct FatRecord
	?generation     dq ?
	?sector         dq ?
	?index          dq ?
	?parent         dd ?
	?offset         dd ?
	?cluster        dd ?
	?size           dd ?
	?lfn_count      dd ?
	?name_length    dd ?
	?raw            db 32 dup ?
ends
assert sizeof.FatRecord = FatEntry.name
assert FatRecord.raw = FatEntry.raw

; Cursor persists across sector/cluster boundaries, including LFN fragments.
struct FatCursor
	?generation     dq ?
	?index          dq ?
	?cluster        dd ?
	?parent         dd ?
	?sector_in      dd ?
	?offset         dd ?
	?steps          dd ?
	?ended          dd ?
	?lfn_next       dd ?
	?lfn_count      dd ?
	?checksum       dd ?
	?reserved       dd ?
	?name           dw 260 dup ?
ends

; Read/write request avoids stack arguments and makes partial reads explicit.
; write is atomic in the sector buffer; done remains zero on failure.
struct FatTransfer
	?data           dq ?       ; readable for write, writable for read; length bytes
	?offset         dq ?       ; starting byte offset in file, zero based
	?length         dd ?       ; requested bytes, input only
	?done           dd ?       ; completed bytes, output; caller need not initialize
ends

; Packed FAT timestamps, supplied by caller in local civil time. No implicit
; host clock or timezone conversion. Raw fields can always be round-tripped.
struct FatStamp
	?create_time    dw ?
	?create_date    dw ?
	?access_date    dw ?
	?write_time     dw ?
	?write_date     dw ?
	?create_tenth   db ?
	?attributes     db ?
ends

struct FatCreate
	?name           dq ?       ; borrowed NUL-terminated UTF-16 component
	?directory      dd ?       ; zero = file, one = directory
	?reserved       dd ?       ; caller sets zero
ends

assert sizeof.SectorOps = 64
assert FatIdentity.fat = 120
assert sizeof.FatIdentity = 152
assert sizeof.FatEntry = 592
assert sizeof.FatCursor = 576
include 'shared.inc'
include 'check.inc'
include 'order.inc'
include 'stream.inc'
include 'formatting.inc'
include 'view.inc'
include 'salvage.inc'
include 'policy.inc'
end if
