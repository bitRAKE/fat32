; UEFI loader example API. Read README.md before publishing a context.
; Same Win64 ABI as fat32.inc; EAX=F_* (not EFI_STATUS), all pointers borrowed.
include '../../fat32.inc'
if ~ definite GUIDE_EFI_READER_INC
GUIDE_EFI_READER_INC := 1

; Caller-owned, serialized state; distinct from identity/ops/file buffers.
; This is our layout, not an EFI_BLOCK_IO_PROTOCOL or EFI_BLOCK_IO_MEDIA overlay.
; The loader validates geometry/extent/alignment and keeps firmware usable.
struct GuideEfiRead
	?protocol       dq ?     ; live EFI_BLOCK_IO_PROTOCOL*
	?read_blocks    dq ?     ; its ReadBlocks callback
	?base_lba       dq ?     ; 0 for partition handle; otherwise selected base
	?sectors        dq ?     ; nonzero selected count; complete extent validated
	?bounce         dq ?     ; >= block_bytes, IoAlign-aligned, exclusive storage
	?last_status    dq ?     ; last full EFI_STATUS; initialize to 0
	?media_id       dd ?     ; snapshot for this mount; never silently replace
	?block_bytes    dd ?     ; 512, 1024, 2048, or 4096; equals provider sector size
	?online         dd ?     ; 1 while usable; 0 on failure/before firmware exit
	?reserved       dd ?     ; 0
ends
assert sizeof.GuideEfiRead = 64

; guide_efi_read: RCX=GuideEfiRead*, RDX=uint64 volume LBA, R8=sector output.
; Output unchanged on error; F_RANGE outside extent/overflow, F_IO offline or
; firmware failure. Failure records EFI_STATUS and makes context offline.
; Retire the whole mount after F_IO: cached library reads may bypass callbacks.

; guide_mount_readonly: RCX=zeroed unused FatIdentity*, RDX=validated context*,
; R8=SectorOps* out. Distinct objects, all live throughout mount. Populates ops,
; selects CP437, then returns fat_mount status; failure leaves identity unmounted.
; Does not discover/validate/allocate context resources. No write callbacks.

; guide_read_root_range: RCX=mounted identity*, RDX=UTF-16 root component,
; R8=FatTransfer* in-out (data/offset/length initialized). Output done=0 on lookup
; failure, otherwise read count, including a prefix on read error. No output entry
; retained. Ordinary file required; no path parsing, image validation, or execute.
end if
