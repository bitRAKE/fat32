# Optional structural diagnostics

Basic mount reads the primary BPB once, checks the arithmetic and extents needed
to use it safely, and publishes geometry. Basic directory-open checks its first
cluster and performs no I/O. Shared reads inspect links needed by their request.
These successful operations make no claim about an unused tail, other files,
backup metadata, or the rest of the volume.

The functions below are independent COMDATs. Basic functions never call them.
Selecting one checker does not select the others. No identity, object or handle
grew for diagnostics: the caller supplies a 112-byte `FatCheck`, and the existing
three sector buffers suffice. All calls use Win64 and the same serialized lease
as shared operations; an outer `fat_call_locked` can hold that lease across a
check/read sequence. The checks issue reads only, not writes or repairs.

## Interfaces and exact scope

| Function | Arguments | Evidence |
| --- | --- | --- |
| `fat_check_chain` | identity, first, maximum links, report | Legal links, cycle detection, finite EOC, count and terminal cluster; first zero means an empty chain |
| `fat_check_file` | handle, maximum links, report | Complete ordinary-file chain and enough clusters for the canonical size; permits excess allocation and sets `FC_EXTRA` |
| `fat_check_directory` | directory handle, maximum slots, report | Bounded logical-directory slots, LFN associations and encountered directory-chain links; immediate evidence |
| `fat_check_fresh` | handle, report | No-I/O test of the file report's lifetime; returns its stored status or `F_STALE` |
| `fat_check_reserved` | identity, report | Active FAT entries 0/1, media byte, clean-shutdown and previous-I/O-error bits |
| `fat_check_backup` | identity, report | Primary/backup signatures, BPB bytes 11–51, extended boot signature and, when present, volume serial |
| `fat_check_mirrors` | identity, `FatFatRange*`, report | Low 28 bits of the requested FAT-entry range in every mirrored copy |
| `fat_read_checked` | handle, transfer, `FatCheckedRead*` | Obtain/reuse file-chain evidence, then call the ordinary shared read |
| `fat_read_adaptive` | handle, transfer, `FatCheckedRead*` | Ordinary read first; diagnose corruption/I/O failure once within the link budget; preserve the read result |

Layouts are in [check.inc](../check.inc); the C test mirror is
[tests/win32/check-api.h](../tests/win32/check-api.h). Reports are overwritten by each checker,
including errors. Checkers require a mounted identity with no active provider
savepoint; this does not preclude holding the optional shared-volume gate.

The file checker uses canonical managed metadata. It does not reread the SFN,
validate parent membership, inspect directory/LFN structure, compare mirrors,
or establish exclusive ownership of clusters. In particular, two valid chains
can share clusters and both pass this checker. The separate ownership checker
below covers allocation ownership; an optional [backup-BPB view](RECOVERY.md#inspecting-backup-geometry)
can recover candidate geometry for read-only inspection.
The adaptive reader below diagnoses only this file-chain scope. Separately
selected [salvage functions](RECOVERY.md) can map and read candidate prefixes
from an explicitly selected FAT without changing the media.

Backup comparison ignores boot code, labels and reserved padding. A zero backup
pointer is absent (`FC_ABSENT`), not contradictory. A nonzero pointer outside the
reserved region is `FC_BACKUP_LOCATION`; an unreadable claimed copy reports the
provider failure. It does not choose a replacement boot sector or certify that
fresh media agrees with a mount that an external writer has changed.

Mirroring disabled is reported as `FC_INACTIVE`, without comparing intentionally
inactive copies. A single FAT has no other copy to compare. The half-open range
`[first, first + count)` must fit `cluster_count + 2`; zero count is allowed.
Reserved high nibbles and FAT padding are excluded. At most one active-copy read
and one read per additional FAT are needed per covered FAT sector. Requests
whose comparison count exceeds `UINT32_MAX` return `F_RANGE`; split those into
smaller explicit ranges. This bounds both caller-selected work and accounting.

### Selecting a FAT copy

Checks that traverse FAT links use the mounted identity's `active_fat`, just
like normal reads. Mirrored mounts select FAT 0; with mirroring disabled, the
BPB chooses the active copy, including FAT 1. Chain, file, directory, name and
ownership checks therefore already work with a volume whose active copy is
the second FAT. Reserved-entry checking also follows that selection.

`fat_view_open` provides an explicit alternate interpretation without swapping
FATs or changing the source identity. Its arguments are a zeroed/closed
`FatView*`, mounted source identity, zero-based FAT-copy number and a separate
three-sector workspace. The 216-byte view contains its own identity and a
read-only provider descriptor. It shares source geometry, OEM mapping and the
provider's read callback/context; its caches, handles and reports are separate.
Opening performs no I/O and imports neither mount nor other checkers.

```c
FatView second = {0};
FatWorkspace scratch = {buffer, 3 * source.sector_bytes, 0};
FatCheck result;
int status = fat_view_open(&second, &source, 1, &scratch);
if (status == F_OK) {
    status = fat_check_chain(&second.identity, first_cluster, budget, &result);
    /* Consume/copy the scoped result before closing. */
    fat_view_close(&second);
}
```

Use `second.identity` with existing checkers, or attach a separate shared volume
and read handles for file/directory/name checks and data reads. Copy selection
does not judge which copy is authoritative. It works for mirrored volumes and
intentionally inactive copies, and includes accepted provider staging. A view
has `mirrored=0`; use the source identity for `fat_check_mirrors`. That checker
compares mirrored copies, but does not walk each file independently and still
reports inactive mirroring without comparing intentionally inactive FATs.

The caller holds the source's serialization/maintenance lease for the complete
view lifetime. Do not mutate, commit, discard, invalidate or remount the source,
change its provider/geometry, or permit an external writer while views exist.
Serialize view and source reads through that same lease; a separate gate on the
view is insufficient. This is a temporary diagnostic interpretation, not an
immutable snapshot or a second writable cache owner. Source provider/context,
OEM map, view and scratch stay live and disjoint. No heap or base state is added.

The view clears write, begin, end, flush and provider-extension capabilities.
Ordinary mutation APIs cannot write through it. As elsewhere, directly editing
bookkeeping or replacing its provider violates the contract. A raw backend
pointer retained elsewhere is not restricted by this interface.

`fat_view_close` performs no I/O, retires only the view and invalidates its
handles/reports. Drain operations first; keep referenced storage alive until
old handles/reports are discarded. Closing a closed view returns `F_STALE`.
Reopening advances its own generation, so old evidence cannot become current.
Generation exhaustion returns `F_LIMIT` instead of wrapping. Open failures
leave the entire output unchanged: live output/active source transaction gives
`F_BUSY`, unmounted source `F_STALE`, invalid copy `F_RANGE`, undersized scratch
`F_MEMORY`, and invalid scratch/reserved fields `F_ARGUMENT`.

## Outcomes and evidence

`status` is the returned `F_*` value, `scope` identifies the check, and `issue`
locates the reason. `sector` is a volume-relative evidence LBA, or `UINT64_MAX`
when no sector applies. `cluster`, `observed` and `expected` are meaningful for
the selected issue. `FC_SHORT` uses `count` and `expected` as actual/required
cluster counts. Chain `count`/`examined` include the successfully read prefix;
`last` is published only at valid EOC. Mirror `examined` counts comparisons.

- `F_OK`: the stated scope passed. Flags can record absent/inactive metadata or
  excess allocation; they must not be discarded when interpreting the result.
- `F_CORRUPT`: a concrete link, size, reserved-value or redundancy contradiction.
- `F_ATTENTION`: dirty shutdown or recorded I/O-error history. This is evidence
  that further checks may be useful, not a claim that structure is damaged.
- `F_LIMIT`: a chain check reached its caller-supplied maximum links before
  reaching a conclusion. Zero budget performs no FAT access for a nonempty
  chain. A new check may use a larger explicit budget; there is no hidden retry.
- Provider failure: returned with `FC_IO` and the failing sector/cluster where
  applicable. A diagnostic read does not make physical write completion known.

FAT masks, backup pointer semantics and padding exclusions follow Microsoft's
[FAT specification, pages 12–13 and 18–19](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf).

Reads go through the selected provider, including any caller staging/cache.
Reserved and redundancy checks refresh the library cache as needed; chain
checks can use coherent cached FAT sectors. This is structural validation, not
a device-cache bypass, persistence test or proof of power-loss survival.

## Reusing a file report

Initialize a `FatCheck` to zero before using it with `fat_read_checked`. The
wrapper's request is `{ report_pointer, maximum_links, reserved_zero }`.
It validates if the report is absent/stale, refuses a current failed/incomplete
report, and otherwise calls `fat_read_at`. `transfer.done` is zero on a check
failure; ordinary read errors still report their completed prefix.

A file report records identity/generation, volume/epoch, canonical object and
incarnation, and chain version. Another handle to the same object can reuse it.
Unrelated file mutations, rename and in-place content changes preserve its
**chain-and-length** scope. Size/chain changes stale it. Explicit resize also
stales it at equal logical size, because excess allocation can be trimmed.
Last-close/reopen, pool
slot reuse, owner reinitialization and remount also stale it. The caller keeps
these state objects allocated while comparing a report; reports own no pins.
Raw-chain and volume-metadata reports are immediate observations and are not
reusable capabilities. Directory reports are also immediate observations;
`fat_check_fresh` deliberately does not reuse them. No whole-volume healthy bit
is inferred from any of these reports.

```c
FatCheck evidence = {0};
FatCheckedRead checked = { &evidence, 4096, 0 };
/* Both operations require the usual volume lease. */
status = fat_read_checked(&opened, &transfer, &checked);
/* A second call reuses current chain evidence; it does not rescan the file. */
```

A current `F_LIMIT` or I/O result is not automatically retried just because the
request supplies a new budget. Explicitly call `fat_check_file` for a deliberate
new attempt. Current diagnosed damage is refused by the checked-read wrapper.
Standalone checkers do not change base operation policy; callers discovering a
fault must route subsequent work through the appropriate policy/recovery path.
Neither wrapper grants permission to mutate known damaged media. The separately
selected ordered provider refuses further operations after uncertain physical
commit; see [ORDERING.md](ORDERING.md). Mirror-write disagreement rejection is available through the optional
`fat_put_checked` mutation hook; ordinary updates do not compare old copies.
The optional [session policy](POLICY.md) retains scoped adverse findings across
consumers and can restrict future operations. Repair remains separately explicit.

## Read-first diagnostic policy

`fat_read_adaptive` uses the same request layout as the checked reader. Initialize
the report to zero and hold the usual volume lease across the whole call.

The ordinary read runs without file preflight. Only `F_CORRUPT` or `F_IO`
triggers `fat_check_file`, once, with the caller's link budget. The function
returns the **original read status** and preserves `transfer.done` and its
completed output prefix. A successful chain check cannot certify unread data
or hide an I/O failure. The diagnostic status is separately available in the
report. There is no data retry, write, repair or escalation to another scanner.
Zero diagnostic budget permits no FAT link reads.

An unchanged current `FC_FILE` report of `F_CORRUPT` refuses a subsequent read
with zero completed bytes and no I/O. Another handle to that same canonical
file shares this restriction; unrelated files and stale reports do not. All
other attempts clear the report before ordinary I/O. `report.scope == 0`
therefore means no diagnosis was performed, not checked-good health. EOF,
argument errors, stale handles, busy and resource statuses cause no diagnosis.

This wrapper does not recover data or repeat failed writes. It supplies a
bounded diagnostic fallback while retaining ordinary partial-read semantics.
The base reader never references it, and selecting it does not retain the
checked-preflight wrapper, metadata/mirror scans, mutation code or recovery state.

## Bounded directory diagnostics

`fat_check_directory(directory_handle, slot_budget, report)` requires a current
readable directory handle and no active provider savepoint. It does not allocate,
write or repair. The existing sector caches and 576-byte local cursor hold the
scan and at most 260 UTF-16 code units; no mounted/shared structure grows.

The budget counts physical 32-byte directory slots, including deleted entries,
LFN fragments and the first unused entry. Zero budget performs no I/O. Exhaustion
returns `F_LIMIT`/`FC_BUDGET`, including when a partial LFN sequence still needs
more slots. At a completely full directory's cluster boundary, discovering EOC
requires remaining budget even though the FAT read consumes no directory slot.
There is no unbounded chain preflight or automatic retry.

The scan stops at the first zero first-byte marker (`FC_ENDMARKER`) or EOC. It
checks legal starting-cluster ranges, nonempty-file/zero-cluster contradictions,
zero directory sizes, incompatible directory/volume attributes, root-only unique
volume labels, and required dot/dotdot records at the start of non-root
directories. The parent link is checked against the canonical parent, with zero
representing the root parent. Unexpected dot entries elsewhere are rejected.
Encountered malformed links and detected cycles have separate diagnostics.

LFN checks cover ordinal order, LAST placement, checksum association with the
following short entry, orphan fragments, termination/padding, length through
255 UTF-16 code units, surrogate pairing and the library's component-name rules.
They work across sector and cluster boundaries. Deleted fragments are ignored;
an active sequence interrupted by a deleted/end entry is an orphan. Ordinary
enumeration still permits its existing SFN fallback for invalid LFNs.

Reserved high attribute bits are ignored. A nonzero LFN type or first-cluster
field returns `F_ATTENTION`/`FC_EXTENSION`, identifying an unsupported encoding
without declaring its future meaning corrupt. These distinctions follow the
[FAT specification's directory rules and implementation notes, pages 25–34](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf).

`examined` counts inspected slots and includes a malformed slot when it was
read. For an entry fault, its zero-based logical index is `examined - 1`.
`count` counts accepted live short entries, including labels and dot entries;
LFN/deleted/end-marker slots do not increment it. `first` identifies the
directory start, and successful `last` identifies its last scanned cluster.
`sector` identifies a loaded slot's volume-relative sector, or `UINT64_MAX`
when no definite slot sector applies (including failed cursor I/O). `cluster`
identifies the cursor cluster. `FC_DOT` cluster mismatches report observed and
expected clusters; `FC_EXTENSION` reports the nonzero extension field.

`F_OK` establishes only this logical-directory scope. The scan does not recurse,
follow child file chains, check duplicate names or all OEM short-name characters,
validate timestamps, inspect allocation beyond an end marker, compare FAT copies,
or prove cluster ownership. Use the separate chain/mirror/file functions for
their stated scopes. Reports own no references and cannot act as reusable
permissions to mutate a directory.

## Bounded namespace diagnostics

`fat_check_names(directory_handle, request, report)` checks that distinct entries
in one directory do not share a lookup name. It first calls `fat_check_directory`
with `request.slot_budget`; any failed or incomplete structural check returns
that original `FC_DIRECTORY` report without using the name workspace. It then
enumerates the validated directory under the same uninterrupted volume lease
and stable provider view. There are at most two passes over the bounded logical
slots, with no recursion, child-chain scan, writes or repair.

The 24-byte `FatNameCheck` request supplies:

| Field | Meaning |
| --- | --- |
| `entries` | Caller-owned temporary `FatEntry[capacity]`; 592 bytes per entry |
| `capacity` | Maximum retained entries, including dot/dotdot, excluding volume labels |
| `slot_budget` | Maximum physical slots in each pass, with the directory check's EOC rule |
| `pair_budget` | Maximum distinct-entry pairs compared; each pair compares at most four bounded names |
| `reserved` | Zero |

Workspace is separate from library state, arguments and report, and needs no
initialization. A zero capacity permits a null pointer and accepts an empty
root; encountering an entry then returns `F_MEMORY`/`FC_WORKSPACE`. Exact
capacity needs no extra sentinel entry. Only `report.count` workspace entries
are valid observations after the name pass. They are disposable snapshots and
grant no mutation permission. Nonzero capacity requires valid storage; null,
reserved-field and address-wrap errors are rejected without I/O.

Each entry's decoded name and, when present, short alias are compared with all
earlier entries. This uses exactly the library's lookup rule: ASCII case is
ignored, other UTF-16 code units compare exactly, and short names use the
mount's OEM mapping and NT case flags. It catches LFN/LFN, SFN/SFN and both mixed
collisions. One entry may have identical LFN and alias. This is a proof about
this library's lookup semantics, not every host filesystem's Unicode folding.

Name-pass results have scope `FC_NAMES`. `examined` counts compared entry pairs,
`budget` is the pair budget, and `count` counts entries accepted into workspace.
Exhaustion returns `F_LIMIT`/`FC_BUDGET` before comparing another pair; zero
budget permits zero or one entry. The scan is quadratic in entry count, bounded
by both supplied budgets and workspace, and retains no per-mount index.

`F_CORRUPT`/`FC_DUPLICATE` identifies the later SFN's volume-relative `sector`
and containing `cluster`. `observed` is its zero-based logical SFN slot index;
`expected` identifies the earlier conflicting SFN's slot. Neither entry is
declared authoritative. Workspace exhaustion reports `observed=capacity` and
`expected=count+1`, a lower bound on required capacity. `first` is the directory
start; successful `last` is its last scanned cluster. `FC_ENDMARKER` retains the
structural pass's termination evidence. A failed read has unknown `sector`;
success also leaves `sector=UINT64_MAX` because there is no offending entry.

These reports are immediate observations. `fat_check_fresh` does not reuse
`FC_NAMES`. Success establishes structure and name uniqueness only within this
directory and the documented comparison rule; it does not establish child
allocation, global ownership, timestamps or exclusive mutation safety.

## Bounded volume ownership

`fat_check_ownership(identity, request, report)` follows all live short-entry
references from the root through the selected FAT, then inventories unclaimed
data-cluster entries. Hold one volume lease and exclusive stable provider view
for the whole call. The function makes no writes, allocates nothing, and does
not change open objects or mount geometry. It uses the existing sector caches.
Accepted pending sectors are read through the same provider, so the report
describes the current logical view rather than claiming physical commitment.

The 40-byte `FatOwnershipCheck` supplies separate caller-owned workspace:

- `owners`: `FatOwner[owner_capacity]`, with at least `identity.cluster_count`
  entries. Each eight-byte entry identifies an owning SFN by 32-bit sector and
  byte offset. Cluster `c` uses index `c-2`; zero/zero is unclaimed and all-ones
  identifies the root. The scanner clears the required entries before use.
- `directories`: `FatDirectoryTask[directory_capacity]`, 16 bytes per discovered
  directory including root. The bounded breadth-first queue retains first and
  parent clusters and the owning SFN. It needs no recursive call stack or shared
  handle pool entries. The capacity is the total directories, not nesting depth.
- `fat_budget`: maximum attempted FAT-entry accesses, including cache hits,
  failed reads and directory-chain rewalks. Exhaustion is `F_LIMIT`/`FC_BUDGET`.
- `slot_budget`: maximum physical 32-byte slots across all directories, including
  deleted entries, LFNs, dots, labels and zero markers. Exhaustion is
  `F_LIMIT`/`FC_SLOT_BUDGET`. Discovering EOC after a full cluster requires
  remaining slot budget, although the FAT access consumes only FAT budget.
- `reserved`: zero.

All workspace, request, report and library storage must be valid and disjoint.
Capacity, reserved-field, null and address-wrap checks precede I/O and workspace
clearing. Zero FAT budget returns without either I/O or clearing. With a positive
budget, clearing costs eight bytes per data cluster regardless of subsequent
progress. Zero slot budget permits the budgeted root-chain inspection but no
directory-slot read. Insufficient owner/root-queue capacity returns `F_MEMORY`
before I/O; a full queue during traversal stops before inspecting that child
chain. Workspace contents after any outcome are disposable diagnostic state.

Each reachable chain is claimed in full, including allocation beyond a file's
logical length or a directory's first zero marker. Repeated ownership by the
same entry is `FC_CYCLE`; ownership by two different entries is `FC_CROSSLINK`.
File chains shorter than their declared size return `FC_SHORT`; extra file
allocation sets `FC_EXTRA`. Encountered free, bad, reserved or out-of-range links
in a reachable chain return `FC_LINK`. Empty files may have no chain.

The directory walk enforces the cluster-bearing entry geometry, legal dot and
dotdot relationships, and root-only volume labels needed to interpret ownership.
LFN records do not own clusters. Nonzero LFN type/cluster extension fields return
`F_ATTENTION`/`FC_EXTENSION`; ordinary LFN checksum/order/name damage remains
outside this scan. Deleted entries and entries after the first zero marker do
not own allocation. Namespace uniqueness and timestamps are separate checks.

The final inventory skips already claimed clusters and reads every remaining
data-cluster FAT entry. Zero means free; `0x0FFFFFF7` means an explicitly marked
bad cluster, which is counted separately and is not an orphan. Other legal
allocated values are unowned allocation. Invalid unclaimed link values stop the
scan with `FC_LINK`. The scanner does not group orphan chains, select a surviving
owner, recover a filename/length or repair any allocation.

The 160-byte `FatOwnershipReport` begins with a `FatCheck` of scope
`FC_OWNERSHIP`. `check.examined` counts attempted FAT accesses, `check.budget`
is the FAT budget, and `check.count` counts claimed clusters. Additional fields
count slots, discovered directories, completed file chains, free clusters, bad
clusters and orphan clusters. `FC_WORKSPACE` reports observed capacity and the
minimum required capacity for the failed admission. On `FC_SLOT_BUDGET`,
observed/expected give consumed slots and their limit.

`owner_sector/owner_offset` identify the current SFN; a cross-link also supplies
`other_sector/other_offset`. A sector of `UINT64_MAX` means root or no current
owner. `check.cluster` identifies the offending data cluster, and `check.sector`
identifies an attempted FAT or directory-sector read, or `UINT64_MAX` when no
sector was read (including a collision found in workspace). A cycle needs only
the current owner. On chain errors, `first/last` describe the current chain;
`FC_SHORT` gives observed/required cluster counts. `FC_LINK` following a FAT read
gives its invalid value in `observed`.

`F_OK` completes the ownership and allocation inventory. A complete inventory
with orphan allocation instead returns `F_CORRUPT`/`FC_ORPHAN`, sets `FC_ORPHANS`,
and identifies the first orphan's cluster/FAT sector and total orphan count.
In both complete outcomes, claimed + free + bad + orphan equals the volume's
data-cluster count; `first` is root and `last` is the final data-cluster index.
Other outcomes are partial: their counters are not whole-volume totals. These
reports are immediate observations, rejected by `fat_check_fresh`, and grant
no mutation capability. FAT mirrors, reserved/status entries, backup geometry,
file content and physical durability remain outside their scope.
