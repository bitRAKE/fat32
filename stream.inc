; Optional reusable read map. One owned shared handle pins its canonical object;
; chain versions scope map validity. Workspace stays caller-owned and immutable
; between open and close. A map covers the file's logical length, not excess tail.
struct FatStreamWorkspace
	?map dq ?
	?capacity dd ?           ; uint32 cluster IDs
	?reserved dd ?           ; zero
ends
struct FatStream
	?handle FatHandle
	?version dq ?
	?map dq ?
	?count dd ?
	?capacity dd ?
ends
; Optional coherent provider range read. Callback(context, request*) returns
; F_* and sets done to completed prefix sectors (including on failure). It must
; accept arbitrary output alignment and preserve all other request fields.
; It reads the SAME logical view as the mounted SectorOps, including staging.
struct FatRangeRequest
	?lba dq ?
	?data dq ?
	?count dd ?
	?done dd ?
ends
struct FatRangeOps
	?context dq ?
	?read dq ?
	?maximum dd ?            ; maximum sectors per callback, nonzero
	?reserved dd ?           ; zero
ends
assert sizeof.FatStreamWorkspace = 16
assert sizeof.FatStream = 72
assert sizeof.FatRangeRequest = 24
assert sizeof.FatRangeOps = 24
; fat_stream_open(handle*, zero/closed stream*, workspace*) -> F_*
; fat_stream_read(stream*, transfer*) -> reads through mounted sector provider
; fat_stream_read_range(stream*, transfer*, range_ops*) -> coalesced full sectors
; fat_stream_close(stream*) -> releases owned reference; never frees workspace
; fat_stream_sector(stream*, uint64 index, uint64* out LBA) -> F_*; no I/O.
; F_END beyond the last sector intersecting the file; output unchanged on error.
; Last sector can include bytes beyond EOF. Use the same logical provider view.
