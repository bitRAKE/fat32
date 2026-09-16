# Optional reusable reads

`fat_stream_open(handle, stream, workspace)` builds a caller-funded cluster map
for a regular file's logical length. `fat_stream_read(stream, transfer)` uses
that map through the mounted sector provider. `fat_stream_read_range(stream,
transfer, range_ops)` additionally batches contiguous full sectors. The ordinary
shared read path and its structures do not reference or contain this module.

`fat_stream_sector(stream, sector_index, out_lba)` optionally exposes a mapped
logical sector without I/O. The returned LBA is volume-relative and must be
used through the same logical provider view, including accepted staging. It
checks the handle and chain version, returns `F_END` at/past logical EOF, and
leaves output unchanged on failure. The final sector can contain bytes beyond
logical EOF; mapping it does not authorize exposing those bytes as file data.
This is address translation, not a claim of exclusive ownership or validation
of excess allocation. Handoff uses it for its explicit direct-read control.

## State and lifetime

The caller supplies a zero/closed 72-byte `FatStream` and a 16-byte
`FatStreamWorkspace = {uint32_t *map, uint32_t capacity, uint32_t reserved}`.
Reserved is zero. Capacity must cover `ceil(file_size / cluster_bytes)` IDs;
an empty file permits zero capacity and a null map. Insufficient capacity fails
with `F_MEMORY` before map traversal. There is no fixed 512-cluster limit, heap
allocation, whole-volume map or mandatory per-handle buffer.

Setup checks each required cluster ID and follows only the links needed for
the file's logical length. It does not audit excess allocation, exclusive
ownership or the rest of the volume. Deep validation remains separately
selectable. A failed setup can have written a prefix of the workspace, but it
does not acquire a stream reference. A successful setup owns an independent
read handle, so closing the input handle does not close the stream. Keep its
map alive, unmodified and separate from transfer output until stream close.

The map follows the canonical object's chain version. An unrelated file write,
same-size content overwrite, metadata update, rename or successful commit does
not stale it. Reads see current coherent bytes, including accepted staging;
the map is not a content snapshot. Changes to the mapped file's size/allocation,
remount, external invalidation, or uncertain commit make reads return `F_STALE`
before I/O. Close and open again to rebuild. `fat_stream_close` releases the
owned reference and never frees caller memory. An open stream prevents unlink.

Hold the same volume operation lease across setup/read/close and every provider
callback. Each consumer may own a stream; their maps do not duplicate the
volume's FAT/directory caches. The optional `fat_call_locked` can dispatch these
functions. No callback may reenter its owner while an operation is suspended.

## Transfers and range callback

Both reads use `FatTransfer` with an explicit offset and leave handle positions
unchanged. They clamp to current file size; EOF and zero length succeed with
zero completion. `done` counts the completed byte prefix even on I/O failure.
Output beyond that prefix is unspecified if a failing provider touched it.
There are no FAT/directory reads during a valid stream read. Unaligned edges
use one existing mount scratch sector; full sectors can go directly to output.

`FatRangeOps = {context, read, maximum, reserved}` occupies 24 bytes. Maximum
is a nonzero sector count and reserved is zero. Its callback uses Win64:

```c
int read(void *context, FatRangeRequest *request);
/* request: uint64_t lba; void *data; uint32_t count, done; */
```

The callback accepts arbitrary output alignment, reads volume-relative LBAs
from the **same logical view** as the mounted provider, preserves input fields,
and reports the completed prefix in sectors. It must not exceed count/extent.
A success with a short count or a count greater than requested becomes `F_IO`.
Backend failure is propagated with the confirmed prefix. No retry is hidden.
The library never calls it for an unaligned partial sector or across a
fragmented boundary; it caps each request at the advertised maximum. A range
callback may implement a bounce buffer or a transport-specific smaller limit.

## Coherent ranges with pending writes

For an ordered provider, `FatOrderRange = {FatOrder *order, FatRangeOps *backend}`
is a 16-byte caller-owned descriptor. Set a logical range table's callback to
`fat_order_read_range` and its context to that descriptor. The backend range
table must read the same volume as `order.backend` and remain alive with the
descriptor. The adapter serves pending sectors from the order index and batches
only intervening unstaged sectors through the backend. It also respects the
backend's own maximum, even if the outer range table advertises a larger one.

Do not point a stream directly at raw backend ranges when accepted data is
pending. This optional adapter implements that coherence in the library. It
rejects a commit-in-progress context, an uncertain provider and out-of-extent
requests, and preserves partial completion through mixed pending/backend runs.
The ordinary commit and ordinary stream profiles do not retain this adapter.

## Evidence and footprint

Tests cover 512-byte and 64 KiB clusters, unaligned output and file offsets,
EOF, same-file overwrite, unrelated changes, rename, rolled-back growth, stale
maps, caller capacity/overflow/canaries, failed/short/overreported callbacks and
hostile volatile-register clobbering. A 600-cluster independently encoded file
has a contiguous run followed by backward-fragmented storage. Setup traverses
599 links with eight FAT-sector reads. Its 307,183-byte steady read uses 304
range calls and one partial edge sector, with zero metadata reads. These are
request counts in a synthetic provider, not a hardware throughput claim.
Both stream profiles also pass the sixteen deterministic image fixtures alongside
the other observation profiles. A short
required chain fails map setup; an unexamined excess tail is not called healthy.

[VALIDATION.md](VALIDATION.md) records current linked code sizes and profile
exclusions. Basic readers do not retain stream or range implementations.
Mandatory mount/shared state remains unchanged; optional memory is 72 bytes
per stream plus four bytes per mapped cluster and a 24-byte range table.
Compressed extents or lazy cursor maps can be future alternate interfaces;
neither is required by the basic reader.

The map-only profile retains shared open/close and stream setup, but no stream
data reader. The other profiles do
not retain `fat_stream_sector`. Tests cover empty files, partial final sectors,
out-of-range indices, stale maps and the 600-cluster fragmented fixture.
