# Examples

| Example | Purpose | Build |
| --- | --- | --- |
| [UEFI loader / OS integration](uefi/README.md) | Read-only firmware sector adapter, caller-owned mount, file reads, and the kernel handoff; guidance for transactional writes | `build.cmd examples examples-test` |
| [Windows console example](win32/README.md) | Small host-side client that lists the selected volume's root through the supplied adapter | `build.cmd fatdemo.exe` |

Run build commands from the repository root. `build.cmd examples` builds both
examples; `build.cmd examples-test` runs the UEFI host mock. The Win32 executable
is written to the repository root; assembly objects and generated linker response
files are written beside their sources.

The UEFI example exports callable procedures. It deliberately leaves device
discovery, allocation, image entry, and the final handoff in the loader that
links it. Its guide distinguishes these integration responsibilities from the
FAT32 library's existing features and tested behavior.
