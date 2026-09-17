; All filesystem knowledge is compiled into this one library object.
include 'common/policy.g'
include 'fat32.inc'

; Public interface reference. All entries use Win64 and return F_* in EAX.
; Arguments below are in RCX, RDX, R8, R9 order (32-bit values use EDX/R8D/R9D).
; Types, ownership, and full contracts: fat32.inc / shared.inc / fat32.h.
; Each function remains independently selectable through its COMDAT section.
; Public functions use COMDAT ANY for consumer link-order replacements.
; Replacements preserve this ABI; see docs/LINKING.md. Private helpers stay unique.

; Volume geometry and FAT access.
; identity*, SectorOps*, OEM map or 0, FatWorkspace*
public fat_mount
; identity* in-out -> F_OK; drop caches/snapshots, no I/O.
public fat_invalidate
; identity*, uint32 FAT index, uint32_t* out
public fat_get
; Advanced mutation hooks: identity*, uint32 FAT index, uint32 low-28-bit value.
; Existing transaction only; outer operation owns rollback and shared publication.
public fat_put
public fat_put_checked
; identity*, uint32_t* out; exact free count in the current selected FAT view.
public fat_count_free
; identity*, uint32 first or 0, uint32_t[2]* out {count,last}; validate full chain.
public fat_chain

; Default shared objects and independent handles. See docs/SHARED.md.
; volume*, identity*, FatObject* pool, uint32 capacity
public fat_volume_init
; volume*; release owner when no caller references remain.
public fat_volume_close
; volume*, uint32 access, handle* out
public fat_root
; parent handle*, UTF-16 component*, uint32 access, handle* out
public fat_open
; parent handle*, FatCreate*, handle* out
public fat_new
; handle*; release its owned reference.
public fat_close
; handle*, FatRecord* out; canonical metadata, no I/O.
public fat_handle_info
; handle*, FatStamp* out; decoded timestamps/attributes, no I/O.
public fat_handle_get_stamp
; handle*, uint64 position
public fat_seek
; handle*, FatTransfer* in-out; explicit offset, independent position unchanged.
public fat_read_at
public fat_write_at
; handle*, FatBuffer* in-out; advance this handle's position by completed bytes.
public fat_read_next
public fat_write_next
; handle*, uint64 byte size
public fat_handle_resize
; handle*, const FatStamp*
public fat_handle_set_info
; handle*, new UTF-16 component*
public fat_handle_rename
; parent handle*, UTF-16 component*
public fat_unlink
; directory handle*, FatIterator* out
public fat_iter_open
; FatIterator* in-out, FatEntry* out -> F_OK/F_END/error
public fat_iter_next
; FatIterator*; release its owned reference.
public fat_iter_close
; volume*, const FatCall*; optional non-spinning gate held through callbacks.
public fat_call_locked

; Optional reusable maps and coalesced reads. See docs/STREAMING.md / stream.inc.
; handle*, FatStream* out, const FatStreamWorkspace*
public fat_stream_open
; FatStream*; release its owned reference, not caller workspace.
public fat_stream_close
; FatStream*, FatTransfer* in-out
public fat_stream_read
; FatStream*, FatTransfer* in-out, const FatRangeOps*
public fat_stream_read_range
; FatStream*, uint64 logical sector index, uint64_t* LBA out; no I/O, F_END at EOF.
public fat_stream_sector

; Optional diagnostics and checked read wrapper. See docs/CHECKING.md / check.inc.
; FatView* zeroed/closed out, source identity*, uint32 FAT copy, const FatWorkspace*.
; Read-only diagnostic view; source stays quiescent until close. No I/O.
public fat_view_open
; FatView* zeroed/closed out, const SectorOps*, const FatBootSource*, workspace*.
; Read-only candidate BPB geometry; no primary substitution after opening.
public fat_view_boot
; FatView*; retire view handles/reports and release source lease at caller.
public fat_view_close
; identity*, const FatSalvageRequest*, FatSalvage* out; bounded candidate prefix.
public fat_salvage_plan
; const FatSalvage*, FatTransfer* in-out; read mapped candidates, never repair.
public fat_salvage_read
; identity*, uint32 first, uint32 budget, FatCheck* out
public fat_check_chain
; handle*, uint32 budget, FatCheck* out
public fat_check_file
; directory handle*, uint32 slot budget, FatCheck* out; immediate scoped evidence.
public fat_check_directory
; directory handle*, const FatNameCheck*, FatCheck* out; structure and lookup-name uniqueness.
public fat_check_names
; identity*, const FatOwnershipCheck*, FatOwnershipReport* out; optional volume scan.
public fat_check_ownership
; handle*, const FatCheck* -> stored status or F_STALE; no I/O.
public fat_check_fresh
; identity*, FatCheck* out
public fat_check_reserved
public fat_check_backup
; identity*, const FatFatRange*, FatCheck* out
public fat_check_mirrors
; handle*, FatTransfer* in-out, const FatCheckedRead*
public fat_read_checked
; Same request; read first, diagnose F_CORRUPT/F_IO once, never retry.
public fat_read_adaptive
; Optional caller-selected session policy. See docs/POLICY.md; no base dispatch.
; FatPolicy* zeroed/closed, FatVolume*, FatCheck* ledger, uint32 capacity.
public fat_policy_init
; FatPolicy*, const FatCheck* immediate evidence; no I/O.
public fat_policy_note
; FatPolicy*, FatPolicyCall*; one action, optional one diagnosis, no retry.
public fat_policy_call
; FatPolicy*; release diagnostic object pins and retire wrapper, no I/O.
public fat_policy_close

; Optional ordered staging and physical commit. See docs/ORDERING.md / order.inc.
; FatOrder*, const SectorOps* backend, const FatOrderWorkspace*
public fat_order_init
; FatOrder*, identity*; discard pending changes before physical commit.
public fat_order_discard
; FatOrder*, identity*, FatCommitReport* out
public fat_order_commit
public fat_order_commit_verified
; FatOrderRange* context, FatRangeRequest* in-out; coherent range callback.
public fat_order_read_range

; Optional direct-backend volume formatting, no partition/device selection.
; const SectorOps*, const FatFormatOptions*, FatFormatPlan* out; no I/O.
public fat_format_plan
; const SectorOps*, const FatFormatOptions*, const FatWorkspace*, FatFormatReport* out.
public fat_format
public fat_format_verified

; Snapshot compatibility interface. Do not mix its writers with live handles.
; identity*, uint32 first directory cluster, cursor* out
public fat_dir_open
; identity*, cursor* in-out, entry* out -> F_OK/F_END/error
public fat_dir_next
; identity*, uint32 parent, UTF-16 component*, entry* out
public fat_lookup
; identity*, const entry*, transfer* in-out
public fat_read
; identity*, entry* in-out, transfer* in-out
public fat_write
; identity*, entry* in-out, uint64 byte size
public fat_resize
; identity*, entry* in-out, const stamp*
public fat_set_info
; identity*, uint32 parent, const request*, entry* out
public fat_create
; identity*, const entry* to delete
public fat_remove
; identity*, entry* in-out, new UTF-16 component*
public fat_rename

include 'fat/disk.inc'
include 'fat/volume.inc'
include 'fat/directory.inc'
include 'fat/file.inc'
include 'fat/create.inc'
include 'fat/snapshot.inc'
include 'fat/shared.inc'
include 'fat/check.inc'
include 'fat/view.inc'
include 'fat/boot-view.inc'
include 'fat/salvage.inc'
include 'fat/check-directory.inc'
include 'fat/check-names.inc'
include 'fat/check-ownership.inc'
include 'fat/order.inc'
include 'fat/stream.inc'
include 'fat/order-range.inc'
include 'fat/space.inc'
include 'fat/adaptive.inc'
include 'fat/policy.inc'

include 'fat/put-checked.inc'

include 'fat/format.inc'
