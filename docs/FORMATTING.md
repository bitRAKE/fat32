# Optional FAT32 volume formatting

`fat_format_plan`, `fat_format` and `fat_format_verified` are independently
selectable public functions in `fat32.asm`. Implementation is in
`fat/format.inc`; layouts are in `formatting.inc` and `fat32.h`. No mount,
reader, ordinary writer or diagnostic imports the formatter.

## Scope and geometry

The caller exclusively owns a bounded, volume-relative provider whose LBA zero
is the new BPB. Retire old mounts before calling. Device selection, partition
creation, MBR/GPT, drive letters, firmware entries and boot-program installation
remain in the consumer. `hidden_sectors` is BPB metadata; it never offsets I/O.

Supported sector sizes are 512, 1024, 2048 and 4096 bytes. Cluster sizes are
powers of two, at least one sector, at most 128 sectors and at most 256 KiB:

| Sector bytes | Cluster bytes |
| --- | --- |
| 512 | 512 through 65,536 |
| 1024 | 1,024 through 131,072 |
| 2048 | 2,048 through 262,144 |
| 4096 | 4,096 through 262,144 |

This follows the allocation-size range advertised by current
[Microsoft format](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/format)
and the installed Windows 11 build 26200 tool, with the BPB's byte-sized
sectors-per-cluster constraint. Normal mount/read/write support has the same
limits. The older FAT specification's 32-KiB guidance is not a library limit.
A boot-image consumer may explicitly choose 4-KiB clusters as a conservative
firmware preset; that choice does not restrict other callers.

The provider extent must fit the BPB's 32-bit total-sector count. The plan uses
all of that extent, two mirrored FATs, 32 reserved sectors, FSInfo at 1, backup
BPB at 6, backup FSInfo at 7, and root cluster 2. At least 65,541 data clusters
are required (16 beyond the FAT32 classification boundary); at most 0x0FFFFFEE
are accepted. It computes the smallest FAT sufficient for the resulting data
area. A trailing partial cluster remains outside allocation. Invalid geometry
fails before any callback; another FAT type is never substituted.

## Inputs and planning

`FatFormatOptions` is 32 bytes: `cluster_bytes`, `serial`, `hidden_sectors`,
zero `reserved`, eleven label bytes and five zero reserved bytes. Allocation
size is explicit; no clock, random source, allocator or size heuristic is used.
A label is printable ASCII padded to eleven bytes with spaces; lowercase is
folded to uppercase. FAT-invalid punctuation is rejected. All-zero or all-space
labels mean unlabeled. BPBs then contain `NO NAME    ` and no root label entry;
a named volume has matching BPB and root labels. No OEM interpretation is implied.

```c
FatFormatOptions options = {0};
options.cluster_bytes = 4096; /* caller's boot-volume choice */
options.serial = 0x1234abcd;
options.hidden_sectors = partition_start_lba;
FatFormatPlan plan;
int status = fat_format_plan(&backend, &options, &plan);
```

Planning does no I/O and does not require read/write callbacks. It returns a
64-byte plan containing sector/cluster geometry, extent, FAT/data locations,
cluster count, root/FSInfo/backup constants, exact successful-run write count,
and ordinary workspace bytes. Failure leaves the plan zeroed. `F_NAME` denotes
an invalid label, `F_ARGUMENT` reserved fields, `F_RANGE` a too-large extent and
`F_FORMAT` unsupported geometry or insufficient/excessive data-cluster count.

Execution receives the original options and recomputes the plan. No caller-
edited plan supplies trusted write addresses. The ordinary formatter needs one
sector in `FatWorkspace`; the verified formatter needs two. Scratch, arguments,
report and provider are valid, stable and disjoint through all callbacks.
Address overflow, null scratch, reserved fields and insufficient capacity are
rejected before I/O. Required argument pointers follow the normal public ABI.

## Execution and evidence

```c
FatWorkspace scratch = {buffer, buffer_bytes, 0};
FatFormatReport report;
status = fat_format(&backend, &options, &scratch, &report);
/* Alternate: fat_format_verified(&backend, &options, &scratch, &report). */
```

The backend must have direct `write` and `flush` callbacks and zero
`begin`, `end` and `reserved` fields. Transaction/staging providers are rejected;
formatting is not one giant rollbackable file operation. Ordinary formatting
requires no read callback. The verified alternate additionally needs `read`.
All calls use the same Win64 ABI as the normal sector provider.

Execution writes invalid primary and backup headers, then flushes. It initializes
the remaining reserved area, both complete FATs and the complete root cluster,
then flushes. It publishes and flushes the backup BPB, then the primary BPB.
The initial FAT contains reserved entries and root allocation only; FSInfo has
an exact free count and next-free hint 3. Boot extension sectors 2 and 8 have
the standard signature. Other data clusters are untouched: this is quick
formatting, without a surface scan or secure erase. The inert legacy entry
installs no bootloader; callers populate `EFI/BOOT/BOOTX64.EFI` separately.

Ordinary success makes `data_start + cluster_sectors + 2` write calls and four
flush calls. The verified alternate adds a flush and a backend reread after
each successful write, compares the full sector, and stops on the first error
or mismatch. Its extra code is not retained by ordinary formatting.

`FatFormatReport` is 64 bytes. It reports returned `status`, effect certainty,
phase (`FF_INVALIDATE`, `FF_RESERVED`, `FF_FATS`, `FF_ROOT`, `FF_BACKUP`,
`FF_PRIMARY`), operation (`FI_WRITE`, `FI_FLUSH`, `FI_READ`, `FI_VERIFY`), and LBA.
During flush, LBA identifies the preceding write. Before I/O it is UINT64_MAX
and phase is `FF_ADMIT`. Counts distinguish attempted writes/flushes from
successful callback returns and also count verification read attempts.

`FE_NONE` means no physical write was attempted. The report becomes
`FE_UNCERTAIN` before the first write callback and retains it on any subsequent
failure, including a partially failed first write. Only final successful flush
returns `FE_COMMITTED`. Completed-write counts do not claim durable sectors.
Readback mismatch returns `F_VERIFY`. There is no automatic retry, rollback,
repair, or claim of crash/power-loss atomicity. The caller retains failure
evidence and explicitly decides whether to start a new destructive format.

## Qualification

The host suites cover all 31 valid sector/cluster combinations in both policies,
raw BPB/FAT/root and FSInfo checks, exact I/O counts, label consistency, unchanged
unrelated data, workspace guards, hostile ABI callbacks, geometry boundaries,
rejection without I/O, every callback failure for both policies, half-sector
failed writes and readback mismatch. Shared create/write/read succeeds after
mounting the produced filesystem. Larger clusters also exercise fragmented
chains, shared handles, streams and cross-cluster LFN growth.

`tests/check-format-images.py` materializes real library output as sparse files,
checks it with an independent raw decoder and can run non-repair `fsck.fat`.
All 31 geometries passed fsck.fat 4.2 with unchanged metadata. `format-images.py`
can also populate a boot fixture through the library's shared API. Partition
wrapping and boot/reboot qualification belong to the consumer harness.

Current planning, formatting, and verified-formatting footprints are recorded
in [VALIDATION.md](VALIDATION.md). No imports or mandatory
mount-state growth are added. The optional read-only stub example rejects both
format executors; planning remains available.
