# 64 KiB cluster support

64 KiB clusters are a supported geometry for the library's UEFI and modern
x86-64 target. Legacy installer and 16-bit application restrictions are outside
this project's compatibility requirements.

## Representation and arithmetic

| Sector bytes | Sectors per cluster | Cluster bytes |
| ---: | ---: | ---: |
| 512 | 128 | 65,536 |
| 4096 | 16 | 65,536 |

`BPB_SecPerClus` is an unsigned byte. Mount zero-extends it, validates a power of
two, and computes the cluster-byte count in a dword. A 65,536-byte count must not
be truncated to 16 bits. Cluster-to-sector mapping uses 64-bit multiplication;
FAT entry width and the 32-byte directory record format remain unchanged.

The mount contract accepts 512–4096-byte sectors, at most 128 sectors per cluster,
and at most 65,536 bytes per cluster. These bounds are independent checks.
Microsoft's [format documentation](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/format)
includes the 64 KiB FAT32 allocation size.

## Verification

The automated tests cover both geometries above, including:

- Independently encoded fragmented and backward-linked chains, with file data
  beyond the 4 GiB device-byte offset.
- Complete reads and writes across the 65,535/65,536-byte boundary, independent
  committed-sector comparisons, extension, zero fill, and truncation.
- A 255-unit long name crossing the final directory sector, directory growth,
  committed remount, rename, deletion, and neighboring-entry preservation.
- Rejection of malformed directory contents and explicit cache invalidation
  after external changes.

Physical tests passed Windows-created boundary fixtures, native verification of
library mutations, and directory growth with a long-name set spanning slots
2,047–2,050. An independent locked reader verified file hashes and fragmented
chains. The library also replayed the captured sectors through a separate
file-backed provider. Missing sectors returned `F_IO`; a deliberately cleared
FAT link returned `F_CORRUPT`. See [VALIDATION.md](VALIDATION.md).

## Consistent media access

An unlocked raw reader may see an older cached FAT alongside newer directory
entries if another filesystem driver changes the volume. A controlled test
reproduces this at both small and 64 KiB clusters. A transient post-format read
failure was consistent with Windows initializing the volume; its exact cause
was not established from the original observation.

Acquire exclusive ownership before creating an identity, or read an immutable
snapshot. After external edits have stopped, invalidate and reacquire entries;
remount after geometry or media changes. A fixed delay does not establish this
consistency boundary.

## Reproduction

[tests/run-64k.ps1](tests/run-64k.ps1) accepts the drive and expected device/volume
identities as explicit inputs. It creates native fixtures, runs the physical
mutation tests, compares hashes, and runs non-repair CHKDSK.
[tests/inspect-64k.py](tests/inspect-64k.py) supplies the independent locked oracle
and sector capture. Commands and required local settings are in [README.md](README.md#physical-tests).
Keep generated evidence in ignored local storage; raw captures contain the
volume's identifiers and file contents.
