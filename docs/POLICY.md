# Optional session policy

`fat_policy_init`, `fat_policy_call`, `fat_policy_note` and `fat_policy_close`
wrap a shared volume without adding fields or calls to its basic operations.
Their public declarations are in `fat32.asm` and `fat32.h`; implementation is
in `fat/policy.inc`. Each entry is independently selectable COMDAT ANY.

## Ownership and lifetime

Use one `FatPolicy` for all participating consumers of a `FatVolume`. The caller
supplies its 176-byte state and a bounded `FatCheck` ledger (112 bytes per entry).
Initialize zeroed or closed state against a current volume. Initialization
does no I/O and does not clear the ledger. Zero capacity is permitted.

Hold the usual volume lease across policy admission, the action, any diagnosis
and publication of evidence. `fat_call_locked` can provide that outer lease.
The wrapper is not a lock. Reentry into an active wrapper returns `F_BUSY`.
Keep the volume, identity, object pool and ledger alive until policy close.

The dispatcher trusts the caller's action classification and function pointers,
just like `fat_call_locked`. Route every ordinary operation through this policy;
direct calls or mislabeled callbacks can bypass it. Inspectors and diagnosis
callbacks must be read-only, bounded and obey the same lease. Do not overlap
requests, reports, ledger or data buffers. External writers require retirement
and reconciliation; cached observations cannot detect an unannounced writer.

## Calls and fallback

`FatPolicyCall` is 80 bytes. Its `action` is a four-argument `FatCall`; `kind`
is `FP_READ`, `FP_WRITE` or `FP_INSPECT`. Supply a current `subject` handle when
an ordinary read concerns a file. Once file findings exist, a read without a
subject returns `F_ARGUMENT`, because its safety cannot be scoped to a file.
`reserved` must be zero. `invoked` and `diagnosed` are output booleans.

An admitted operation calls its action exactly once. Success, EOF, argument,
resource, stale-handle and busy results do not trigger diagnosis. Normal reads
and writes perform no audit I/O. Optional `diagnosis` and `report` must either
both be supplied or both absent. Only an original `F_CORRUPT` or `F_IO` invokes
that callback, once, while the policy binding remains current. The report is
cleared immediately before diagnosis so a failed callback cannot reuse old
evidence. The callback writes a standard `FatCheck` report; its supplied budgets
and workspace determine the diagnostic cost. There is no scanner registry or
automatic progression through increasingly expensive checks.

The action's original status and completed output remain authoritative.
Diagnosis never retries an operation or converts its failure to success. An
admission refusal invokes neither callback and leaves action-specific buffers
untouched, including a transfer's previous `done` value. Inspect `invoked` to
distinguish refusal from an action that completed no bytes.

```c
/* policy already initialized; hold the volume lease for this whole call */
FatCheck finding;
FatCall diagnosis = {(uintptr_t)fat_check_file,
    {(uintptr_t)file, 1024, (uintptr_t)&finding, 0}};
FatPolicyCall call = {0};
call.action = (FatCall){(uintptr_t)fat_read_at,
    {(uintptr_t)file, (uintptr_t)&transfer, 0, 0}};
call.subject = file;
call.kind = FP_READ;
call.diagnosis = &diagnosis;
call.report = &finding;
int status = fat_policy_call(&policy, &call);
/* finding is new evidence only when call.diagnosed != 0. */
```

## Retained restrictions

`fat_policy_note` consumes a diagnostic result obtained under the same stable
lease. It checks mount identity/generation; file evidence additionally needs
the same live shared object, incarnation and version. Other report scopes must
be immediate observations under this lease, not historical certificates.

| Finding | Subsequent ordinary operations |
| --- | --- |
| Current `FC_FILE` / `F_CORRUPT` | Refuse all writes; refuse reads of that file; allow unrelated file reads. |
| Other structural corruption, or `F_VERIFY` | Refuse all writes and ordinary reads. |
| `F_ATTENTION`, such as a dirty status observation | Refuse writes; retain reads for inspection/use under caller policy. |
| `F_OK`, `F_IO`, incomplete/resource results | Do not establish health or erase an existing restriction. |

The first adverse report is copied into `cause`. If an action first returns
corruption, attention or verification failure, `cause.status` records that
original result, `cause.scope` stays zero, and `cause_target` identifies the
action. Later findings never replace it. The ledger separately retains file
diagnoses. A bare action `F_CORRUPT` refuses writes and reads globally unless a
current, matching file diagnosis scopes a failed `FP_READ`. A failed mutation
cannot use file diagnosis to keep ordinary reads enabled. `F_IO` alone does
not prove corruption; the provider remains responsible for write uncertainty.

Each ledger entry pins its canonical file object. Closing and reopening user
handles therefore cannot discard a known-damaged file's identity. Duplicate
findings add no pins. Ledger exhaustion or reference overflow preserves the
write restriction and refuses ordinary reads globally (`F_MEMORY` or `F_LIMIT`
from `fat_policy_note`). Later healthy reports cannot clear restrictions.

`FP_INSPECT` permits explicitly chosen read-only diagnosis or salvage despite
these restrictions. It neither triggers fallback nor automatically notes a
report; call `fat_policy_note` separately when appropriate. It still requires
a current session. In particular, uncertain ordered commit retires the mount:
the original commit result/evidence survives, no diagnostic follows, and all
later policy operations return `F_STALE` without I/O. Inspect that medium using
a separately reconciled read-only mount/view.

Close releases ledger pins and retires the policy while preserving evidence
bytes. Closing stale state is allowed. Starting a new policy is an explicit
session decision after reconciliation, not a routine error-clearing operation.
No automatic FAT selection, repair, retry or recovery mutation is provided.

## Selection and cost

Only referenced callbacks bring their algorithms into the consumer. A policy
reader need not link a writer, formatter, scanner, salvage implementation or
durability provider. The feature matrix checks these exclusions with both
linkers, object/library input and release/debug linking. Existing consumers
retain their previous code and data sizes when this optional policy is unused.

The policy-only, shared-reader, and diagnostic-reader footprints are recorded
in [VALIDATION.md](VALIDATION.md), including probe/alignment costs. The four public
functions' own stack frames are
40/40/88/104 bytes for init/note/call/close, excluding entry return addresses,
callees and provider callbacks; these are not whole-call stack bounds. Ledger
admission is linear in its bounded entry count and performs no I/O. No heap
allocation occurs; each retained file consumes one reference in the existing
object pool until policy close.
