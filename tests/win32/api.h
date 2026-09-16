/* Host verification adapters; filesystem declarations are public. */
#ifndef TEST_API_H
#define TEST_API_H
#include "../../fat32.h"
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
_Static_assert(sizeof(SectorBuffer) == 112, "ABI");
_Static_assert(sizeof(WinVolume) == 88, "ABI");
#endif
