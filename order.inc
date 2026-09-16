; Optional transaction-order extension. Base SectorOps remains 64 bytes, with
; reserved=0. An extended provider sets reserved=sizeof.FatOrderedOps and
; supplies revision=1, flags=0 and barrier. Readers never inspect this tail.
; barrier(context, reason) -> F_* closes the preceding staged phase. It does
; not write or flush physical media. Failure rolls back the enclosing operation.
; Versions across phase boundaries must survive until fallible physical commit.
FO_PREPARED := 1           ; initialized/reserved storage precedes references
FO_PUBLISH := 2            ; content/name preparation precedes SFN publication
FO_UNPUBLISH := 3          ; smaller/deleted entry precedes releasing its chain
FO_DETACH := 4             ; retained chain's EOC precedes freeing detached tail
struct FatOrderedOps
	?base SectorOps
	?barrier dq ?
	?revision dd ?
	?flags dd ?
ends
assert sizeof.FatOrderedOps = 80

; Optional indexed staging and physical-commit module. All memory is caller
; owned. One accepted operation is committed/discarded before the next begin.
struct FatOrderWorkspace
	?data dq ?
	?bytes dq ?
	?index dq ?              ; slots uint32 entries, initially cleared by init
	?slots dd ?              ; power of two, >=2; unique LBA limit = slots/2
	?reserved dd ?           ; zero
ends
struct FatOrder
	?ops FatOrderedOps
	?backend dq ?
	?data dq ?
	?index dq ?
	?capacity dd ?
	?stride dd ?
	?slots dd ?
	?used dd ?
	?phase dd ?
	?active dd ?             ; 0 idle, 1 staging, 2 committing
	?accepted dd ?
	?poisoned dd ?           ; 0 normal, 1 uncertain physical I/O, 2 known damage/status
	?error dd ?
	?error_phase dd ?
	?error_lba dq ?
	?writes dq ?
	?flushes dq ?
	?unique dd ?
	?error_operation dd ?
ends
struct FatOrderSector
	?lba dq ?
	?phase dd ?
	?reason dd ?
	; sector_bytes payload follows; records are sector_bytes + 16 bytes apart.
ends
FP_ADMIT := 0FFFFFFFDh
FP_DIRTY := 0FFFFFFFEh
FP_CLEAN := 0FFFFFFFFh
FE_NONE := 0
FE_UNCERTAIN := 1
FE_COMMITTED := 2
FI_READ := 1
FI_WRITE := 2
FI_FLUSH := 3
FI_VERIFY := 4
struct FatCommitReport
	?status dd ?
	?effect dd ?
	?phase dd ?
	?operation dd ?
	?lba dq ?
	?writes dq ?             ; attempted backend writes
	?flushes dq ?            ; attempted backend flushes, including verification
ends
assert sizeof.FatOrderWorkspace = 32
assert sizeof.FatOrder = 176
assert sizeof.FatOrderSector = 16
assert sizeof.FatCommitReport = 40

; Optional coherent range adapter. Used as FatRangeOps.context with callback
; fat_order_read_range. Backend range reads must describe order.backend's same
; volume; pending sectors are served from the index before any backend ranges.
struct FatOrderRange
	?order dq ?
	?backend dq ?            ; borrowed FatRangeOps*
ends
assert sizeof.FatOrderRange = 16

; fat_order_init(order*, backend SectorOps*, workspace*) -> F_*; fresh state.
; Mount over &order.ops.base. Initialization does no I/O; staging can read the
; backend but never writes/flushes it. No allocation is hidden in either path.
; fat_order_discard(order*, identity*) -> F_*; no physical I/O, retires handles.
; fat_order_commit(order*, identity*, report*) -> ordered writes/phase flushes.
; fat_order_commit_verified(...) -> additionally flush/read back each write.
