# fat32

A FAT32 library in the assembly style of `hexed`: fasmg/fasm2,
MS64 NEWCOFF objects, explicit state structures, Win64 procedures, and NMAKE.

The filesystem library has **no Win32 imports and no allocator dependency**.
It consumes a transactional sector-buffer interface. The supplied Win32 adapter
and buffer are separate objects in a separate library.

```text
client: file names, metadata, byte ranges
                  |
              fat32.lib
   FAT32 routines + caller-owned FatIdentity
    [boot sector | FAT cache | directory cache]
                  |
               SectorOps
       read / stage / begin / accept-or-rollback
                  |
              sector.lib
     SectorBuffer: Win32-allocated sector versions
                  |
       WinVolume: raw volume sector I/O
                  |
             \\.\X: or another backend
```

## Included

- FAT32 geometry validation, 512–4096-byte sectors, clusters through **64 KiB**.
- Directory enumeration, component lookup, checked UTF-16 long names and OEM
  short names; long names can cross sector and cluster boundaries.
- File reads, overwrite, extension, truncation, and zero-filled gaps.
- File and directory creation, same-directory rename, and empty-directory removal.
- Attributes and all FAT timestamp fields, with the original 32-byte short entry
  retained for exact inspection and preservation.
- FAT mirroring, active-FAT selection, reserved-nibble preservation, bounded
  chain validation, and advisory FSInfo invalidation.
- Atomic **in-memory** operations: a failed mutation restores its staged sectors.
  Explicit buffer commit is the only route to physical writes.

## Build and run

From an x64 Visual Studio developer prompt:

```cmd
nmake
nmake test
nmake usbcheck.exe
nmake verify
fatdemo.exe
usbcheck.exe X:
```

`build.cmd` selects the installed Visual Studio 18 Community x64 tools on this
machine. For example, `build.cmd all test usbcheck.exe` builds everything needed
for the checks. Tool paths are at the top of `makefile`.

Outputs:

| Output | Purpose |
| --- | --- |
| `fat32.lib` | All FAT32 algorithms; no unresolved external dependencies |
| `sector.lib` | Win32 raw-volume adapter and generic sector staging |
| `fatdemo.exe` | CRT-free assembly example; lists X:'s root, read-only |
| `tests.exe` | Synthetic, failure-injection, and Win32 sector-file tests |
| `usbcheck.exe` | Compare raw file reads with normal Windows file reads |
| `repocheck.exe` | Persisted fragmentation workload and raw readback for Git verification |

`usbcheck X: --write-test` additionally creates an isolated, uniquely named test
directory through the library. It checks raw creation, repeated flushes, a patch
across a cluster boundary, zero-filled growth, Unicode rename, packed metadata,
truncation, and deletion. Windows independently verifies committed file bytes and
metadata between phases. It first requires a complete read comparison and a
FAT32 volume named TESTING. Writable opening acquires a volume lock; close
dismounts after explicit commits, while the lock is still held.

`usbcheck X: --flush-test` checks two explicit flushes on one locked handle without
staging sector changes. The earlier **32 KiB** format passed the complete write
test twice, 18 automated suites, and read-only CHKDSK. Logs, hashes, and the
failure that led to the flush-lifetime fix
are recorded in [VALIDATION.md](VALIDATION.md).

### Repository fragmentation test

Build `repocheck.exe` and `usbcheck.exe`, then run the PowerShell 7 driver with
the current volume serial (eight hexadecimal digits) and a new evidence folder:

```powershell
.\build.cmd repocheck.exe usbcheck.exe
.\tests\run-repo.ps1 -Drive X: -ExpectedSerial $VolumeSerial -Revision HEAD `
  -EvidenceDirectory tests\evidence\2026-09-15-512b
```

The driver pins the commit, sorts all regular tracked files by blob size and
path, and exports their exact committed bytes. The loop is:

```text
pass 1: write A, write B, delete A; write C, write D, delete C; ...
pass 2: repeat over missing A, C, E, ...
later:  repeat until none are missing; keep an unpaired final file
```

Each file write/deletion reaches an explicit physical commit. After closing and
reopening the volume, the library records each actual FAT chain and reads each
file back to local storage. Git hashes both that raw readback and a separate
native Windows read; every result must equal its original blob ID. At least one
final file must have multiple noncontiguous cluster extents. File-cluster
ownership is checked across the copied files, and CHKDSK runs before and after.

This test requires the named USB identity and **512-byte sectors/clusters**. It
leaves every file under a unique `FAT32-repo-...` directory with the repository
hierarchy preserved. It does not format the volume. The driver accepts up to
4096 ordinary files of at most 16 MiB each and currently uses SHA-1 Git repos.

Git's [blob IDs](https://git-scm.com/book/en/v2/Git-Internals-Git-Objects) already
identify committed file content. The test uses `git cat-file blob` and
[`git hash-object --no-filters`](https://git-scm.com/docs/git-hash-object), so CRLF
checkout conversion cannot hide changed bytes or produce false mismatches.
Untracked build products and `.git` internals are outside the pinned tree.

The 512-byte-cluster run at commit `bd5370b` passed: 45/45 files match their Git
blob IDs through raw and native reads, after six passes, 86 writes, and 41
deletions. Six final files have fragmented chains (up to 12 extents). The files
remain under `X:\FAT32-repo-session`. See
[the validation record](VALIDATION.md) for the pinned corpus and evidence.

## Contracts and scope

Start with [API.md](API.md) for integration and [DEVELOPING.md](DEVELOPING.md)
for cache, transaction, ABI, and durability rules.

Physical commit is not a journal and is not atomic across a crash or power loss.
An I/O failure during commit poisons the buffer. This library is not a volume
repair tool: global cross-link detection, orphan recovery, formatting, partition
discovery, and moving entries between directories are outside its API.

Lookup folds ASCII case; other UTF-16 code units compare exactly. The default
short-name decoding is CP437, with a caller-provided OEM table available at mount.

## Format references

- [Microsoft FAT specification, version 1.03](https://www.pcjs.org/documents/papers/microsoft/MS_FAT_OVERVIEW_103-2000-12-06.pdf)
- [Win32 volume handles](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- [Unbuffered file I/O](https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering)
- [Volume locking](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_lock_volume)

The 64 KiB cluster case remains an intentional compatibility requirement from
the original USB format. It exceeds the older specification's conservative
32 KiB guidance. Physical read/write coverage now includes both 32 KiB and
512-byte clusters.
