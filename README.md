# fat32

An x86-64 FAT32 library written in fasmg/fasm2 assembly, for UEFI loaders and
modern operating systems. It uses the Win64 calling convention, MS64 NEWCOFF
objects, explicit state structures, and NMAKE.

The filesystem library has **no Win32 imports or allocator dependency**. All
FAT32 geometry, FAT chains, directories, file data, and metadata belong to
`fat32.lib`. The caller supplies a sector provider and owns the `FatIdentity`
that caches volume, FAT, and directory information.

```text
loader / kernel / application
             |
         fat32.lib
             |
         SectorOps
             |
optional sector staging
             |
UEFI Block I/O / kernel driver / Win32 adapter
```

## Features

- 512–4096-byte sectors and clusters through **64 KiB**.
- File and directory enumeration, creation, same-parent rename, and deletion.
- Arbitrary file reads, overwrite, extension, truncation, and zero-filled gaps.
- UTF-16 long names across sector/cluster boundaries; OEM short-name decoding.
- Attributes and packed FAT creation, access, and modification timestamps.
- FAT mirroring, active-FAT selection, reserved-bit preservation, and bounded
  chain validation. FSInfo remains advisory.
- Atomic in-memory mutation savepoints and explicit sector commits.
- Documented register, ownership, lifetime, and error contracts.

Start with [examples/uefi](examples/uefi/README.md) for OS integration,
[API.md](API.md) for interfaces, and [DEVELOPING.md](DEVELOPING.md) for internals.

## Build

Install the x64 Visual Studio C++ tools, fasm2, and the generated Win32 projection
from win32json. By default, the makefile expects sibling `../fasm2` and
`../win32json` directories. Override `FASM2_ROOT` and `WIN32JSON_ROOT` in the
environment or as NMAKE arguments when using a different layout. LLVM tools for
`verify` come from PATH or `LLVM_BIN`.

```cmd
build.cmd all test examples examples-test usbcheck.exe repocheck.exe
build.cmd verify
```

`build.cmd` uses an existing developer environment or discovers Visual Studio
through `vswhere`. An x64 developer prompt can also run NMAKE directly.

| Output | Purpose |
| --- | --- |
| `fat32.lib` | Filesystem algorithms and caller-owned identity |
| `sector.lib` | Optional Win32 transport and generic staging buffer |
| `fatdemo.exe` | Minimal assembly client; set its sample device in `example.asm` |
| `tests.exe` | Synthetic media, fault injection, ABI, and sector-file tests |
| `ueficheck.exe` | UEFI adapter host mock |
| `usbcheck.exe` | Raw/native interoperability and capture replay |
| `repocheck.exe` | Git-blob pair-write/delete workload |

## Physical tests

The runners require an explicitly selected FAT32 test volume labeled `TESTING`,
its volume serial, and its USB device serial. Supply those values locally; they
are not repository defaults. The runners do not format the device. `X:` below
is a placeholder for the selected test volume.

```powershell
.\tests\run-64k.ps1 -Drive $TestDrive -ExpectedSerial $VolumeSerial `
  -ExpectedDeviceSerial $UsbSerial -EvidenceDirectory build\64k

.\tests\run-repo.ps1 -Drive $TestDrive -ExpectedSerial $VolumeSerial `
  -ExpectedDeviceSerial $UsbSerial -Revision HEAD -EvidenceDirectory build\repo-test
```

`run-64k.ps1` requires 64 KiB clusters. It checks Windows-created boundary files,
raw-library mutations, and directory growth across a cluster boundary.
`tests/inspect-64k.py` independently verifies FAT chains and hashes while holding
a volume lock, and saves a sparse sector capture for replay:

```powershell
py -3 tests\inspect-64k.py build\64k\native-fixtures.json $VolumeSerial build\capture
Expand-Archive build\capture\sectors.zip build\replay
.\usbcheck.exe X: --capture build\replay
```

`run-repo.ps1` requires 512-byte sectors/clusters. It exports exact Git blobs,
ordered by size, and performs repeated passes over missing files:

```text
write A, write B, delete A; write C, write D, delete C; ...
repeat over missing files until all remain; keep an unpaired final file
```

Every write/delete is committed. Fresh raw-library and native Windows reads must
match each original Git blob ID. The runner records chains, requires measured
fragmentation, and checks file-cluster ownership. Fixtures remain in a unique
directory on the test volume.

Generated logs, manifests, and sector captures are **local artifacts**. Keep them
under ignored `build/` or `tests/evidence/`, or outside the repository. They can
contain device identifiers and captured file contents. [VALIDATION.md](VALIDATION.md)
contains the publication-safe coverage summary.

## Contracts and scope

64 KiB clusters are part of the supported geometry for this modern x86-64 target.
See [CLUSTER64.md](CLUSTER64.md) for implementation boundaries and test coverage.

The backing volume must remain stable while an identity caches its metadata.
Serialize library calls and exclude external writers, or use an immutable
snapshot. The read-only Win32 adapter does not acquire a volume lock.

Physical commit is not a journal and is not atomic across a crash or power loss.
A failed commit poisons the staging buffer. Global cross-link repair, orphan
recovery, formatting, partition discovery, and cross-parent moves are outside
the API. Lookup folds ASCII case; other UTF-16 units compare exactly. Short names
use CP437 by default, with a caller-provided OEM table available at mount.

## References

- [Microsoft FAT format specification](https://www.pcjs.org/documents/papers/microsoft/MS_FAT_OVERVIEW_103-2000-12-06.pdf)
- [UEFI media access protocols](https://uefi.org/specs/UEFI/2.11/13_Protocols_Media_Access.html)
- [Win32 volume handles](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- [Volume locking](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_lock_volume)
