# Implementation and verification

The library contains all FAT32 interpretation. A provider supplies bounded,
synchronous sector I/O and optional transaction callbacks. See [API.md](API.md)
for that boundary and [SHARED.md](SHARED.md) for the normal handle interface.

## Source organization

| Source | Responsibility |
| --- | --- |
| `fat32.asm` | Complete export reference and implementation includes |
| Root `.inc` files and `fat32.h` | Assembly and C interfaces, layouts, and ownership |
| `fat/disk.inc` | Private on-disk layouts and format signatures |
| `fat/volume.inc`, `directory.inc`, `file.inc`, `create.inc` | Geometry, FAT, namespace, data, and mutation algorithms |
| `fat/shared.inc`, `snapshot.inc` | Shared handles and snapshot compatibility |
| `fat/check*.inc`, `view.inc`, `boot-view.inc`, `salvage.inc` | Optional diagnostics and recovery views |
| `fat/order*.inc`, `stream.inc`, `space.inc` | Ordered staging, mapped/range reads, and free-space scan |
| `fat/format.inc`, `policy.inc`, `adaptive.inc` | Formatting and optional operation policy |
| `common/policy.g` | NEWCOFF, static-RSP procedures, unwind, and CodeView |

The root [makefile](../makefile) builds the library. Platform adapters and test
executables belong to [tests](../tests/README.md); integration samples belong to
[example](../example/README.md). Extend the public contract independently of a
particular harness or device.

## Register and procedure policy

The [x86-64 coding policy](x86-64_coding_policy.md) explains the rules below
through source changes, emitted encodings, PROC/ENDP expansion, and repository
measurements. It explains how those choices reduce instruction-processing,
load/store, and dependency demands as well as code size, and covers value
lifetimes and memory changes across calls.

The static-RSP `PROC`/`ENDP` configuration leaves RBP available as an ordinary
nonvolatile register. Prefer RBP/EBP for a live pointer or a non-64-bit integer
when it reduces the actual encoding. Include RBP in `uses`, just like any other
callee-saved register; it is not an implicit frame pointer.

EBP operations can avoid the extra register-extension prefix needed by R12D–R15D.
RBP-based addressing can avoid R12's extra SIB byte. Measure the complete routine:
`[rbp]` requires a displacement byte, BPL still needs REX, and 64-bit arithmetic
already needs REX.W. Short-branch relaxation and push/pop encodings also affect the
result. Preserve pointer width and intentional zero-extension; do not narrow a
value merely to fit a smaller encoding.

Keep values in nonvolatile registers only when their lifetime requires it.
Use incoming argument registers directly while valid. `fastcall` assigns
arguments left to right; an earlier assignment can destroy a later source.
Stack-argument setup may use RAX. Check expanded instructions when changing
argument or register order.

Every framed procedure shares its epilogue. Load an error status before an
adjacent CMP/TEST and conditional branch, preserving their adjacency. Bare
leaf returns may remain separate. All nonvolatile pushes belong in the prologue;
the unwind records must describe their exact order and stack allocation.

Structure-valued locals can affect dot-label scope. Use a `body:` anchor after
`endl` when necessary. Keep local storage, call home area, and unwind metadata
consistent with the static-RSP policy.

Each procedure owns a `.text$procedure` COMDAT. Public defaults use ANY; private
helpers and constants use NODUPLICATES. Unwind and debug contributions associate
with their owning code. Keep exported declarations centralized in `fat32.asm`,
with register summaries matching the `.inc` contracts and C header.

## Filesystem invariants

1. Provider LBAs are volume-relative and bounded. Use 64-bit arithmetic where
   offsets/products can exceed 32 bits; cluster bytes remain a dword.
2. Mount validates primary geometry, FAT capacity, root, and extent before
   publishing the identity. Additional diagnosis is explicitly selected.
3. FAT reads mask the high nibble; writes preserve it in each affected copy.
   Basic `fat_put` updates selected copies. `fat_put_checked` additionally checks
   prior low-bit agreement; both require an existing transaction.
4. Shared reads validate encountered links within bounded traversal. Complete
   chain checking and snapshot preflight have separate contracts.
5. Cache keys never publish failed reads or hide unstaged mutations. A write
   invalidates affected views; provider rollback restores the prior overlay.
6. LFN records must agree in ordinal, checksum, type, cluster, padding, length,
   and UTF-16 validity. Invalid sets fall back to the short alias.
7. Newly allocated or exposed file bytes are zeroed. Partial-sector writes
   preserve surrounding bytes; shrinking releases only the validated tail.
8. Failed mutations preserve prior canonical metadata and staged changes.
   Successful shared publication updates affected object versions. Snapshot
   mutation retires incompatible shared ownership.
9. FSInfo is advisory. Allocation scans actual FAT entries and invalidates valid
   hints after mutation; it never infers exhaustion from a hint alone.
10. Physical commit is distinct from logical acceptance. Recovery after uncertain
    media writes follows the selected provider and consumer policy.

## Review and validation

Run the [Windows harness](../tests/win32/README.md) for synthetic media, fault
injection, ABI callbacks, image observations, and linker matrices. The ABI probe
seeds all eight nonvolatile GPRs, including RBP, checks preservation and call
alignment, and poisons volatile registers after callbacks.

Compare emitted code per procedure before accepting register changes. Retain
unwind/debug associations and feature membership, not just a smaller total.
The linker matrices enforce separate feature-size ceilings and replacement
selection. See [VALIDATION.md](VALIDATION.md) for current results.

Keep evidence under ignored build storage. Published documentation records
coverage and contracts; local compiler paths and device identifiers stay in
private reports. Update links and build dependencies when moving or adding files.
