; FAT32 library ABI. Every public entry uses the Windows x64 calling convention.
; No ambient register state, allocation, handles, clock, or imports in fat32.obj.
; All LBAs are relative to the start of the volume, never the physical disk.
if ~ definite FAT32_H
FAT32_H := 1

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

F_MASK        := 0FFFFFFFh
F_EOC         := 0FFFFFF8h
F_LAST        := 0FFFFFFFh
F_MAGIC       := 32335446h
F_MAX_SECTOR  := 4096

; read/write(context, lba, sector) -> status; exactly sector_bytes on success.
; begin(context) -> status; end(context, accept) -> void, MUST NOT fail.
; A successful begin isolates all writes until end; end(0) restores them.
; flush is a backend-only durability callback. FAT32 never calls it.
struct SectorOps
	?context        dq ?
	?read           dq ?
	?write          dq ?
	?begin          dq ?
	?end            dq ?
	?flush          dq ?
	?sectors        dq ?
	?sector_bytes   dd ?
	?reserved       dd ?
ends

; Format identity owns parsed geometry plus separate volume/FAT/directory
; sector caches. Only the FAT32 library interprets or modifies these fields.
; Zero before first mount. One identity per buffer, one serialized caller.
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
	?next_free      dd ?
	?free_hint      dd ?
	?serial         dd ?
	?transaction    dd ?
	?reserved       dd ?
	?boot           db F_MAX_SECTOR dup ?
	?fat            db F_MAX_SECTOR dup ?
	?directory      db F_MAX_SECTOR dup ?
	?scratch        db F_MAX_SECTOR dup ?
ends

; An entry is a snapshot, including all 32 on-disk SFN bytes and a checked
; UTF-16 name. generation and raw bytes protect writes from stale snapshots.
; Names have at most 255 UTF-16 code units plus NUL. raw preserves OEM bytes.
struct FatEntry
	?generation     dq ?
	?sector         dq ?
	?index          dq ?       ; SFN slot index in the parent directory
	?parent         dd ?
	?offset         dd ?
	?cluster        dd ?
	?size           dd ?
	?lfn_count      dd ?
	?name_length    dd ?
	?raw            db 32 dup ?
	?name           dw 256 dup ?
ends

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
	?data           dq ?
	?offset         dq ?
	?length         dd ?
	?done           dd ?
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
	?name           dq ?
	?directory      dd ?       ; zero = file, one = directory
	?reserved       dd ?
ends

assert sizeof.SectorOps = 64
assert FatIdentity.boot = 120
assert sizeof.FatEntry = 592
assert sizeof.FatCursor = 576
end if
