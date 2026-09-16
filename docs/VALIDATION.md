# Validation

Results distinguish executable library tests, independent image observations,
and recorded physical interoperability. Local manifests, raw captures, and
compiler transcripts remain in ignored build storage.

## Automated coverage

The Windows harness passes **95 suites** against the assembled library. C sources
compile as C17 with warnings treated as errors. Fixtures encode BPBs, FATs,
directories, and file patterns independently; sparse providers model complete
FAT32 extents without allocating an entire volume.

| Area | Covered behavior |
| --- | --- |
| Geometry and formatting | All 31 supported sector/cluster combinations; primary/backup BPB, FAT/root/FSInfo, labels, exact callback counts, ordinary/verified policies, and every callback failure |
| Files and directories | Reads, overwrite, zero-filled extension, truncation, metadata, same-parent rename, deletion, empty directories, LFN lengths and damage, alias fallback, and 4 GiB limits |
| Allocation and mutation | Fragmented/backward chains, high-nibble preservation, active FATs, exhaustion, every staging failure, and preservation of the previous overlay on rollback |
| Shared ownership | Canonical same-file objects, independent positions, affected-file versions, iterators, parent pins, pool/reference limits, and stale mounts |
| ABI and workspace | All eight nonvolatile GPRs, callback alignment, volatile-register poisoning, exact three-sector workspaces, canaries, and failure before I/O on invalid storage |
| Concurrency | A competing thread while a provider/commit is suspended under the optional gate; busy callers preserve output/state |
| Diagnostics | Bounded chains, logical directory/LFN structure, lookup-name collisions, allocation ownership, reserved status, BPB backups, FAT copies, and stale/incomplete evidence |
| Recovery | Independent FAT views, explicit backup-BPB geometry, unique-prefix salvage, budgets, extent exhaustion, partial results, and source leases |
| Policy | Retained adverse evidence, scoped write/read restrictions, pinned findings, explicit diagnosis, and first-failure/partial-result preservation |
| Ordered commit | Production callback phases, independent raw decoding, every read/write/flush failure, half-sector failures, readback mismatches, dirty markers, and blocked retries after uncertainty |
| Streams | Map lifetime, fragmentation, coherent pending data, batching, unaligned outputs, exact completed prefixes, and failed/short/overreported range callbacks |
| Windows adapter | Unbuffered sector I/O against a disposable regular file, staging savepoints, limits, commit/discard, and poisoning |

Fragmented, shared, stream, and mutation tests include 128-KiB and 256-KiB
clusters. Large-directory tests cross a 256-KiB cluster. File data is also read
beyond a 4 GiB device-byte offset.

For the 600-cluster stream fixture, setup traverses 599 links in eight FAT-sector
reads. Reading 307,183 bytes then uses 304 range calls and one edge-sector call,
with zero steady-state metadata reads. These are synthetic request counts.

## Linker and example checks

- **264 ordinary links:** 33 feature profiles, MSVC LINK and LLD, object/archive
  inputs, and release/debug configurations. Checks enforce imports, feature
  membership, size ceilings, COMDAT associations, and retained unwind ranges.
- **32 replacement links:** both selection orders, direct/internal calls,
  object/archive/mixed inputs, and selected-definition metadata.
- **16 read-only links:** all 71 public exports rooted, 20 denied mutation APIs,
  excluded private writer/commit helpers, no provider writes, and writable controls.
- **UEFI host mock:** five-argument callback dispatch, 64-bit LBA/status, bounce
  alignment, mount/read/EOF, bounds, and offline/media-change behavior.

The core has no undefined external symbols or OS imports. The Win32 assembly
demo imports only KERNEL32. The UEFI example has not been boot-tested.

## Code size

The RBP register-allocation review measures the same library behavior before
and after substitution: total emitted procedure code falls from **27,796 to
27,150 bytes**, saving **646 bytes (2.3%)** across 62 of 149 procedures. Each
selected substitution reduces its own procedure. RBP remains a saved general
register; stack frames continue to use static RSP.

Decoded instructions were compared across all 149 procedures, accounting for
register substitutions, equivalent address expressions, and changed code offsets.
Branch destinations, relocation targets, saved-register sets, and stack allocations
match the baseline; unwind metadata retains no frame-register assignment.

The following linked `.text` sizes use the LLD release/archive profile with
`/OPT:REF /OPT:NOICF`. They include the small probe and linker alignment; they
are not the sum of selected procedure sizes and are not performance timings.
The tests enforce separate ceilings without relaxing them for this review.

| Consumer profile | Code bytes |
| --- | ---: |
| `mount` | 682 |
| `fat-view` | 1,034 |
| `boot-view` | 1,226 |
| `policy` | 1,770 |
| `policy-read` | 5,994 |
| `policy-adaptive` | 6,586 |
| `salvage-plan` | 938 |
| `salvage-read` | 1,338 |
| `format-plan` | 474 |
| `format` | 1,690 |
| `format-verified` | 1,786 |
| `put` | 1,018 |
| `put-checked` | 1,210 |
| `free-space` | 954 |
| `read` | 3,978 |
| `append` | 8,394 |
| `full` | 9,962 |
| `shared-read` | 4,410 |
| `shared-write` | 9,290 |
| `shared-full` | 11,482 |
| `locked-read` | 4,490 |
| `checked-read` | 5,226 |
| `adaptive-read` | 5,242 |
| `diagnostics` | 2,266 |
| `directory-check` | 3,466 |
| `name-check` | 5,226 |
| `ownership-check` | 3,114 |
| `ordered-write` | 11,466 |
| `verified-write` | 11,578 |
| `stream-read` | 4,554 |
| `stream-map` | 4,378 |
| `range-read` | 4,954 |
| `ordered-range` | 12,986 |


Per-function `.pdata`, `.xdata`, and CodeView contributions remain associated
with the owning COMDAT. Unused optional modules add no mounted state or retained
code. Current measurements are generated by `tests/win32/features.py`.

## Independent disk images

`tests/images.py` generates sixteen clean/damaged sparse volumes. Six observation
profiles cover snapshot, shared, checked, alternate-FAT, stream, and range reads.
Manifest extents and hashes are checked independently of library output. Cases
include short/cyclic chains, cross-links, orphan/bad clusters, duplicate lookup
names, LFN damage, unknown FSInfo, and conflicting FAT/BPB metadata.

Ordinary reads validate their requested range; they do not certify global
ownership or an unused tail. A cross-linked file can therefore return readable
but unexpected content. Diagnostic profiles are checked against their stated
scope, including damage they intentionally do not classify.

Separate tools exercise sixteen explicit backup-BPB cases and all 31 formatter
geometries. Production commit traces generate 89 completed/interrupted states
across six mutations and two geometries. Independent decoders check chains,
copy agreement, ownership, known bytes, and dirty intermediate states.

Earlier optional non-repair `fsck.fat 4.2` runs agreed with the stated fixture
expectations, including all formatter geometries and commit states. Its failure
to detect an LFN/short-alias name collision is recorded as an oracle limitation;
the independent slot decoder and library name checker detect that collision.
The publication review reruns raw decoders; it does not claim a fresh external
fsck or physical-device run.

## Recorded physical interoperability

These previous runs used 512-byte sectors. They establish observed Windows/raw
interoperability, not exhaustive hardware or firmware qualification.

| Cluster size | Recorded result |
| --- | --- |
| 512 bytes | A 45-file Git corpus matched raw/native reads after six pair-write/delete passes: 86 writes, 41 deletions, six fragmented files, up to 12 extents. |
| 32 KiB | Native boundary fixtures, two library mutation passes, and clean non-repair CHKDSK. |
| 64 KiB | Native hashes, mutation and directory-boundary workloads, clean CHKDSK, and a locked independent FAT/hash capture. A later replay matched 38 files totaling 4,131,934 bytes; its fragmented fixture used four clusters in two extents. |

The physical workload includes boundary patches, Unicode rename, metadata,
zero-filled growth, truncation, deletion, and native verification between phases.
Directory growth crosses a cluster boundary with a long-name record set.

Synthetic tests reproduce stale-cache errors when an external writer changes
media beneath a mounted identity. A prior transient post-format failure was
consistent with that mechanism, but its original cause was not proven. A separate
malformed-directory observation could not be replayed after reformatting;
its origin remains unknown. See [large-cluster coverage](CLUSTER64.md).

## Reproduction and limits

Run the [Windows harness](../tests/win32/README.md) and [image tools](../tests/README.md)
from the repository root. Generated output stays under ignored `build/`; physical
runners require explicit device identities and private evidence destinations.

Fault injection models callback failures, write prefixes, and selected torn
sectors. It does not establish real power-loss survival, all device reorderings,
firmware boot compatibility, or a new kernel/provider's correctness. Physical
tests are historical results; current synthetic and image checks cover the
revised library independently of that hardware.
