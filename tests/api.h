/* C ABI mirror for the verification harness. Production contracts: ../fat32.h. */
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
int fat_mount(FatIdentity *, SectorOps *, const uint16_t *oem);
int fat_invalidate(FatIdentity *);
int fat_get(FatIdentity *, uint32_t, uint32_t *);
int fat_chain(FatIdentity *, uint32_t, uint32_t count_last[2]);
int fat_dir_open(FatIdentity *, uint32_t, FatCursor *);
int fat_dir_next(FatIdentity *, FatCursor *, FatEntry *);
int fat_lookup(FatIdentity *, uint32_t, const uint16_t *, FatEntry *);
int fat_read(FatIdentity *, const FatEntry *, FatTransfer *);
int fat_write(FatIdentity *, FatEntry *, FatTransfer *);
int fat_resize(FatIdentity *, FatEntry *, uint64_t);
int fat_set_info(FatIdentity *, FatEntry *, const FatStamp *);
int fat_create(FatIdentity *, uint32_t, const FatCreate *, FatEntry *);
int fat_remove(FatIdentity *, const FatEntry *);
int fat_rename(FatIdentity *, FatEntry *, const uint16_t *);
int sb_init(SectorBuffer *, SectorOps *);
int sb_read(void *, uint64_t, void *);
int sb_write(void *, uint64_t, const void *);
int sb_begin(void *);
void sb_end(void *, int);
int sb_discard(SectorBuffer *);
int sb_commit(SectorBuffer *);
int win_open(WinVolume *, const wchar_t *, int);
int win_close(WinVolume *);
int win_read(void *, uint64_t, void *);
int win_write(void *, uint64_t, const void *);
int win_flush(void *);
_Static_assert(sizeof(SectorOps) == 64, "ABI");
_Static_assert(offsetof(FatIdentity, boot) == 120, "ABI");
_Static_assert(sizeof(FatIdentity) == 16504, "ABI");
_Static_assert(sizeof(FatEntry) == 592, "ABI");
_Static_assert(sizeof(FatCursor) == 576, "ABI");
_Static_assert(sizeof(SectorBuffer) == 112, "ABI");
_Static_assert(sizeof(WinVolume) == 88, "ABI");
#endif
