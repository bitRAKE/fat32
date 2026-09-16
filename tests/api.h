/* C ABI mirror for the verification harness. Production contracts: ../fat32.inc. */
#ifndef TEST_API_H
#define TEST_API_H
#include <stdint.h>
#include <stddef.h>
enum { F_OK, F_END, F_IO, F_FORMAT, F_CORRUPT, F_RANGE, F_READONLY,
       F_NOSPACE, F_NOTFOUND, F_EXISTS, F_NAME, F_STALE, F_BUSY,
       F_MEMORY, F_NOTEMPTY, F_ARGUMENT };
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
    uint32_t next_free, free_hint, serial, transaction, reserved;
    uint8_t boot[4096], fat[4096], directory[4096], scratch[4096];
} FatIdentity;
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
typedef struct SectorBuffer {
    SectorOps ops;
    SectorOps *backend;
    void *head, *mark;
    uint64_t pages, limit;
    uint32_t active, poisoned;
} SectorBuffer;
typedef struct WinVolume {
    SectorOps ops;
    void *handle;
    uint32_t locked, error;
    void *bounce;
} WinVolume;
/* Win64 ABI; F_* status except void sb_end. Required structure/name pointers
   must be valid and nonoverlapping; callers own storage and serialize access.
   No automatic null checks. See ../fat32.inc and ../buffer.inc for full contracts.
   Use mounted identities/current-generation snapshots; reacquire after any
   mutation failure because rollback may have invalidated the old generation. */

/* Zero identity first. Retains sectors/context and optional 256-unit OEM map;
   NULL OEM selects CP437. Failed mount leaves the identity unmounted. */
int fat_mount(FatIdentity *identity, const SectorOps *sectors, const uint16_t *oem);
/* Drop caches/snapshots after external edits/discard; does not reload BPB. */
int fat_invalidate(FatIdentity *identity);
/* Masked FAT value, including indexes 0/1; out_value unchanged on error. */
int fat_get(FatIdentity *identity, uint32_t cluster, uint32_t *out_value);
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
   on error; new timestamps zero. All mutation data is staged until sb_commit. */
int fat_create(FatIdentity *identity, uint32_t parent, const FatCreate *request, FatEntry *out_entry);
/* Ordinary file or empty directory. Input memory unchanged; success stales it. */
int fat_remove(FatIdentity *identity, const FatEntry *entry);
/* Same parent, valid new component; F_OK refreshes entry; failure preserves it. */
int fat_rename(FatIdentity *identity, FatEntry *entry, const uint16_t *new_component);

/* Fresh/discarded buffer only; backend must outlive it and cannot alias it. */
int sb_init(SectorBuffer *buffer, const SectorOps *backend);
/* Callback context is SectorBuffer*. One sector; output valid on F_OK. */
int sb_read(void *buffer, uint64_t lba, void *out_sector);
/* Active savepoint required; copies one sector, never writes media here. */
int sb_write(void *buffer, uint64_t lba, const void *sector);
int sb_begin(void *buffer);                    /* One savepoint; no nesting. */
void sb_end(void *buffer, int accept);           /* 0 rollback; cannot fail. */
int sb_discard(SectorBuffer *buffer);            /* Free pages, no I/O; keeps poison. */
/* Idle buffer; writes+flushes backend. Failed I/O poisons it; never retry. */
int sb_commit(SectorBuffer *buffer);

/* Zero/freed adapter, borrowed UTF-16 raw volume path; nonzero write_mode locks.
   Failed open cleans up; volume.error records native I/O failure. */
int win_open(WinVolume *volume, const wchar_t *volume_path, int write_mode);
int win_close(WinVolume *volume);               /* Dismount/close/free, no commit. */
/* One complete sector through aligned bounce storage; serialize seek+I/O. */
int win_read(void *volume, uint64_t lba, void *out_sector);
int win_write(void *volume, uint64_t lba, const void *sector); /* Lock required. */
int win_flush(void *volume);                   /* Open writable handle required. */
_Static_assert(sizeof(SectorOps) == 64, "ABI");
_Static_assert(offsetof(FatIdentity, boot) == 120, "ABI");
_Static_assert(sizeof(FatIdentity) == 16504, "ABI");
_Static_assert(sizeof(FatEntry) == 592, "ABI");
_Static_assert(sizeof(FatCursor) == 576, "ABI");
_Static_assert(sizeof(SectorBuffer) == 112, "ABI");
_Static_assert(sizeof(WinVolume) == 88, "ABI");
#endif
