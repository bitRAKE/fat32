; Optional volume formatter. Provider LBAs start at the new BPB. No partitioning.
struct FatFormatOptions
    ?cluster_bytes dd ?         ; explicit power of two, sector_bytes..262144, at most 128 sectors
    ?serial dd ?
    ?hidden_sectors dd ?        ; BPB metadata only, never an I/O offset
    ?reserved dd ?              ; zero
    ?label db 11 dup ?          ; padded ASCII label, or all zero/all spaces
    ?reserved_label db 5 dup ?  ; zero
ends
struct FatFormatPlan
    ?sector_bytes dd ?
    ?cluster_sectors dd ?
    ?cluster_bytes dd ?
    ?total_sectors dd ?
    ?fat_start dd ?
    ?fat_sectors dd ?
    ?data_start dd ?
    ?cluster_count dd ?
    ?root_cluster dd ?
    ?fat_count dd ?
    ?fsinfo dd ?
    ?backup dd ?
    ?writes dq ?                ; exact successful-run backend write count
    ?workspace_bytes dd ?       ; ordinary scratch; verified requires twice this
    ?reserved dd ?
ends
FF_ADMIT := 0
FF_INVALIDATE := 1
FF_RESERVED := 2
FF_FATS := 3
FF_ROOT := 4
FF_BACKUP := 5
FF_PRIMARY := 6
struct FatFormatReport
    ?status dd ?
    ?effect dd ?                ; FE_NONE / FE_UNCERTAIN / FE_COMMITTED
    ?phase dd ?
    ?operation dd ?             ; FI_READ / FI_WRITE / FI_FLUSH / FI_VERIFY
    ?lba dq ?                   ; UINT64_MAX when no sector applies
    ?writes dq ?                ; attempted callbacks
    ?flushes dq ?
    ?completed_writes dq ?      ; successful callback returns, not durability
    ?completed_flushes dq ?
    ?reads dq ?                 ; attempted verification reads
ends
assert sizeof.FatFormatOptions = 32
assert sizeof.FatFormatPlan = 64
assert sizeof.FatFormatReport = 64
