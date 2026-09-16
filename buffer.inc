; Generic sector staging; this layer knows no filesystem structures.
;
; All functions use Win64 (arguments RCX,RDX,R8,R9; EAX=F_* except sb_end void).
; Required pointers must be valid, nonoverlapping objects. Serialize each buffer
; and its backend; backend callbacks may not reenter the same buffer. The backend
; operations table/context must outlive the initialized buffer. Discard/close
; never commit staged sectors, and no buffer/device ownership is transferred.
;
; sb_init: RCX=SectorBuffer* out, RDX=const SectorOps* backend (read required).
;   Clears the entire buffer. Only initialize fresh or discarded storage; an
;   existing live page chain would be leaked. Backend cannot alias the buffer.
;   F_OK installs transactional ops, copies geometry, sets limit=131072 versions.
;   F_ARGUMENT on invalid geometry/read callback; no allocation during init.
;
; sb_read: RCX=SectorBuffer*, RDX=uint64 LBA, R8=void* out (sector_bytes).
;   Read newest pending sector or backend; works both inside/outside a savepoint.
;   F_RANGE outside extent; F_IO if poisoned; backend errors propagate. Output
;   on backend failure follows the backend's contract (may be partly modified).
;
; sb_begin: RCX=SectorBuffer* -> F_OK creates one savepoint; F_BUSY if active,
;   F_IO if poisoned. No nesting. No allocation/media I/O; unchanged on failure.
; sb_write: RCX=SectorBuffer*, RDX=uint64 LBA, R8=const void* sector_bytes.
;   Requires active savepoint, copies input before return. Failure preserves the
;   prior overlay. F_BUSY without begin, F_RANGE outside extent, F_MEMORY on
;   version limit/allocation failure. Buffer can stage over a read-only backend.
; sb_end: RCX=SectorBuffer*, EDX=accept (0 rollback, nonzero accept), VOID.
;   Exactly one end follows successful begin; never fails/allocates/writes media.
;   Accepted pages remain pending; rollback frees only versions since begin.
; sb_discard: RCX=SectorBuffer* -> F_OK; free all pages/cancel savepoint, no I/O.
;   Does not clear poison. Invalidate attached FAT identities after discard.
; sb_commit: RCX=SectorBuffer* -> write latest sector versions then backend flush.
;   No active savepoint (F_BUSY); backend write+flush required (F_READONLY).
;   Success frees pending pages; even an empty commit calls flush. Write/flush
;   failure preserves pages and poisons buffer (future read/begin/commit F_IO).
;   No commit retry/undo/order guarantee. Discard and establish media state before
;   reinitializing after failure. No durability beyond what backend guarantees.
;
; win_open: RCX=WinVolume* out, RDX=const UTF-16* raw volume path (e.g. \\.\X:),
;           R8D=write_mode (0 read only, nonzero read/write with exclusive lock).
;   Zero storage before first use; never reopen a live handle in place. Path is
;   borrowed only for this call. F_OK publishes sector callbacks/extent. Failed
;   open cleans up and returns F_IO; volume.error records Win32 status. No retry.
;   Writable mode holds the lock through explicit flush and close/dismount.
; win_close: RCX=WinVolume* previously opened/failed-open/closed -> F_OK or F_IO.
;   Dismount locked volume, close handle, free bounce buffer; never commit/flush.
;   Attempts cleanup after failure and records first dismount/close error.
; win_read/win_write: RCX=WinVolume*, RDX=uint64 LBA, R8=one sector buffer.
;   read destination writable; write source readable. No alignment requirement
;   on caller memory (adapter uses aligned bounce storage). Requires successful
;   open; write additionally requires its exclusive lock. Read copies output only
;   after a complete sector was read. Short/error writes can affect media.
; win_flush: RCX=WinVolume* successfully opened writable -> explicit device flush.
;   All I/O callbacks return F_* and store native errors in volume.error on I/O
;   failure (not cleared by later success). LBA range failures do not set it.
;   Single-caller use is essential because seek+I/O share the handle file pointer.
include 'fat32.inc'
if ~ definite BUFFER_INC
BUFFER_INC := 1
struct SectorBuffer
	?ops       SectorOps
	?backend   dq ?
	?head      dq ?
	?mark      dq ?
	?pages     dq ?
	?limit     dq ?
	?active    dd ?
	?poisoned  dd ?
ends
struct SectorPage
	?next      dq ?
	?lba       dq ?
	?data      db F_MAX_SECTOR dup ?
ends
; Raw device is a volume handle, not a physical-disk handle. io storage must
; outlive its SectorBuffer. Write mode retains an exclusive lock until close.
struct WinVolume
	?ops       SectorOps
	?handle    dq ?
	?locked    dd ?
	?error     dd ?
	?bounce    dq ?
ends
end if
