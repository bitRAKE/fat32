# Validation

This document records coverage and summarized results. Raw device captures,
session manifests, compiler transcripts, and local paths are kept outside the
published source tree. The test tools regenerate those artifacts locally.

## Automated tests

`build.cmd test` passes **28 suites** against the assembled library. C harnesses
compile with `/std:c17 /W4 /WX`. Fixtures independently encode boot sectors,
FATs, directories, and fragmented chains. Sparse providers model FAT32 cluster
counts without allocating whole volumes.

Coverage includes:

- 512-byte sectors with 512-byte, 32 KiB, and 64 KiB clusters; 4096-byte sectors
  with 64 KiB clusters.
- Geometry/extent rejection, free/bad/reserved cluster links, cycles, invalid
  directory clusters, FAT mirror disagreement, and active-FAT selection.
- Reads, overwrite, extension, truncation, deletion, zero-filled gaps, preserved
  surrounding bytes, attributes, packed timestamps, and 4 GiB file-size rejection.
- All long-name lengths 1–255 UTF-16 units, surrogate pairs, invalid components,
  damaged LFN sequences, short-alias fallback, directory growth, and nesting.
- Fragmented and backward-linked chains, high-nibble preservation, and file data
  beyond a 4 GiB device-byte offset.
- Allocation exhaustion and injected failures at each staging write in create,
  write, shrink, remove, rename, and metadata changes; rollback preserves the
  previous overlay and caller entry.
- Sector-buffer bounds, savepoints, discard, commit, allocation limits, and
  poisoning after backend write/flush failures.
- Actual Win32 unbuffered read/write/flush against a disposable sparse file.
- Nonvolatile GPR preservation across all 26 public exports, callback stack
  alignment, and deliberate volatile-register clobbering.
- Stale FAT caches after external writes, followed by quiescent invalidation.

## Physical interoperability

Successful device tests used 512-byte sectors with each cluster size below.
These are recorded runs, not an exhaustive hardware qualification.

| Cluster size | Recorded result |
| --- | --- |
| 512 bytes | A 45-file committed Git corpus matched through raw and native reads after six pair-write/delete passes: 86 writes and 41 deletions. Six files were fragmented, with up to 12 extents. |
| 32 KiB | Native boundary fixtures, two complete library mutation passes, and clean non-repair CHKDSK. |
| 64 KiB | 18 native fixtures totaling 2,065,923 bytes matched expected hashes; two mutation passes and a directory-boundary growth test passed. A locked independent FAT/hash oracle and immutable capture replay agreed with Windows. |

The mutation workload covers creation, repeated flushes, a cluster-boundary
patch, zero-filled growth, Unicode rename, packed metadata, truncation, deletion,
and native verification between phases. The 64 KiB directory test starts with
2,045 native short-name files and grows through a long-name set crossing the
cluster boundary. [CLUSTER64.md](CLUSTER64.md) describes the relevant arithmetic.

Earlier testing exposed a Win32 adapter lifecycle issue: flushing after dismount
returned `ERROR_NOT_READY`. The adapter now holds its lock through commit/flush
and dismounts on close. A separate transient post-format unlocked-read failure
was followed by clean locked capture and fresh-read results. It is consistent
with concurrent volume initialization; that event's precise cause was not proven.

An original malformed-directory observation could not be replayed after the
volume was reformatted. Its origin remains unknown. Synthetic reconstruction
rejects the malformed pattern independently of cluster size.

## UEFI example

`build.cmd examples examples-test` assembles the example and passes its independent
host firmware mock. Coverage includes the five-argument callback, 64-bit LBA and
status, aligned bounce buffers, unaligned caller output, mount/read/EOF, bounds,
and offline/media-change handling. The example has not been boot-tested under
firmware; the [integration guide](examples/uefi/README.md) describes bring-up.

## Generated code

The interface/code review reduced the combined library `.text` size from 10,754
to 10,038 bytes and decoded RET count from 115 to 55. All 53 procedures' saves,
stack allocations, restores, and returns were matched against their unwind
records. These are static code-size measurements.

`build.cmd verify` checks unwind information, undefined core symbols, and example
imports. The FAT32 object has no undefined external symbols. The Win32 assembly
example imports only KERNEL32.

## Limits

This validation does not establish crash/power-loss durability, global cross-link
repair, non-ASCII case folding, cross-parent moves, a full media surface scan,
or exhaustive controller/firmware coverage. Physical tests require explicit
local device identities and keep their detailed artifacts in ignored storage.
