; Default managed interface. See docs/SHARED.md for ownership and serialization.
; All functions use Win64, return F_* in EAX, and allocate no memory.
FH_READ := 1
FH_WRITE := 2

struct FatObject
	?record FatRecord
	?incarnation dq ?
	?file_version dq ?
	?chain_version dq ?
	?directory_version dq ?
	?references dd ?
	?live dd ?
	?parent dq ?
ends

struct FatVolume
	?identity dq ?
	?objects dq ?
	?capacity dd ?
	?gate dd ?
	?generation dq ?
	?epoch dq ?
	?next_id dq ?
ends

; Zero before acquisition. Do not copy a live handle; use fat_open/fat_root.
; A close retires its reference. Caller storage must outlive every operation.
struct FatHandle
	?volume dq ?
	?object dq ?
	?epoch dq ?
	?incarnation dq ?
	?position dq ?
	?access dd ?
	?reserved dd ?
ends

struct FatBuffer
	?data dq ?
	?length dd ?
	?done dd ?
ends

struct FatIterator
	?handle FatHandle
	?version dq ?
	?cursor FatCursor
ends

; Caller-built invocation for the optional non-spinning gate. No global table
; in the library roots other features. Target must operate on this volume.
struct FatCall
	?target dq ?
	?arg1 dq ?
	?arg2 dq ?
	?arg3 dq ?
	?arg4 dq ?
ends

assert sizeof.FatObject = 128
assert sizeof.FatVolume = 48
assert sizeof.FatHandle = 48
assert sizeof.FatIterator = 632
; fat_handle_get_stamp(handle*, FatStamp* out) decodes canonical metadata without
; I/O; file/directory supported, output unchanged if the handle is stale.
