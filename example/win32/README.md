# Win32 root-directory demo

[demo.asm](demo.asm) is a small CRT-free console client. It opens a FAT32 volume
read-only, initializes the sector buffer, mounts the filesystem, and lists root
entry names as UTF-8 through `fat_dir_open` / `fat_dir_next`. All filesystem
interpretation stays in `fat32.lib`.

Set the `device` string in `demo.asm` to the intended volume before running.
The checked-in `X:` value is a placeholder. Opening a raw volume requires an
elevated process, and the backing volume must remain stable while it is mounted.

Build from the repository root:

```cmd
build.cmd fatdemo.exe
```

This produces `fatdemo.exe` in the repository root and `demo.obj` / `demo.response`
beside the source. Run the executable from an elevated console after selecting
the volume. It returns zero after enumeration or one on error; the error message
points to the diagnostic USB harness.

The demo links `fat32.lib`, `sector.lib`, and KERNEL32. See [buffer.inc](../../buffer.inc)
for the Win32 provider and staging contracts, or the [UEFI example](../uefi/README.md)
for a firmware-backed provider.
