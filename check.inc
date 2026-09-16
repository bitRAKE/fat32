; Optional, read-only structural diagnostics. No core routine references these
; functions. See docs/CHECKING.md for scope, budgets, and report lifetimes.
FC_CHAIN := 1
FC_FILE := 2
FC_RESERVED := 3
FC_BACKUP := 4
FC_MIRRORS := 5
FC_DIRECTORY := 6
FC_NAMES := 7
FC_OWNERSHIP := 8

FC_LINK := 1
FC_CYCLE := 2
FC_SHORT := 3
FC_RESERVED_VALUE := 4
FC_STATUS := 5
FC_BACKUP_LOCATION := 6
FC_BACKUP_SIGNATURE := 7
FC_BACKUP_MISMATCH := 8
FC_MIRROR_MISMATCH := 9
FC_IO := 10
FC_BUDGET := 11
FC_DIRENT := 12
FC_LFN := 13
FC_DOT := 14
FC_EXTENSION := 15
FC_DUPLICATE := 16
FC_WORKSPACE := 17
FC_CROSSLINK := 18
FC_ORPHAN := 19
FC_SLOT_BUDGET := 20

FC_EXTRA := 1               ; chain longer than file size requires
FC_ABSENT := 2              ; backup absent, not compared
FC_INACTIVE := 4            ; FAT mirroring disabled, not compared
FC_DIRTY := 8
FC_HARD_ERROR := 16
FC_ENDMARKER := 32          ; directory scan stopped at first unused entry
FC_ORPHANS := 64            ; complete allocation inventory found unowned clusters

struct FatCheck
	?identity dq ?
	?generation dq ?
	?volume dq ?
	?epoch dq ?
	?object dq ?
	?incarnation dq ?
	?version dq ?              ; chain_version, only for FC_FILE
	?scope dd ?
	?status dd ?
	?issue dd ?
	?flags dd ?
	?sector dq ?               ; evidence LBA; -1 if no sector applies
	?cluster dd ?
	?observed dd ?
	?expected dd ?
	?examined dd ?             ; scope-specific work: FAT entries, slots, or name pairs
	?first dd ?
	?count dd ?                ; chain nodes, or accepted live short directory slots
	?last dd ?                 ; terminal chain node, or last directory cluster read
	?budget dd ?               ; corresponding scope-specific limit; see docs/CHECKING.md
ends

struct FatFatRange
	?first dd ?                ; FAT entry index (including reserved 0/1)
	?count dd ?                ; explicit comparison range, may be zero
ends

struct FatCheckedRead
	?report dq ?               ; zero initially or an existing FatCheck
	?budget dd ?
	?reserved dd ?             ; zero
ends
struct FatNameCheck
	?entries dq ?              ; FatEntry[capacity], disposable operation workspace
	?capacity dd ?             ; includes dot entries, excludes the volume label
	?slot_budget dd ?          ; maximum slots per pass (two passes on success)
	?pair_budget dd ?          ; maximum distinct-entry comparisons, at most four names each
	?reserved dd ?             ; zero
ends
assert sizeof.FatCheck = 112
assert sizeof.FatCheckedRead = 16
assert sizeof.FatNameCheck = 24

; Optional whole-volume ownership workspace. An all-zero owner is unclaimed;
; all-ones identifies the root; other owners identify their physical SFN slot.
struct FatOwner
	?sector dd ?
	?offset dd ?
ends
struct FatDirectoryTask
	?first dd ?
	?parent dd ?
	?owner FatOwner
ends
struct FatOwnershipCheck
	?owners dq ?               ; FatOwner[owner_capacity], indexed by cluster - 2
	?directories dq ?          ; FatDirectoryTask[directory_capacity], breadth-first
	?owner_capacity dd ?       ; at least identity.cluster_count
	?directory_capacity dd ?   ; at least one; includes root and all discovered dirs
	?fat_budget dd ?           ; maximum attempted FAT entry reads, including rewalks
	?slot_budget dd ?          ; maximum physical slots across all directories
	?reserved dq ?             ; zero
ends
struct FatOwnershipReport
	?check FatCheck
	?owner_sector dq ?         ; current SFN location, or -1 for root/no owner
	?other_sector dq ?         ; conflicting owner location, or -1 for root/no owner
	?owner_offset dd ?
	?other_offset dd ?
	?slots dd ?
	?directories dd ?          ; discovered directories, including root
	?files dd ?                ; successfully traversed file entries, including empty
	?free_clusters dd ?
	?bad_clusters dd ?
	?orphan_clusters dd ?
ends
assert sizeof.FatOwner = 8
assert sizeof.FatDirectoryTask = 16
assert sizeof.FatOwnershipCheck = 40
assert sizeof.FatOwnershipReport = 160

; fat_check_chain(identity*, first, budget, report*) -> F_*.
; fat_check_file(handle*, budget, report*) -> F_*; chain and size, no slot reread.
; fat_check_fresh(handle*, report*) -> stored status or F_STALE; no I/O.
; fat_check_reserved(identity*, report*) -> reserved entries/status bits.
; fat_check_backup(identity*, report*) -> primary/backup BPB agreement.
; fat_check_mirrors(identity*, range*, report*) -> masked entries in that range.
; fat_read_checked(handle*, transfer*, FatCheckedRead*) -> check/reuse then read.
; fat_read_adaptive(handle*, transfer*, FatCheckedRead*) -> basic read, then
; bounded file diagnosis on F_CORRUPT/F_IO; original status/done retained.
; fat_check_directory(handle*, maximum 32-byte slots, report*) -> bounded
; logical-directory structure/LFN check, immediate evidence only; no recursion.
; fat_check_names(handle*, const FatNameCheck*, report*) -> first check directory
; structure, then lookup-name uniqueness. Structural errors retain FC_DIRECTORY;
; name-pass results use FC_NAMES. Immediate evidence, never mutation permission.
; fat_check_ownership(identity*, const FatOwnershipCheck*, FatOwnershipReport*)
; -> bounded reachable-allocation and unowned-cluster inventory, selected FAT.
