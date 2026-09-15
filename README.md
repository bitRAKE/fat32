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

`usbcheck X: --write-test` additionally creates an isolated, uniquely named test
directory through the library, commits it, verifies its file through Windows,
then removes it through the library. It first requires a clean read comparison
and a volume named TESTING. Raw write opening must acquire a volume lock and
dismount it. **This write test was not run on the current USB**, because the
read-only inspection found existing filesystem damage. See [VALIDATION.md](VALIDATION.md).

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

The 64 KiB cluster case is an intentional compatibility requirement for the
supplied USB. It exceeds the older specification's conservative 32 KiB guidance.
