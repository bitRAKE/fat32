# Ordered staging and commit

FAT32 mutations describe dependencies; an optional library provider preserves
their sector versions and executes an explicit ordered commit. The ordinary
commit flushes completed phases. A separately linked alternate function also
flushes and reads back each write. Neither policy makes FAT32 journaled or
crash-atomic. The Windows `SectorBuffer` retains its separate unordered test
adapter contract.

## Provider extension

The base `SectorOps` layout stays 64 bytes with `reserved=0`. An optional
`FatOrderedOps` appends a barrier callback, 32-bit revision and 32-bit flags,
making the complete structure 80 bytes. Its base `reserved` field is set to
80, revision to 1 and flags to zero. The complete structure must remain alive
for the mount. A read-only consumer neither inspects nor links the extension.

Before calling `begin`, mutation admission rejects an unrecognized nonzero
size, revision, flags or missing barrier with `F_ARGUMENT`. The library never
reads the tail of an ordinary 64-byte provider. Base writable providers remain
usable for in-memory savepoints and host tests.

`barrier(context, uint32_reason)` uses Win64 and returns `F_*`. It marks the end
of the preceding staged phase. It must not perform physical writes or flushes;
the entire operation can still fail and roll back. Failure flows to `end(0)`
and preserves the prior logical view and all canonical object versions.
Provider callbacks consume input before returning and cannot reenter the owner.
An outer shared-volume operation lease covers the callbacks, including waits.

| Reason | Required dependency |
| --- | --- |
| `FO_PREPARED` | New storage is reserved and initialized before publishing references to it; directory end/slot preparation precedes its new name |
| `FO_PUBLISH` | Prepared content or name records precede SFN publication; a complete new rename target precedes erasing its old name |
| `FO_UNPUBLISH` | A smaller or deleted SFN precedes cutting/freeing the storage it previously referenced |
| `FO_DETACH` | The retained chain ends at EOC before detached tail clusters become reusable |

Reasons identify dependencies, not sortable global phase ranks. Phase order is
the order in which the operation calls the provider. Repeated reasons and empty
phases are allowed. A provider can omit an empty phase from its stored plan.
It may coalesce repeated writes to one LBA within a phase, but must preserve
different versions of that LBA across phase boundaries. It must not reorder
phases or keep only the last sector version for the whole operation.

## Corrected operations

Growth constructs a detached new chain, reserving and zeroing all its clusters.
After `FO_PREPARED`, it attaches that chain to the old tail. A write supplies its
requested bytes before `FO_PUBLISH`; only then does it publish the larger SFN
size. Plain resize supplies zeros before that publication. A growth write can
consume existing excess allocation without unnecessarily trimming it.

Shrink first publishes the smaller size (and zero first-cluster field for an
empty file). After `FO_UNPUBLISH`, it cuts a retained chain to EOC. After
`FO_DETACH`, it frees the detached tail. Full deletion erases the name records,
marks `FO_UNPUBLISH`, then frees the chain. Successful equal-size resize can
trim excess allocation, so the shared interface advances its chain version
even when first cluster and logical size remain unchanged.

Directory growth initializes each newly allocated cluster before linking it
to its parent chain. Creation prepares a new end marker before replacing the
old one. Bytes after a directory end marker are unspecified: slots that will
hold the new name are neutralized while preserving existing zero markers.
After `FO_PREPARED`, the LFN records are written; `FO_PUBLISH` precedes the SFN.
This prevents arbitrary old bytes after the previous end marker from becoming
live phantom files during name publication.

Rename prepares the new name and complete file metadata before its final
`FO_PUBLISH`, then erases the old name. An interruption can leave both names
referring to the same chain; this is not crash-atomic rename. Recovery/admission
policy must recognize that outcome before further mutations. In-place content
overwrite is likewise not made crash-atomic by metadata ordering.

## Caller-owned staging

`fat_order_init(order, backend, workspace)` initializes a fresh 176-byte
`FatOrder` without I/O or allocation. Mount over `&order.ops.base` using the
normal three-sector mount workspace. Use one identity/shared owner per order
provider, with exclusive ownership of the underlying volume. Keep the backend
table, context and all buffers alive until the owner is drained and discarded.
Backend LBAs are volume-relative and its extent/sector size must stay constant.
The order object, arrays, mount buffers and callback I/O must not overlap.

The 32-byte `FatOrderWorkspace` describes a byte arena and an index:

| Field | Contract |
| --- | --- |
| `data`, `bytes` | Caller storage for `floor(bytes / (sector_bytes + 16))` sector versions; at least one, at most `0xFFFFFFFC` |
| `index`, `slots` | Caller array of 32-bit entries; slots is a power of two at least 2 |
| `reserved` | Zero |

Initialization clears the index. At most `slots/2` distinct LBAs may be pending.
Repeated writes in one phase coalesce; a new phase consumes another version.
Reads use the latest pending version, otherwise the backend. Arena/index
exhaustion returns `F_MEMORY` and the logical operation rolls back without
physical writes. No heap allocator, OS function, global cache or per-file
commit buffer is required. Size the arena for the largest accepted operation,
including zero-filled allocation and repeated metadata sectors across phases.

Exactly one accepted logical mutation may be pending. Reads see it immediately;
another mutation returns `F_BUSY` until explicit commit/discard. Logical
acceptance updates canonical objects. Commit changes durability, not visibility.
A successful commit preserves handles, object versions and current chain reports.
`fat_order_discard(order, identity)` drops staged versions without I/O and calls
`fat_invalidate`, retiring handles and reports. It does not clear a poisoned
context or make a partially written medium healthy.

## Physical commit and reports

`fat_order_commit(order, identity, report)` requires a backend `write` and
`flush`, in addition to `read`. Initialization permits a read-only backend for
staged experiments, but committing a nonempty plan then returns `F_READONLY`
without I/O. A backend flush must mean completion of all preceding writes to
its advertised persistence boundary. The library cannot improve a device's
truthfulness or guarantee atomic sector writes.

Hold the same shared-volume operation lease across staging, commit, discard
and every backend callback. The optional `fat_call_locked` can dispatch commit
with these three arguments. It holds the gate through cooperative waits;
competing operations return busy without touching their output.

Commit performs these steps:

1. Read FAT[1] from each selected copy; reject invalid reserved bits or existing
   dirty/error history before writing. This is a local commit prerequisite,
   not a full mirror, chain, directory or ownership audit.
2. Clear the clean-shutdown bit in every selected copy and flush. Disabled
   mirroring selects only the active FAT; otherwise all copies participate.
3. Write the stored versions in chronological phase order and flush after each
   nonempty phase. Any intermediate FAT[1] sector has its clean bit masked off,
   including versions staged before the dirty marker was written.
4. Set the clean bit in all selected copies and flush after all mutation phases
   have completed. Reserved high nibbles remain unchanged.

`fat_order_commit_verified` additionally flushes after **each** write, rereads
that LBA through the backend into a separate mount scratch buffer and compares
the complete sector. It never compares against the pending overlay. A mismatch
returns `F_VERIFY`. This is optional bring-up/diagnostic policy; readback can
detect divergence but does not establish physical survival of a power cut.
Only the verified entry point references the verification helper.

The caller supplies a 40-byte `FatCommitReport`:

| Fields | Meaning |
| --- | --- |
| `status`, `effect` | Returned `F_*`; `FE_NONE`, `FE_UNCERTAIN`, or `FE_COMMITTED` |
| `phase` | Stored chronological phase number, or `FP_ADMIT`, `FP_DIRTY`, `FP_CLEAN` |
| `operation`, `lba` | `FI_READ`, `FI_WRITE`, `FI_FLUSH`, `FI_VERIFY`; flush uses LBA `UINT64_MAX` |
| `writes`, `flushes` | Attempted backend calls, including the call that failed and strict-policy flushes |

An empty plan returns success with `FE_NONE` and no I/O. A pre-write backend
read failure leaves the plan accepted and reports `FE_NONE`; an explicit retry
is possible because no write was attempted. A diagnosed reserved/status fault
also has no physical effect, but restricts future commits/mutations on that
context until an explicit new initialization following inspection/recovery.
Its pending view is still readable and can be discarded for backend inspection.

Effect becomes `FE_UNCERTAIN` **before** the first write callback: a failed
callback may already have changed part of a sector. Any later read/write/flush
or verification failure preserves the plan and first failure evidence, poisons
the provider, and invalidates the mount's handles and reports. Repeated commit
returns that same evidence without I/O. Reads and mutations through an uncertain
provider fail; discard cannot undo physical effects or authorize a blind retry.
Inspect the backend through a separate read-only recovery session before deciding
whether to reinitialize. No automatic repair or retry is hidden in this module.

## Evidence and limits

The test-only trace in [tests/win32/ordering.c](../tests/win32/ordering.c) records staged writes
and phase boundaries. An independent raw-sector decoder reconstructs every
completed phase and checks live directory references, finite allocated chains,
file lengths and FAT-copy agreement. Targeted assertions check detached append
allocation, content before enlarged size, smaller/deleted entries before free,
rename publication, and unspecified garbage after an old directory end marker.
Both 512-byte and 4096-byte sectors, with clusters through 64 KiB, pass. Barrier failures roll back the
operation, and unsupported extension versions fail before a savepoint begins.

The production implementation in `fat/order.inc` is covered by the Windows
harness. Commit suites exercise both policies, four geometries, every backend
read/write/flush failure, partial failed writes, mismatching readback, exact
first-failure retention, index collisions and bounded capacity. They test active
and single FATs, directory growth and an actual competing thread while commit
is suspended. Independently decoded raw media is checked at normal phase flushes.

The linker matrix includes ordinary and verified commit profiles;
[VALIDATION.md](VALIDATION.md) records their current footprints. Basic mount/read
and their workspaces are independent of this module. No base profile
retains the commit module, and ordinary commit does not retain verification.

`tests/commit-images.py` exports production callback traces for append, shrink,
rename, delete, directory creation and metadata changes at 512/512 and
4096/65536-byte geometries. It creates sparse images at flush boundaries and
selected half-sector metadata failures, records independent chain/ownership/
content observations, and can run non-repair `fsck.fat`. These are reproducible
fault-model images; consumer boot and real power-loss behavior require their
own qualification.
All 89 generated states pass their independent decoder and fsck expectations;
baseline/final images are clean, while intermediate allocation/rename states
remain explicit recovery inputs. The optional [range adapter](STREAMING.md)
merges pending sectors and raw backend ranges without bypassing accepted data.
Unflushed device reordering, torn sectors at other offsets, firmware behavior
and recovery/repair qualification remain separate work.
