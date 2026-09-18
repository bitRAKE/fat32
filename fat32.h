/* Public x86-64 Win64 C17 ABI. Production implementation: fat32.asm/fat32.inc.
   Shared handles are the default; snapshot, checking, order and stream exports
   are optional COMDAT features. These declarations allocate or import nothing.
   Compile callers for the Windows x64 ABI, including freestanding EFI/kernel
   callers. All storage is caller-owned; hold one volume lease through callbacks. */
#ifndef FAT32_PUBLIC_H
#define FAT32_PUBLIC_H
#include <stdint.h>
#include <stddef.h>
enum { F_OK, F_END, F_IO, F_FORMAT, F_CORRUPT, F_RANGE, F_READONLY,
       F_NOSPACE, F_NOTFOUND, F_EXISTS, F_NAME, F_STALE, F_BUSY,
       F_MEMORY, F_NOTEMPTY, F_ARGUMENT, F_LIMIT, F_ATTENTION, F_VERIFY };
typedef struct SectorOps {
    void *context;
    int (*read)(void *, uint64_t, void *);
    int (*write)(void *, uint64_t, const void *);
    int (*begin)(void *);
    void (*end)(void *, int);
    int (*flush)(void *);
    uint64_t sectors;
    uint32_t sector_bytes, reserved;
} SectorOps;
typedef struct FatIdentity {
    SectorOps *ops;
    uint64_t generation, fat_lba, dir_lba;
    const uint16_t *oem;
    uint32_t magic, sector_bytes, cluster_sectors, cluster_bytes;
    uint32_t total_sectors, fat_start, fat_sectors, data_start, cluster_count;
    uint32_t root_cluster, fat_count, active_fat, mirrored, fsinfo, backup;
    /* Library-owned search cursor survives accepted mutations; it is not a
       promise of free space or the on-disk FSInfo next-free hint. */
    uint32_t next_free, free_hint, serial, transaction, completions;
    uint8_t *fat, *directory, *scratch;
    void *shared;
} FatIdentity;
typedef struct FatWorkspace { void *data; uint32_t bytes, reserved; } FatWorkspace;
/* Optional diagnostic interpretation. Zero before first use. Keep the source
   provider quiescent and all storage disjoint throughout the view's lifetime. */
typedef struct FatView { FatIdentity identity; SectorOps provider; } FatView;
typedef struct FatBootSource { uint32_t sector,reserved; const uint16_t *oem; } FatBootSource;
int fat_view_open(FatView *,const FatIdentity *source,uint32_t fat_copy,const FatWorkspace *);
/* Explicit backup BPB in its own reserved area. Failed admission leaves view
   unchanged; after success the read-only provider uses authentic source LBAs. */
int fat_view_boot(FatView *,const SectorOps *,const FatBootSource *,const FatWorkspace *);
int fat_view_close(FatView *);
_Static_assert(sizeof(FatView)==216,"FAT view ABI");
_Static_assert(sizeof(FatBootSource)==16,"boot source ABI");
typedef struct FatEntry {
    uint64_t generation, sector, index;
    uint32_t parent, offset, cluster, size, lfn_count, name_length;
    uint8_t raw[32];
    uint16_t name[256];
} FatEntry;
typedef struct FatCursor {
    uint64_t generation, index;
    uint32_t cluster, parent, sector_in, offset, steps, ended;
    uint32_t lfn_next, lfn_count, checksum, reserved;
    uint16_t name[260];
} FatCursor;
typedef struct FatTransfer {
    void *data;
    uint64_t offset;
    uint32_t length, done;
} FatTransfer;
typedef struct FatStamp {
    uint16_t create_time, create_date, access_date, write_time, write_date;
    uint8_t create_tenth, attributes;
} FatStamp;
typedef struct FatCreate { const uint16_t *name; uint32_t directory, reserved; } FatCreate;
/* C mirror of shared.inc; production ABI is assembly and SHARED.md. */
enum { FH_READ=1, FH_WRITE=2 };
typedef struct FatRecord {
    uint64_t generation, sector, index;
    uint32_t parent, offset, cluster, size, lfn_count, name_length;
    uint8_t raw[32];
} FatRecord;
typedef struct FatObject {
    FatRecord record;
    uint64_t incarnation, file_version, chain_version, directory_version;
    uint32_t references, live;
    struct FatObject *parent;
} FatObject;
typedef struct FatVolume {
    FatIdentity *identity;
    FatObject *objects;
    uint32_t capacity, gate;
    uint64_t generation, epoch, next_id;
} FatVolume;
typedef struct FatHandle {
    FatVolume *volume;
    FatObject *object;
    uint64_t epoch, incarnation, position;
    uint32_t access, reserved;
} FatHandle;
typedef struct FatBuffer { void *data; uint32_t length, done; } FatBuffer;
typedef struct FatIterator { FatHandle handle; uint64_t version; FatCursor cursor; } FatIterator;
typedef struct FatCall { uintptr_t target, args[4]; } FatCall;

int fat_volume_init(FatVolume *, FatIdentity *, FatObject *pool, uint32_t count);
int fat_volume_close(FatVolume *);
int fat_root(FatVolume *, uint32_t access, FatHandle *out);
int fat_open(FatHandle *parent, const uint16_t *name, uint32_t access, FatHandle *out);
int fat_new(FatHandle *parent, const FatCreate *, FatHandle *out);
int fat_close(FatHandle *);
int fat_handle_info(FatHandle *, FatRecord *out);
int fat_handle_get_stamp(FatHandle *, FatStamp *out);
int fat_seek(FatHandle *, uint64_t position);
int fat_read_at(FatHandle *, FatTransfer *);
int fat_write_at(FatHandle *, FatTransfer *);
int fat_read_next(FatHandle *, FatBuffer *);
int fat_write_next(FatHandle *, FatBuffer *);
int fat_handle_resize(FatHandle *, uint64_t size);
int fat_handle_set_info(FatHandle *, const FatStamp *);
int fat_handle_rename(FatHandle *, const uint16_t *name);
int fat_unlink(FatHandle *parent, const uint16_t *name);
int fat_iter_open(FatHandle *directory, FatIterator *out);
int fat_iter_next(FatIterator *, FatEntry *out);
int fat_iter_close(FatIterator *);
int fat_call_locked(FatVolume *, const FatCall *);
_Static_assert(sizeof(FatRecord)==80,"record ABI");
_Static_assert(sizeof(FatObject)==128,"object ABI");
_Static_assert(sizeof(FatVolume)==48,"volume ABI");
_Static_assert(sizeof(FatHandle)==48,"handle ABI");
_Static_assert(sizeof(FatIterator)==632,"iterator ABI");

typedef struct FatStreamWorkspace { uint32_t *map; uint32_t capacity,reserved; } FatStreamWorkspace;
typedef struct FatStream { FatHandle handle; uint64_t version; uint32_t *map; uint32_t count,capacity; } FatStream;
typedef struct FatRangeRequest { uint64_t lba; void *data; uint32_t count,done; } FatRangeRequest;
typedef struct FatRangeOps {
    void *context; int (*read)(void *,FatRangeRequest *);
    uint32_t maximum,reserved;
} FatRangeOps;
int fat_stream_open(FatHandle *,FatStream *,const FatStreamWorkspace *);
int fat_stream_read(FatStream *,FatTransfer *);
int fat_stream_read_range(FatStream *,FatTransfer *,const FatRangeOps *);
/* Map a sector intersecting the logical file; the final sector may be partial.
   No I/O. F_END at/past EOF; output unchanged on F_END or error. The returned
   LBA is volume-relative and must be read through the same logical provider. */
int fat_stream_sector(FatStream *,uint64_t sector_index,uint64_t *out_lba);
int fat_stream_close(FatStream *);
_Static_assert(sizeof(FatStreamWorkspace)==16,"stream workspace ABI");
_Static_assert(sizeof(FatStream)==72,"stream ABI");
_Static_assert(sizeof(FatRangeRequest)==24,"range request ABI");
_Static_assert(sizeof(FatRangeOps)==24,"range provider ABI");

enum { FC_CHAIN=1, FC_FILE, FC_RESERVED, FC_BACKUP, FC_MIRRORS, FC_DIRECTORY, FC_NAMES, FC_OWNERSHIP };
enum { FC_LINK=1, FC_CYCLE, FC_SHORT, FC_RESERVED_VALUE, FC_STATUS,
       FC_BACKUP_LOCATION, FC_BACKUP_SIGNATURE, FC_BACKUP_MISMATCH,
       FC_MIRROR_MISMATCH, FC_IO, FC_BUDGET, FC_DIRENT, FC_LFN, FC_DOT, FC_EXTENSION,
       FC_DUPLICATE, FC_WORKSPACE, FC_CROSSLINK, FC_ORPHAN, FC_SLOT_BUDGET };
enum { FC_EXTRA=1, FC_ABSENT=2, FC_INACTIVE=4, FC_DIRTY=8, FC_HARD_ERROR=16, FC_ENDMARKER=32, FC_ORPHANS=64 };
typedef struct FatCheck {
    FatIdentity *identity; uint64_t generation;
    FatVolume *volume; uint64_t epoch;
    FatObject *object; uint64_t incarnation,version;
    uint32_t scope,status,issue,flags;
    uint64_t sector;
    uint32_t cluster,observed,expected,examined,first,count,last,budget;
} FatCheck;
typedef struct FatFatRange { uint32_t first,count; } FatFatRange;
typedef struct FatCheckedRead { FatCheck *report; uint32_t budget,reserved; } FatCheckedRead;
typedef struct FatNameCheck {
    FatEntry *entries;
    uint32_t capacity,slot_budget,pair_budget,reserved;
} FatNameCheck;
int fat_check_chain(FatIdentity *,uint32_t first,uint32_t budget,FatCheck *);
int fat_check_file(FatHandle *,uint32_t budget,FatCheck *);
/* Logical slots through first unused entry or EOC; no recursive/ownership scan.
   Budget counts 32-byte slots, including deleted/LFN/end-marker slots.
   Immediate evidence only: fat_check_fresh does not reuse directory reports. */
int fat_check_directory(FatHandle *,uint32_t slot_budget,FatCheck *);
/* Directory structure, then uniqueness under lookup's LFN/alias comparison.
   Two bounded slot passes, caller workspace, bounded entry-pair comparisons.
   Structural failures keep FC_DIRECTORY; the name pass reports FC_NAMES. */
int fat_check_names(FatHandle *,const FatNameCheck *,FatCheck *);
int fat_check_fresh(FatHandle *,const FatCheck *);
int fat_check_reserved(FatIdentity *,FatCheck *);
int fat_check_backup(FatIdentity *,FatCheck *);
int fat_check_mirrors(FatIdentity *,const FatFatRange *,FatCheck *);
int fat_read_checked(FatHandle *,FatTransfer *,const FatCheckedRead *);
/* Read first; on F_CORRUPT/F_IO diagnose once within budget, without retry.
   Preserves the read's status/done. report.scope==0 means no diagnosis.
   Current known F_CORRUPT file evidence refuses the read with done=0. */
int fat_read_adaptive(FatHandle *,FatTransfer *,const FatCheckedRead *);
_Static_assert(sizeof(FatCheck)==112,"FatCheck ABI");
_Static_assert(sizeof(FatCheckedRead)==16,"FatCheckedRead ABI");
_Static_assert(sizeof(FatNameCheck)==24,"FatNameCheck ABI");

/* Read-only candidate extents. Keep the source/provider quiescent under the
   same exclusive maintenance lease from planning through every salvage read.
   Reports, extents and library storage are disjoint; no ownership is inferred. */
enum { FC_SALVAGE=9, FC_PAIR_BUDGET=21 };
typedef struct FatExtent { uint32_t first,count; } FatExtent;
typedef struct FatSalvageRequest {
    FatExtent *extents;
    uint32_t capacity,first,bytes,fat_budget,pair_budget,reserved;
} FatSalvageRequest;
typedef struct FatSalvage {
    FatCheck check; FatExtent *extents;
    uint32_t capacity,extent_count,bytes,available,pairs,pair_budget;
} FatSalvage;
int fat_salvage_plan(FatIdentity *,const FatSalvageRequest *,FatSalvage *);
/* F_END with done > 0 when crossing the available prefix; no zero-fill. The
   original plan status/issue remains unchanged even after a successful read. */
int fat_salvage_read(const FatSalvage *,FatTransfer *);
_Static_assert(sizeof(FatExtent)==8,"salvage extent ABI");
_Static_assert(sizeof(FatSalvageRequest)==32,"salvage request ABI");
_Static_assert(sizeof(FatSalvage)==144,"salvage ABI");

enum { FP_READ=1, FP_WRITE=2, FP_INSPECT=3, FP_NO_WRITE=1, FP_NO_READ=2 };
typedef struct FatPolicy {
    FatVolume *volume; FatIdentity *identity; uint64_t generation,epoch;
    FatCheck *reports; uint32_t capacity,count,flags,active;
    uintptr_t cause_target; FatCheck cause;
} FatPolicy;
typedef struct FatPolicyCall {
    FatCall action; FatHandle *subject; uint32_t kind,invoked;
    const FatCall *diagnosis; FatCheck *report; uint32_t diagnosed,reserved;
} FatPolicyCall;
int fat_policy_init(FatPolicy *,FatVolume *,FatCheck *ledger,uint32_t capacity);
int fat_policy_note(FatPolicy *,const FatCheck *);
int fat_policy_call(FatPolicy *,FatPolicyCall *);
int fat_policy_close(FatPolicy *);
_Static_assert(sizeof(FatPolicy)==176,"policy ABI");
_Static_assert(sizeof(FatPolicyCall)==80,"policy call ABI");
typedef struct FatOwner { uint32_t sector,offset; } FatOwner;
typedef struct FatDirectoryTask { uint32_t first,parent; FatOwner owner; } FatDirectoryTask;
typedef struct FatOwnershipCheck {
    FatOwner *owners; FatDirectoryTask *directories;
    uint32_t owner_capacity,directory_capacity,fat_budget,slot_budget;
    uint64_t reserved;
} FatOwnershipCheck;
typedef struct FatOwnershipReport {
    FatCheck check;
    uint64_t owner_sector,other_sector;
    uint32_t owner_offset,other_offset,slots,directories,files;
    uint32_t free_clusters,bad_clusters,orphan_clusters;
} FatOwnershipReport;
/* Selected-FAT ownership from root, then inventory of unclaimed data clusters.
   Workspace is disposable; reports are immediate, not mutation permissions. */
int fat_check_ownership(FatIdentity *,const FatOwnershipCheck *,FatOwnershipReport *);
_Static_assert(sizeof(FatOwner)==8,"FatOwner ABI");
_Static_assert(sizeof(FatDirectoryTask)==16,"FatDirectoryTask ABI");
_Static_assert(sizeof(FatOwnershipCheck)==40,"FatOwnershipCheck ABI");
_Static_assert(sizeof(FatOwnershipReport)==160,"FatOwnershipReport ABI");

enum { FO_PREPARED=1, FO_PUBLISH, FO_UNPUBLISH, FO_DETACH };
typedef struct FatOrderedOps {
    SectorOps base;
    int (*barrier)(void *,uint32_t reason);
    uint32_t revision,flags;
} FatOrderedOps;
_Static_assert(sizeof(FatOrderedOps)==80,"FatOrderedOps ABI");
typedef struct FatOrderWorkspace { void *data; uint64_t bytes; uint32_t *index,slots,reserved; } FatOrderWorkspace;
typedef struct FatOrder {
    FatOrderedOps ops;
    SectorOps *backend;
    uint8_t *data;
    uint32_t *index;
    uint32_t capacity,stride,slots,used,phase,active,accepted,poisoned,error,error_phase;
    uint64_t error_lba,writes,flushes;
    uint32_t unique,error_operation;
} FatOrder;
enum { FE_NONE, FE_UNCERTAIN, FE_COMMITTED };
enum { FI_READ=1, FI_WRITE, FI_FLUSH, FI_VERIFY };
#define FP_ADMIT UINT32_C(0xFFFFFFFD)
#define FP_DIRTY UINT32_C(0xFFFFFFFE)
#define FP_CLEAN UINT32_C(0xFFFFFFFF)
typedef struct FatCommitReport {
    uint32_t status,effect,phase,operation;
    uint64_t lba,writes,flushes;
} FatCommitReport;
int fat_order_init(FatOrder *,const SectorOps *,const FatOrderWorkspace *);
int fat_order_discard(FatOrder *,FatIdentity *);
int fat_order_commit(FatOrder *,FatIdentity *,FatCommitReport *);
int fat_order_commit_verified(FatOrder *,FatIdentity *,FatCommitReport *);
typedef struct FatOrderRange { FatOrder *order; const FatRangeOps *backend; } FatOrderRange;
int fat_order_read_range(void *,FatRangeRequest *);
_Static_assert(sizeof(FatOrderRange)==16,"order range ABI");
_Static_assert(sizeof(FatOrderWorkspace)==32,"FatOrderWorkspace ABI");
_Static_assert(sizeof(FatOrder)==176,"FatOrder ABI");
_Static_assert(sizeof(FatCommitReport)==40,"FatCommitReport ABI");
/* Win64 ABI; F_* status. Required structure/name pointers
   must be valid and nonoverlapping; callers own storage and serialize access.
   No automatic null checks. See fat32.inc and the interface documents for contracts.
   Use mounted identities and current-generation snapshots. Snapshot mutation
   failures can invalidate their generation; shared handles retain the scoped
   publication/rollback contract documented in SHARED.md. */

/* Zero identity first. Retains sectors/context and optional 256-unit OEM map;
   NULL OEM selects CP437. Failed mount leaves the identity unmounted. */
int fat_mount(FatIdentity *identity, const SectorOps *sectors, const uint16_t *oem, const FatWorkspace *workspace);
/* Drop caches/snapshots after external edits/discard; does not reload BPB. */
int fat_invalidate(FatIdentity *identity);
/* Masked FAT value, including indexes 0/1; out_value unchanged on error. */
int fat_get(FatIdentity *identity, uint32_t cluster, uint32_t *out_value);
/* Advanced hooks within an existing library mutation. The outer operation owns
   transaction completion, allocation hints and canonical metadata publication.
   Neither hook starts/commits a transaction or makes an arbitrary FAT edit safe. */
int fat_put(FatIdentity *identity, uint32_t cluster, uint32_t value);
int fat_put_checked(FatIdentity *identity, uint32_t cluster, uint32_t value);
/* Exact free count of the selected current FAT view; no FSInfo or stored cache.
   Hold the volume lease throughout. Output is unchanged on failure. */
int fat_count_free(FatIdentity *identity, uint32_t *out_count);
/* Full validated chain -> {count,last}; first=0 or failure gives {0,0}. */
int fat_chain(FatIdentity *identity, uint32_t first, uint32_t count_last[2]);
/* first must be a directory data cluster, not zero. Output only on success. */
int fat_dir_open(FatIdentity *identity, uint32_t first, FatCursor *out_cursor);
/* F_END is exhaustion. Entry/cursor may be partial on error; reopen cursor. */
int fat_dir_next(FatIdentity *identity, FatCursor *cursor, FatEntry *out_entry);
/* One UTF-16 component; LFN/SFN matching. Output usable only on F_OK. */
int fat_lookup(FatIdentity *identity, uint32_t parent, const uint16_t *component, FatEntry *out_entry);
/* Ordinary file. data writable; done includes a successful prefix on error. */
int fat_read(FatIdentity *identity, const FatEntry *entry, FatTransfer *transfer);
/* data readable. Staged atomically; failure preserves entry, sets done=0.
   Nonzero success refreshes entry; offset+length must fit 0xFFFFFFFF. */
int fat_write(FatIdentity *identity, FatEntry *entry, FatTransfer *transfer);
/* Size <=0xFFFFFFFF; zero-fill growth/free tail; readonly files rejected. */
int fat_resize(FatIdentity *identity, FatEntry *entry, uint64_t new_size);
/* File/directory; packed caller-validated times, tenth<=199; no structural
   attribute changes. May clear readonly. Publish entry only on success. */
int fat_set_info(FatIdentity *identity, FatEntry *entry, const FatStamp *stamp);
/* request: valid UTF-16 component, directory=0/1, reserved=0. Output unchanged
   on error; new timestamps zero. Mutation acceptance stages data; commit uses the selected provider contract. */
int fat_create(FatIdentity *identity, uint32_t parent, const FatCreate *request, FatEntry *out_entry);
/* Ordinary file or empty directory. Input memory unchanged; success stales it. */
int fat_remove(FatIdentity *identity, const FatEntry *entry);
/* Same parent, valid new component; F_OK refreshes entry; failure preserves it. */
int fat_rename(FatIdentity *identity, FatEntry *entry, const uint16_t *new_component);

/* Optional formatter: direct backend, exclusive extent, no live mount.
   Labels are eleven space-padded printable ASCII bytes (lowercase is folded),
   or eleven zero bytes for no label. Reserved fields must be zero. */
typedef struct FatFormatOptions {
    uint32_t cluster_bytes, serial, hidden_sectors, reserved;
    uint8_t label[11], reserved_label[5];
} FatFormatOptions;
typedef struct FatFormatPlan {
    uint32_t sector_bytes, cluster_sectors, cluster_bytes, total_sectors;
    uint32_t fat_start, fat_sectors, data_start, cluster_count;
    uint32_t root_cluster, fat_count, fsinfo, backup;
    uint64_t writes;
    uint32_t workspace_bytes, reserved;
} FatFormatPlan;
enum { FF_ADMIT, FF_INVALIDATE, FF_RESERVED, FF_FATS, FF_ROOT, FF_BACKUP, FF_PRIMARY };
typedef struct FatFormatReport {
    int32_t status;
    uint32_t effect, phase, operation;
    uint64_t lba, writes, flushes, completed_writes, completed_flushes, reads;
} FatFormatReport;
int fat_format_plan(const SectorOps *, const FatFormatOptions *, FatFormatPlan *);
int fat_format(const SectorOps *, const FatFormatOptions *, const FatWorkspace *, FatFormatReport *);
int fat_format_verified(const SectorOps *, const FatFormatOptions *, const FatWorkspace *, FatFormatReport *);
_Static_assert(sizeof(FatFormatOptions)==32,"format options ABI");
_Static_assert(sizeof(FatFormatPlan)==64,"format plan ABI");
_Static_assert(sizeof(FatFormatReport)==64,"format report ABI");

_Static_assert(sizeof(SectorOps) == 64, "ABI");
_Static_assert(offsetof(FatIdentity, fat) == 120, "ABI");
_Static_assert(sizeof(FatIdentity) == 152, "ABI");
_Static_assert(sizeof(FatEntry) == 592, "ABI");
_Static_assert(sizeof(FatCursor) == 576, "ABI");
#endif
