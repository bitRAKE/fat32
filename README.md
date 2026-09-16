# fat32

An x86-64 FAT32 library in fasmg/fasm2 assembly for UEFI loaders and operating
systems. It implements filesystem operations over caller-supplied sector I/O,
using the Windows x64 calling convention and caller-owned storage. The library
has no operating-system imports, allocator, or global mount state.

## Interface

Mount a bounded `SectorOps` provider with `fat_mount`, supplying a `FatIdentity`
and three sectors of workspace. Use a `FatVolume` to share canonical file objects
between independent handles. The library owns FAT32 interpretation and caches;
the caller owns device selection, storage, synchronization, and durability.

```text
application / loader / kernel
             |
         fat32.lib
             |
         SectorOps
             |
caller sector provider / optional transactional staging
```

- 512–4096-byte sectors and clusters through 256 KiB, at most 128 sectors per cluster.
- Directory enumeration, file creation, same-parent rename, deletion, byte reads,
  overwrite, extension, truncation, attributes, and packed timestamps.
- UTF-16 long names, short aliases, FAT mirroring, and explicit cache lifetimes.
- Transactional staging for mutations, with separate physical commit.
- Optional diagnostics, read-only recovery views, salvage, streams, ordered
  commit, session policy, and volume formatting.
- Independently selectable functions; unused modules and their metadata are
  discarded by the linker. Consumers may replace public functions under the
  documented [linking contract](docs/LINKING.md).

Start with the [shared handle API](docs/SHARED.md) and [sector contract](docs/API.md).
[fat32.asm](fat32.asm) indexes every export. [fat32.inc](fat32.inc) and the
included `.inc` files specify the assembly ABI; [fat32.h](fat32.h) is the C17 ABI.

## Build

The root build produces only `fat32.obj` and `fat32.lib`. In an x64 Visual Studio
developer prompt, with the required fasm2 toolchain available:

```cmd
nmake /nologo
```

`FASM2_ROOT` defaults to `../fasm2`; override it in the environment or on the
NMAKE command line. The core does not need Win32 bindings. See [building and
linking](docs/BUILD.md) for assembler capabilities and direct-object builds.

## Project layout

| Location | Contents |
| --- | --- |
| Root `.asm`, `.inc`, `.h` files | Library entry point and public interfaces |
| `fat/` | Filesystem implementation |
| `common/` | Assembly object, procedure, and unwind policy |
| [docs/](docs/README.md) | API contracts, implementation notes, and validation |
| [example/](example/README.md) | UEFI, read-only replacement, and Win32 examples |
| [tests/](tests/README.md) | Image fixtures, independent checks, and platform harnesses |

The [Windows harness](tests/win32/README.md) supplies the test transport, buffer,
build bootstrap, and host executables. Its dependencies and device workflow are
separate from the library build.

## Operating contract

Keep a mounted provider stable and serialize access through the owning volume.
Read callbacks complete synchronously. Mutations require provider savepoints;
accepted changes become durable only through an explicit commit policy.
Physical commit is not a journal or a power-loss guarantee.

FAT32 file sizes are limited to `0xFFFFFFFF` bytes. Lookup folds ASCII case and
compares other UTF-16 units exactly. Partition discovery, cross-parent moves,
automatic repair, executable loading, and boot handoff are caller responsibilities.

See [validation](docs/VALIDATION.md) for tested coverage and its limits, and the
[UEFI example](example/uefi/README.md) for an assembly integration walkthrough.

## License

[MIT](LICENSE).
