# Examples

| Example | Purpose | Build |
| --- | --- | --- |
| [UEFI loader / OS integration](uefi/README.md) | Read-only firmware sector adapter, caller-owned mount, file reads, and the kernel handoff; guidance for transactional writes | `build.cmd examples examples-test` |
| [Windows console example](../example.asm) | Small host-side client that lists X:'s root through the supplied adapter | `build.cmd all` |

The UEFI example exports callable procedures. It deliberately leaves device
discovery, allocation, image entry, and the final handoff in the loader that
links it. Its guide distinguishes these integration responsibilities from the
FAT32 library's existing features and tested behavior.
