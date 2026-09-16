# Windows test harness

This harness links the FAT32 library to a Win32 raw-sector provider and a
format-neutral staging buffer. It supplies synthetic fixtures, ABI checks,
linker tests, regular-file observations, and explicit USB interoperability tests.
The adapter and `sector.lib` are test infrastructure; the product is `fat32.lib`.

## Build and run

Install the x64 Visual Studio C++ tools and the fasm2 toolchain described in
[BUILD.md](../../docs/BUILD.md). Windows adapter assembly also needs the generated
Win32 projection from win32json. Python and LLVM are used by the linker matrices.

Run from the repository root:

```cmd
tests\win32\build.cmd all test examples-test feature-test verify
```

`build.cmd` uses the active developer environment or discovers Visual Studio
through `vswhere`. It runs the harness makefile from the repository root.
`FASM2_ROOT` and `WIN32JSON_ROOT` default to sibling `../fasm2` and `../win32json`;
override them in the environment or as NMAKE arguments. LLVM comes from
`LLVM_BIN` or PATH. The core-only makefile needs neither win32json nor this harness.

| Target | Result |
| --- | --- |
| `all` | Library, host tools, and example objects |
| `test` | Synthetic, transactional, ABI, and regular-file suites |
| `examples-test` | UEFI host mock and read-only replacement controls |
| `feature-test` | Ordinary consumer profiles and public replacement matrix |
| `verify` | Core/adapter unwind data, undefined symbols, and example imports |
| `usbcheck.exe`, `repocheck.exe` | Build physical test tools; do not run them |

Executables, host objects, PDBs, and `sector.lib` are written to `build/win32/`.
`fat32.obj` / `fat32.lib` stay at the root; example objects stay beside their
sources. Linker matrices write their separate reports under `build/`.

## Physical interoperability

Select a FAT32 USB test volume labeled `TESTING`. The runners require its drive,
volume serial, USB device serial, and a private evidence destination. They do not
format the device and reject identity mismatches before destructive tests.

```powershell
.\tests\win32\run-64k.ps1 -Drive $TestDrive -ExpectedSerial $VolumeSerial `
  -ExpectedDeviceSerial $UsbSerial -EvidenceDirectory build\64k
.\tests\win32\run-repo.ps1 -Drive $TestDrive -ExpectedSerial $VolumeSerial `
  -ExpectedDeviceSerial $UsbSerial -Revision HEAD -EvidenceDirectory build\repo-test
```

`run-64k.ps1` requires 64 KiB clusters and checks native boundary files, library
mutations, and directory growth. `run-repo.ps1` requires 512-byte sectors and
clusters. It exports committed Git blobs in size order, writes A/B and deletes A,
writes C/D and deletes C, then repeats over missing files until all remain.
Raw-library and native reads must match the original blob IDs; the runner also
checks measured fragmentation and cluster ownership.

The independent locked FAT/hash oracle saves a sparse capture for replay:

```powershell
py tests\win32\inspect-64k.py build\64k\native-fixtures.json $VolumeSerial build\capture
Expand-Archive build\capture\sectors.zip build\replay
.\build\win32\usbcheck.exe X: --capture build\replay
```

`X:` is a placeholder. Captures, logs, and manifests can contain device identities
and file contents. Keep them in ignored `build/` or `tests/evidence/`, or outside
the repository. Publication summaries belong in [VALIDATION.md](../../docs/VALIDATION.md).

## Adapter and buffer contracts

[buffer.inc](buffer.inc) documents the assembly ABI; [api.h](api.h) adds its C
mirror to the public FAT32 header. The [Win32 example](../../example/win32/README.md)
shows a small assembly client. Required pointers are borrowed and nonoverlapping;
provider/state storage outlives the mounted identity.

```c
WinVolume volume = {0};
SectorBuffer buffer = {0};
FatIdentity identity = {0};
unsigned char sectors[3 * 4096];
FatWorkspace workspace = {sectors, sizeof(sectors), 0};

win_open(&volume, L"\\\\.\\X:", 0);       /* check each returned status */
sb_init(&buffer, &volume.ops);
fat_mount(&identity, &buffer.ops, NULL, &workspace);
/* enumerate/lookup/read; mutations stage in RAM even on a read-only backend */
sb_discard(&buffer);                     /* explicitly discard pending sectors */
win_close(&volume);
```

`win_open(...,1)` requires read/write access and locks the volume before returning.
It retains that lock through writes and explicit flushes. `win_close` dismounts
the volume while still locked, then closes the handle and frees its bounce buffer.
Check its status: failed dismount/handle close returns `F_IO` and records the
Win32 error, even though cleanup is still attempted. Close never commits pending
sectors. A failed open also cleans up and returns an error; it does not retry.
Reopen and remount when changing access mode; do not carry snapshots across it.
The adapter uses a
page-aligned bounce buffer for unbuffered I/O, reports short transfers as errors,
and records the Win32 error in `volume.error`. `win_read`, `win_write`, and
`win_flush` implement the backend callbacks.

`sb_commit(&buffer)` writes the latest version of each pending sector and flushes
the backend. `sb_discard(&buffer)` frees all staged changes. Call
`fat_invalidate(&identity)` after an external discard or direct buffer alteration;
remount if boot geometry changed. Commit preserves the same logical view, so it
does not require invalidation. Neither close nor discard commits implicitly.


The buffer stores a linked stack of `VirtualAlloc`-owned sector versions.
`begin` saves the head; rollback frees newer versions and acceptance retains
them. The configurable version limit bounds allocation. Lookup and superseded-page
elimination use linear scans; this provider is intended for bounded test sessions.

Keep the writable volume lock through all reads, writes, and flushes. Dismount
only during close, after commit or discard. The adapter never retries a failed
physical commit. On commit failure, the buffer retains evidence and becomes
poisoned; reads, staging, and retries fail. Discard and establish actual media
state before opening a new session. In-memory rollback cannot undo media writes.

The unbuffered backend transfers one sector through an aligned bounce buffer.
Seek/read/write state is serialized, short transfers are errors, and native
errors remain available in the adapter. The raw read-only mode does not acquire
a volume lock; its caller must provide a stable view for reliable diagnosis.
