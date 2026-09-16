# Optional read-only salvage

`fat_salvage_plan` maps an explicitly selected file-chain prefix into caller-owned
extents. `fat_salvage_read` copies bytes from that map. Neither function changes
the volume, chooses an authoritative FAT, repairs allocation, opens files by
name, or imports ordinary mutation functions. Both are separately selectable
public `COMDAT ANY` functions listed in `fat32.asm`.

Use existing directory/name/ownership diagnostics to collect metadata and assess
its scope. Pass a mounted identity or an explicit [FAT-copy view](CHECKING.md#selecting-a-fat-copy)
to select the interpretation. A valid chain can belong to another file: every
returned extent is **candidate content**, including when planning returns
`F_OK`. Mapping does not establish exclusive ownership, original file length,
correct data, or durable storage. Retain the evidence that supplied the first
cluster and requested byte count alongside the result.

## Planning and work limits

`fat_salvage_plan(identity, request, plan)` takes a 32-byte `FatSalvageRequest`:

- `extents` and `capacity`: caller-owned `FatExtent[capacity]`, eight bytes per
  entry (`first` cluster and contiguous cluster `count`).
- `first` and `bytes`: explicit first cluster and maximum candidate bytes,
  bounded by the FAT file-size representation (`UINT32_MAX`). Zero bytes needs
  no FAT access; it makes no statement about allocation after that limit.
- `fat_budget`: maximum attempted FAT-entry accesses, including cache hits and
  failures. A zero budget admits no cluster from a nonempty request.
- `pair_budget`: maximum comparisons of a prospective cluster against an
  existing extent. These establish uniqueness without a whole-volume bitmap.
  Each comparison costs constant work. Contiguous chains normally need one
  comparison per additional cluster; fragmented chains can need more.
- `reserved`: zero.

The 144-byte `FatSalvage` holds an embedded `FatCheck`, extent pointer/capacity,
extent count, requested `bytes`, admitted `available` bytes, and comparison
count/budget. All storage is caller-owned; no mandatory mounted, object, handle
or provider state grows. Only the existing FAT cache and sector scratch are used.

Each prospective cluster must lie in the mounted data area, not occur in any
earlier extent, and have a readable selected-FAT entry containing a legal next
data cluster or EOC. Free, bad, reserved and out-of-range values stop **before
admitting that cluster**. Data from such a cluster may still exist physically;
this interface does not guess allocation from unallocated sectors. A repeated
cluster is never included twice. Adjacent ascending clusters coalesce into one
extent; a full array can still extend its final contiguous run.

Planning stops once the requested bytes are mapped; it does not audit an unused
tail. `FC_EXTRA` then means the final admitted entry has a legal continuation,
not that the unseen tail is healthy. A last partial cluster contributes only
the requested bytes, although the extent necessarily names the whole cluster.

Every return initializes the report. Inspect both status and `available`:

| Result | Meaning |
| --- | --- |
| `F_OK` | The requested candidate byte limit is mapped; health remains outside this scope |
| `F_CORRUPT` / `FC_SHORT` | EOC or first cluster zero precedes the requested byte limit |
| `F_CORRUPT` / `FC_LINK` | Invalid prospective cluster or allocation value |
| `F_CORRUPT` / `FC_CYCLE` | The next prospective cluster already occurs in the prefix |
| `F_LIMIT` / `FC_BUDGET` | FAT-attempt budget exhausted |
| `F_LIMIT` / `FC_PAIR_BUDGET` | Extent-comparison budget exhausted |
| `F_MEMORY` / `FC_WORKSPACE` | Another extent needs a slot; observed/expected give capacity and required capacity |
| Provider error / `FC_IO` | Failed FAT access; original provider status retained |

`check.scope` is `FC_SALVAGE`. `examined` counts FAT attempts; `count` counts
admitted unique clusters; `last` is the last admitted cluster. `cluster` identifies
the current candidate. `sector` identifies the last attempted FAT-sector LBA,
or `UINT64_MAX` if the stop precedes that attempt. `observed` records the FAT
value for a link failure. `pairs` is independent of FAT attempts. Admission
errors (`F_ARGUMENT`, unmounted `F_FORMAT`, active-transaction `F_BUSY`) perform
no I/O, leave extent storage untouched, and make no prefix available.

## Reading a candidate prefix

Hold the same exclusive source/provider maintenance lease from planning through
the last salvage read. Source geometry, logical content, provider, OEM mapping,
plan and extents remain unchanged and live. No mutation, commit/discard, remount,
external writer or callback reentry is allowed during that lifetime. This also
applies to accepted staging, which is visible through the selected provider.
Serialize other reads through the same lease. A generation check rejects
explicit invalidation/remount or a closed view, but is not a substitute for this
lease or an immutable-snapshot guarantee.

`fat_salvage_read(plan, transfer)` accepts a plan with a partial prefix even when
planning reported damage, a limit or a provider failure. It performs data-sector
reads through the same logical provider; it does not walk the FAT again or alter
the diagnostic result. It returns:

- `F_OK` when all requested bytes lie in the available prefix and were copied.
- `F_END` when the request starts at/past that prefix, or crosses its end. In
  the latter case `transfer.done` reports the bytes actually copied.
- The provider error when a sector cannot be read; only earlier complete
  copies count in `done`. The failed sector's bytes are not copied to output.

Zero length succeeds without I/O on a current plan. Null/wrapping nonempty
buffers and wrapping offset/length ranges are refused. All buffers, maps,
requests, reports and library state must be valid and disjoint. As with other
caller-owned structures, modifying a generated plan violates the contract.

No missing bytes are synthesized or zero-filled. The caller must preserve the
original planning status and byte limit when presenting recovered data. A
successful salvage read does not turn a shortened recovery into a successful
ordinary read of the original file.

```c
FatExtent extents[32];
FatSalvageRequest request = {extents, 32, first_cluster, declared_size,
                             4096, 65536, 0};
FatSalvage plan;
int diagnosis = fat_salvage_plan(&chosen_identity, &request, &plan);
/* Under the same lease, retain diagnosis and plan.check with recovered data. */
FatTransfer transfer = {output, 0, output_capacity, 0};
int copied = fat_salvage_read(&plan, &transfer);
/* Only transfer.done output bytes exist; examine both diagnosis and copied. */
```

## Inspecting backup geometry

`fat_view_boot(view, provider, source, workspace)` opens a read-only candidate
when the primary BPB cannot be used, or when the caller explicitly wants to
compare another interpretation. It never runs automatically after mount failure.
`view` is a zeroed or closed 216-byte `FatView`; the separate workspace holds
three sectors. The 16-byte `FatBootSource` supplies `sector` (volume-relative),
`reserved` (zero), and an optional caller-owned OEM map.

The caller chooses a nonzero sector within the provider extent. The function
uses the ordinary mount parser with a temporary bootstrap read callback, so
geometry rules stay in one implementation. The default mount performs exactly
one read of the selected sector. The candidate must also lie within its own
reserved region (`sector < fat_start`). No backup pointer in a broken primary
is trusted, no sector is discovered by a scan, and a candidate's optional backup
pointer need not point to the explicitly selected copy.

The constructor's unwind record accounts for 312 frame bytes, excluding its
return address and callees/provider. This is a static frame measurement, not a
worst-case stack bound; its temporary identity/provider live across callbacks.

`F_OK` means that the selected bytes supply usable candidate geometry. It does
not establish their authority, filesystem health or agreement with the primary.
The caller retains the selected sector, source identity and original failure
as evidence. Argument, range, workspace, format and provider failures leave the
entire output view unchanged; workspace contents are disposable. A live output
returns `F_BUSY`, and exhausted generations return `F_LIMIT` without I/O.

After opening, every read uses its original volume-relative LBA. LBA zero is
**not** redirected to the backup. In particular, `fat_check_backup` still sees
the broken/unreadable primary or disagreement between real copies. The view
retains only the provider's read capability; mutation, transaction, flush and
extension capabilities are cleared. Attach a shared volume for ordinary reads,
select a different FAT using `fat_view_open`, or map candidate file data through
the salvage API above. Close using `fat_view_close`; close/reopen stales old
handles and reports.

Hold one stable provider maintenance lease for the complete view lifetime,
including all child views and salvage plans. Keep provider/context, OEM map,
view and disjoint workspace live; no mutation, commit/discard, external writer
or callback reentry is permitted. This recovers an interpretation for inspection
without replacing the primary BPB or making a writable recovery mount.

```c
FatView candidate = {0};
FatBootSource source = {6, 0, NULL}; /* explicit location chosen from evidence */
int status = fat_view_boot(&candidate, &provider, &source, &scratch);
if (status == F_OK) {
    /* Inspect candidate.identity under the same lease; preserve real BPB data. */
    fat_view_close(&candidate);
}
```

## Repair boundary

This API is read-only analysis and copying. A future repair proposal must retain
the expected original sector contents and evidence for its choice, then recheck
them under exclusive maintenance ownership before any explicit application.
Cross-link resolution, selecting authoritative FAT/BPB contents and orphan
reconstruction can require different evidence and workspace. They are
not inferred from a readable prefix. Normal mount/read never invokes salvage
or repair, and the read-only stub example keeps these two exports available.
