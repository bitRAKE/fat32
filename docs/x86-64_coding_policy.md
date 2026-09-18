# x86-64 coding policy

This policy guides assembly changes to the FAT32 library. It assumes x86-64
assembly knowledge and the library's Win64 calling convention. The objectives
are clear contracts, efficient use of processor resources, compact emitted code,
and correct behavior across provider callbacks. Remove unnecessary work and
dependencies so the processor can devote more of its finite capacity to useful
work. A source edit earns its place through its meaning and its assembled result;
fewer source lines alone establish neither size nor speed.

The procedure configuration is [common/policy.g](../common/policy.g). Public
contracts live in the root `.inc` interfaces and `fat32.h`; implementation
invariants are in [DEVELOPING.md](DEVELOPING.md). Apply the rules below to the
complete procedure, including macro expansion, error paths, and unwind data.

## Processor resources are part of the objective

Review what the processor must fetch, decode, track, execute, and retire on each
affected path. Retirement commits instruction results in program order;
even operations with no execution latency can require instruction-processing
capacity. Code size, operation count, and dependency latency describe different
demands. Improving any of them can leave resources available for useful work.

| Change | Resource rationale | What to check |
| --- | --- | --- |
| Use an existing argument register; remove a redundant copy | Removes an instruction from the instruction stream and its processing requirements | Move elimination may already remove execution latency; do not count every deleted MOV as an ALU operation saved |
| Reuse a loaded value; remove a redundant load | Removes address-generation and load work, queue demand, and a new load-to-use dependency | The retained value must have the required observation time and survive intervening operations |
| Shorten a value's lifetime; remove an unnecessary `uses` entry | Avoids the generated stack store and restore load on each affected invocation, as well as their instruction-processing demand | Inspect the complete frame and every return; an architectural register count alone does not measure physical-register pressure |
| Reuse incoming home space for private storage | Packs live stack state into a smaller address range, potentially touching fewer data-cache lines and reducing stack-access misses | Check complete frames, field alignment and address encodings; unchanged load/store counts can still benefit from better locality |
| Keep eligible flag writers next to their branch | Allows fusion to reduce the internal operations competing for dispatch, issue, and retirement | Eligibility depends on the processor, operands, addressing, and combined instruction length |
| Use shorter encodings and share epilogues | Reduces instruction footprint and can improve use of instruction-cache lines and fetch/decode windows | Byte savings alone do not establish fewer internal operations; shared exits can add a taken branch |

AMD's [Zen 4 Software Optimization Guide][zen4-sog], publication 57647,
revision 1.01 (April 2023), gives concrete mechanisms: instruction cache and
fetch/decode in sections 2.6.1 and 2.9 (pages 17 and 26), decoded-operation
cache in 2.9.1 (pages 26–27), fusion in 2.9.3 (pages 28–29), zero-delay moves
in 2.9.6 (page 30), dispatch in 2.9.8 (page 31), and load/store queues in
2.12 (pages 39–40). The [AMD Zen SOG archive][amd-sogs] provides guides for
other generations. These are processor-specific explanations for the policy;
the library's x86-64 interface does not require Zen hardware.

Apply this resource model to the actual expanded instructions. Shorter code
can fit a fetch window better while using the same execution resources. A
dependency improvement can matter at equal size. A larger sequence can be
worthwhile when it reduces work at a demonstrated bottleneck. Evaluate the
complete path and its frequency, including added branches and spills. Record
the resource mechanism separately from measured application timing; the
repository results below establish code and path changes, without a measured
whole-library speedup.

## 1. Establish the contract before selecting instructions

Public declarations must describe parameter order, width, direction, required
storage, ownership, lifetime, and optional/null cases. State return values,
partial completion, and which outputs remain unchanged on failure. A pointer
type alone does not describe an interface.

For an implementation helper, document the facts callers actually depend on:

- Which registers contain arguments and results, including nonstandard returns.
- Which memory it reads or changes: caches, generations, transaction state,
  caller buffers, and provider-visible data.
- Preconditions and useful postconditions, such as already-zeroed scratch space.
- Any private register-preservation guarantee stronger than the calling convention.

For example, `f_cluster_sector` returns an LBA in RAX, preserves RCX, and does
not return an `F_*` status or perform a range check. That contract justifies a
different call sequence from an ordinary public entry. Public functions can
also be replaced at link time; rely on their declared interface, not incidental
behavior of the current default. See [LINKING.md](LINKING.md).

Comments should explain constraints and intent: why a load must follow a call,
why an offset is a particular disk field, or why an error exit may reuse a
register. Prefer symbolic field names. If a raw format offset is necessary,
identify the field and the validation performed. A comment restating a `mov`
does not supply that information.

## 2. Use data where it already is

### Incoming registers and their saved copies

Saving an input for use after a call does not make the incoming register cease
to be useful before that call. In this sequence, RCX already holds the first
argument and RDX still holds the entry pointer:

```asm
mov rbx, rcx
mov r12, rdx
fastcall fat_dir_open, rcx, [rdx + FatEntry.parent], addr cursor
```

Passing RBX as the first argument would generate `mov rcx, rbx`. Reading the
parent through R12 would introduce a source dependency on its preceding copy.
Keep the saves only if their values are needed later. Direct use expresses the
actual dataflow and avoids unnecessary copies. When a copy disappears, its
instruction-processing demand disappears too. If only the source register
changes, the emitted instruction count may stay the same. Zen 4 documents
zero-delay register moves in [section 2.9.6][zen4-sog], so removing this source
dependency need not remove an execution cycle on that processor.

The September volume review applies the same principle to initial geometry
loads and stores in `fat_mount`, `f_directory`, and `f_allocate`. The equivalent
register choices do not all change instruction length.

### `fastcall` is an ordered sequence

The installed `macro/proc64.inc` processes ordinary arguments left to right:
RCX, RDX, R8, R9, then stack slots. It omits moves between identical registers
of the same width. It does not perform a parallel assignment or resolve cycles.
For example:

```asm
; Wrong if argument 2 needs the original RCX:
fastcall target, rbx, [rcx + offset]
; Expansion begins: mov rcx, rbx
; The subsequent load now uses RBX's address.
```

Preserve that old value before the macro, or use an existing saved copy.
Review the call target as well as its arguments: the indirect target is consumed
after argument preparation. Stack arguments, address materialization, and other
macro forms can use RAX. A target held in RAX is safe only when this particular
expansion leaves RAX intact. Nested calls inside argument expressions require
their own expansion review.

In `f_write_sector`, changing the callback table register from R9 to RAX is
safe for the three-argument dispatch: RCX receives the context, RDX and R8 are
already correct, and no stack argument uses RAX. The indirect call loses one
register-extension byte. This is a property of that call site, not a convention
that RAX always survives `fastcall`.

### Choose registers for their next use

`f_info_unknown` now loads the scratch-buffer address into R8, validates and
updates that buffer, then passes R8 directly as the write callback's third
argument. The former R9 choice required a second load into R8. Removing that
seven-byte load accounts for the entire procedure's size reduction. On each
path reaching the write, it also removes a load, its address-generation work,
and the outgoing argument's dependency on that new load. The already available
buffer address supplies the argument directly.

Reuse an existing register when its former value is dead on every reachable
path. `fat_get` can replace its identity pointer in RBX with the FAT-buffer
pointer on the successful tail. Its failed-read path still needs the identity,
so the overwrite must remain after the branch to that path.

## 3. Preserve the needed value, with the correct observation time

If only a field value is needed after a call, retaining that value can be better
than retaining its object's address and loading the field later. This shortens
the pointer's lifetime and can remove work. The choice is valid when the
operation needs the pre-call value, or the value is guaranteed stable across
the call.

Register preservation and memory stability are separate questions. A
nonvolatile register can faithfully preserve a value that has become obsolete.
Before moving a load, establish whether the required value is from before or
after the intervening operation. Include indirect writes, aliases, callbacks,
and permitted reentry in that reasoning.

### Repository example: snapshot generation

`f_snapshot_done` invalidates earlier snapshots, then gives a successful output
entry the identity's new generation. Retaining the generation before calling
`fat_invalidate` made the code smaller, but invalidation increments that field.
The returned entry consequently carried the old generation.

The correct ordering retains the identity address across the call:

```asm
mov rbx, rcx
; Save the output pointer and status in other nonvolatile registers.
fastcall fat_invalidate, rcx
; After checking successful status and a non-null output:
mov rax, [rbx + FatIdentity.generation]
mov [rsi + FatEntry.generation], rax
```

If invalidation left generation unchanged, retaining the value instead would
express the intended optimization. Here the post-call value is part of the
interface contract. Do not substitute a guessed increment for the reload:
that would duplicate another function's policy and constrain valid replacements.

The premature-load variant assembled successfully and retained valid unwind
data. The existing `test_basic` create/write/read sequence rejected it with
`F_STALE` on the read. Restoring the post-invalidation load passes all 95 suites.
This is why object inspection and behavioral testing are both necessary.

## 4. Allocate registers with encoding and lifetime in mind

The library uses static RSP-relative frames. RBP is available as an ordinary
nonvolatile register; writing it still requires preservation through `uses rbp`.
Prefer it for pointers or integers narrower than 64 bits when the complete
procedure encodes smaller.

RBP is useful for two distinct reasons: EBP operations can avoid an extension
prefix, and RBP addressing can avoid the SIB byte required by an R12 base. A
64-bit operand already requires REX.W, so replacing an extended register does
not necessarily remove a prefix. Representative assembled encodings are:

| Instruction | Bytes | Size |
| --- | --- | ---: |
| `inc r12d` | `41 FF C4` | 3 |
| `inc ebp` | `FF C5` | 2 |
| `mov eax, [r12+8]` | `41 8B 44 24 08` | 5 |
| `mov eax, [rbp+8]` | `8B 45 08` | 3 |
| `mov rax, [r12+8]` | `49 8B 44 24 08` | 5 |
| `mov rax, [rbp+8]` | `48 8B 45 08` | 4 |
| `mov rax, [r13+8]` | `49 8B 45 08` | 4 |
| `push r12` / `push rbp` | `41 54` / `55` | 2 / 1 |

There is no universal register ordering. `[rbp]` needs an encoded zero
displacement: `mov eax,[rbp]` is three bytes, versus two for `[rbx]`. BPL needs
a REX prefix. Additional saves, spills, changed branch lengths, and alignment
can outweigh local gains. Avoid dedicating a nonvolatile register to a value
that is consumed entirely before the next call.

### Width is part of the data contract

Use dword operations for dword state, and retain qword operations for pointers,
LBAs, generations, and products that require them. A 32-bit register write
zero-extends; byte and word writes do not establish the rest of the register.
Do not narrow pointers or signed arithmetic to obtain a shorter instruction.

The library's public interface uses the Microsoft x64 register convention:
RCX/RDX/R8/R9 for the first four integer arguments, EAX for `F_*`, and preserved
RBX/RBP/RSI/RDI/R12–R15. Only the declared part of a narrow argument is valid;
do not infer useful upper bits from a caller. Volatile registers must survive
neither a provider call nor a public replacement. These requirements follow
the [x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention).

### Separate LEA's result width from its address width

In 64-bit mode, these calculations produce the same low 32-bit result and
zero-extend it into RBP:

```asm
lea ebp, [esi+1]        ; 67 8D 6E 01: 32-bit address calculation
lea ebp, [rsi+1]        ;    8D 6E 01: default 64-bit address calculation
```

For this arithmetic, carrying out the addition modulo 64 bits and taking the
low 32 bits gives the same result as adding modulo 32 bits. High input bits
cannot change those low bits. The second form avoids the `67h` address-size
override without widening the result. Both leave flags unchanged.

The same reasoning applies to `lea edx,[rax+rax*2]` and the cluster calculation
`lea eax,[rdx-2]`. It does **not** justify widening a real memory reference:
`[esi]` and `[rsi]` can access different addresses. Nor does it justify a
64-bit LEA destination when the operation requires truncation.

## 5. Make status flow and side effects explicit

### Load the status before the condition

For a guard with a known error result, preload EAX and branch directly to the
common exit:

```asm
mov eax, F_ARGUMENT
cmp byte [r12 + FatEntry.raw], '.'
je .done
```

Keep CMP/TEST next to its conditional branch. For eligible pairs, fusion reduces
the internal operation count and frees dispatch, issue, and retirement capacity
without removing either architectural instruction. Loading a status between
the comparison and branch needlessly separates them even though MOV preserves
flags. [Zen 4 section 2.9.3][zen4-sog] describes this benefit and its conditions.

Adjacency alone does not guarantee fusion. In particular, that guide excludes
flag writers containing both an immediate and a displacement, as in the memory
guard above. It also restricts RIP-relative addressing and combined instruction
length. Keep the adjacency policy, then check the actual pair before claiming
a fusion benefit on a particular processor.

The status must remain live through the taken path. Use a different scratch
register if a guard needs a temporary value; the earlier `fat_set_info` review
used DL instead of AL to avoid damaging the preloaded result. On success,
produce the success value explicitly when it is not already established.

### Compact transfers with accumulator XCHG

`xchg eax,edi` encodes as `97h`, one byte shorter than `mov eax,edi` (`89 F8`).
It is useful when the old EAX value may replace EDI without consequence. It
overwrites both registers, including zero-extension of both 32-bit results,
and preserves flags. It is not a general substitute for a copy.

The revised `f_finish` carries status in EAX until it must survive the end
callback, then exchanges it into EDI. The final exchange returns that status;
the epilogue restores the caller's RDI. `f_snapshot_done` uses the same compact
return transfer. In both return cases, the displaced EAX value has no later use.

The compact encoding reduces instruction footprint. [Zen 4 section 2.9.6][zen4-sog]
also lists register XCHG among its zero-delay operations, including accumulator
forms. That supports its use here without assuming the same handling on every
x86-64 processor. Zero delay does not establish zero resource cost, and fewer
bytes do not establish fewer internal operations. No timing comparison of
these return sequences was made.

This policy concerns register/register XCHG; a memory XCHG has atomic locking
semantics and belongs to a different design decision. Instruction semantics
are specified in the Intel SDM's LEA and XCHG entries, available from the
[official manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

### Moving work across a failure point

In `fat_put`, moving `inc esi` before the write callback is valid because EAX
already holds the current FAT-copy index, ESI survives the callback, and the
failure exit does not observe the incremented count. Its flags are dead before
the later tests. The next successful iteration still sees the same index.

That is a liveness argument, not a rule permitting visible state updates before
success. Cache publication, generation changes, completed-byte counts, and
transaction state require their own ordering. Keep the failure path in view
when rescheduling instructions. The index move alone claims no size or speed gain.

## 6. PROC/ENDP dynamics

### The body determines the entry frame

`PROC` is a macro interface, not a hardware instruction or a fixed prologue.
The installed configuration combines `macro/proc64.inc` with NEWCOFF's debug
procedure wrapper:

1. `uses` lists registers to push in declaration order.
2. `locals` contributes local storage; initialized locals can also emit stores.
3. `fastcall` records the largest outgoing call area used by the procedure.
4. `ENDP` finalizes local and call-area sizes. Assembler passes resolve their
   forward use in the entry allocation and RSP-relative local addresses.
5. The debug wrapper records the prologue, frame allocation, saved registers,
   parameters, and locals; its close hook completes the procedure metadata.

The static-RSP prologue allocates locals, alignment, and the maximum outgoing
area once. Calls reuse it. The first four argument home slots need 32 bytes;
additional arguments and alignment may enlarge that area. Consequently, adding
one call can change the entry/exit instructions and every local displacement.
Keep RSP stable through the body rather than inserting untracked pushes or
temporary stack adjustments. For ABI constraints, see Microsoft's
[prologue and epilogue rules](https://learn.microsoft.com/en-us/cpp/build/prolog-and-epilog).

This layout also avoids repeated call-area adjustments and frees RBP for useful
data. Removing an unnecessary `uses` entry then removes actual save/restore
work as well as its encoding. Zen 4's stack-pointer tracking removes certain
implicit RSP dependencies ([section 2.9.7][zen4-sog]); it does not remove the
data store and load required to preserve a register. Count these memory
operations separately from stack-address dependency handling.

This assembled example has two saved registers, one qword local, and a
five-argument call:

```asm
proc frame_example uses rbx rbp
    locals
        value dq ?
    endl
body:
    mov rbx, rcx
    mov rbp, rdx
    mov qword [value], 0
    fastcall callback, rcx, rdx, r8, r9, 1
    fastcall callback, rbx, rbp
    ret
endp
```

The entry is `push rbx; push rbp; sub rsp,38h`. The fifth argument occupies
`[rsp+20h]`; `value` occupies `[rsp+30h]`. Both calls use the same allocation.
The return expands to `add rsp,38h; pop rbp; pop rbx; ret`. The corresponding
unwind record describes 56 allocated bytes, both pushes, and no frame register.
RBP can therefore hold the second input while locals remain RSP-relative.

Named parameters describe stack locations; declaring them does not automatically
store register arguments into home slots. Use incoming registers or explicitly
save them. Use named stack parameters for fifth and later arguments instead of
hard-coded offsets that become wrong when the frame changes.

### Use incoming home space for small private storage

Every Win64 call provides four eight-byte home slots, even when it has fewer
arguments. The callee owns these incoming 32 bytes and may repurpose them for
private storage. They remain available across nested calls. The separate
outgoing call area belongs to the next callee and cannot retain live values
across that call. See Microsoft's [stack usage rules](https://learn.microsoft.com/en-us/cpp/build/stack-usage).

The runtime aim is better data-cache locality. Packing live stack state into a
smaller address range can touch fewer cache lines and reduce stack-access misses
and interference with other data. Load/store instruction counts need not fall
for this benefit. The effect depends on the active call path, layout and cache
state; frame sizes alone do not measure cache misses.

The installed PROC macros reserve the declared size for each typed name. Use
that facility for packed scratch, while the ABI comments continue to identify
the actual register arguments:

```asm
; RCX=identity*, RDX=const FatEntry*. Incoming homes hold f_slot's output.
proc f_validate uses rbx rsi, location:8, offset:4
    assert offset+4-(parmbase@proc) <= 32
body:
    ; ... f_slot initializes the contiguous {qword sector, dword offset} ...
```

The names do not initialize storage or save incoming registers. Every field
must be written before it is read. Eight `:4` names can describe eight scratch
DWORDs within the four ABI home slots; actual stack arguments still use their
ABI positions. Preserve contiguous output layouts when reordering fields.
Parenthesize `parmbase@proc` in subtractions: it expands to an address expression.

For a small record, `home_struct` supplies its existing field names without
contributing to the ordinary local allocation:

```asm
proc f_read_record uses rdi
    home_struct request, FatTransfer, 0
body:
    ; ... initialize request.data, request.offset and request.length ...
```

Its final argument is a constant byte offset within incoming home space,
defaulting to zero. The macro rejects a negative offset or a record extending
past byte 32. Combine a record with packed scalar names only at disjoint offsets;
`fat_order_read_range` places `first:8` at zero and its 24-byte request at eight.
Keep the `body:` anchor after record declarations. These declarations generate
no initialization instructions.

`f_create` fills the first 32 bytes with private scalar storage and keeps its
real fifth argument, `ignore`, immediately afterward. Its assertion fixes that
argument at entry RSP+40 regardless of frame size. Declared scratch names are
therefore storage aliases, not an argument-count declaration for callers.

Measure the complete frame before adopting a relocation. Alignment can absorb
small local blocks: `f_slot` retains its four-byte local because moving it would
leave the same allocation. Moving only `first` and `limit` in `f_order_run`
already removes 16 stack bytes; leaving `mode` and `cursor` local keeps their
frequent accesses within short RSP displacements. Filling every home byte is
not the objective. Longer displacements can increase code size even while
stack consumption falls.

The fasm2 CodeView wrapper uses an unsigned 32-bit default for labels whose
declared size is four bytes; explicit type annotations remain authoritative.
Packed DWORDs retain their real addresses and 32-bit CodeView types. The harness
checks these records, eight packed fields, the real fifth argument, outgoing-home
clobbers, and the home-area boundary.

### RET is an epilogue expansion

Each operandless source `ret` emits the configured epilogue. A routine with
many saved registers and several early returns duplicates the entire restore
sequence. Prefer a common `.done` exit, with conditional branches or short jumps
from cold error paths. Keep common cleanup before that epilogue; paths requiring
different cleanup can converge after their obligations are satisfied.

Perform the size comparison on the expanded code. A bare leaf's one-byte return
can be smaller than a jump to a shared return. A framed routine can save much
more by sharing restores. Neither return count nor source-label count alone
measures that tradeoff.

Sharing an epilogue reduces static footprint; a returning invocation still
executes one restore sequence. Removing entries from `uses` reduces that
sequence's dynamic work. Keep these effects distinct, and account for any
additional branch needed to reach the common exit.

Moving `mov rbx,rcx` below the guards in `f_begin` avoids that body instruction
on rejected calls. It does **not** avoid the entry's `push rbx`: `uses rbx`
still applies to the whole procedure. Likewise, the early empty-chain case in
`fat_chain` still executes its generated frame setup. Actual selective saving
would require a different procedure structure and valid unwind treatment.

`ENDP` closes macro scope and metadata; it does not synthesize a missing return.
In these macros, `retn` and `ret` with an explicit operand bypass the ordinary
epilogue expansion. Do not use them as shorthand in a framed routine. Keep the
generated stack restoration and pops together; additional work belongs before
the epilogue.

### Preserve wrapper and namespace behavior

Forward the `uses` list as `<reglist>` through the policy wrappers. Flattening
it into separate macro arguments changes how the prologue/close macros receive
their inputs. The emitted saves and the unwind description must agree exactly.

Structure-valued locals can affect the scope of subsequent dot labels. The
repository uses a `body:` anchor after `endl` where necessary to establish a
consistent base for branches. Keep that anchor when editing the local layout.

Each current procedure has its own `.text$procedure` COMDAT. Its unwind and
CodeView contributions must remain associated with it. Changes to procedure
wrappers, exports, or section policy require linker and replacement tests as
well as ordinary execution tests.

## 7. Measured repository experience

These measurements establish emitted code and identifiable work removed from
execution paths. Timing depends on how those paths use the target processor's
resources. Object-file size also includes relocations and debug/unwind
information; linked size additionally depends on selected functions, alignment,
and linker settings.

### Earlier register and exit review: `86b1a4e`

Reassembling the commit and its parent with the same toolchain reproduces:

| Object scope at that revision | Before `.text` | After `.text` | Before / after RET instructions |
| --- | ---: | ---: | ---: |
| FAT32 library | 8,687 | 8,164 | 80 / 42 |
| Buffer adapter | 805 | 689 | 20 / 7 |
| Win32 adapter | 1,262 | 1,185 | 15 / 6 |
| **Combined** | **10,754** | **10,038** | **115 / 55** |

The 716-byte reduction came from combined register/call cleanup and shared
exits; it cannot all be attributed to the reduced return count. All 53 procedures
remain represented in the rebuilt unwind records. Representative changes were:

- `f_slots` reduced its saved-register list from six registers to four.
- `f_store` used volatile R9 for a scratch address instead of saving RDI.
- `f_read_sector` eliminated a nonvolatile save for a table pointer consumed
  before callback dispatch.
- `f_begin` and `f_finish` stopped retaining callback-table pointers across calls.
- `f_create` used the allocator's documented zeroed-buffer postcondition instead
  of zeroing the same buffer again.

The first four examples remove preservation work by shortening value lifetimes.
The last removes algorithmic work by using an existing postcondition. Both kinds
of improvement matter alongside instruction encoding.

### RBP review incorporated into `v1.0.0` (`60ae145`)

The retained before/after objects contain 149 procedures. Trying available
R12–R15 allocations as RBP and selecting smaller complete procedures reduced
their combined code from **27,796 to 27,150 bytes**, saving **646 bytes (2.3%)**
across 62 procedures. Examples include:

| Procedure | Substitution | Before | After |
| --- | --- | ---: | ---: |
| `f_order_run` | R12 to RBP | 915 | 868 |
| `fat_salvage_plan` | R13 to RBP | 664 | 632 |
| `fat_check_backup` | R12 to RBP | 305 | 275 |
| `fat_check_directory` | R15 to RBP | 1,286 | 1,260 |

The earlier review compared decoded instructions after register and branch-offset
normalization, relocation targets, saves, and allocation sizes. RBP remained a
saved general register, with no unwind frame-register assignment. The 27,796-byte
baseline was the expanded pre-review library, not the much smaller library in
the parent of the release commit. See [VALIDATION.md](VALIDATION.md) for that
review's behavioral and link coverage.

### Volume review after `v1.0.0`

The corrected September 16 review retains all 149 procedure sections and reduces
their total from **27,150 to 27,128 bytes**. The nine smaller procedures are:

| Procedure | Before | After | Reason for the reduction |
| --- | ---: | ---: | --- |
| `fat_mount` | 612 | 608 | Four address-size overrides removed from LEA |
| `f_write_sector` | 60 | 59 | Low-register indirect dispatch |
| `fat_get` | 129 | 126 | Direct guard exits and reuse of RBX on the successful tail |
| `fat_put` | 240 | 238 | Direct guard exit removes the separate status/jump tail |
| `f_cluster_sector` | 18 | 17 | Default address size for a dword LEA result |
| `f_info_unknown` | 146 | 139 | Buffer address already in the outgoing argument register |
| `f_finish` | 88 | 86 | Status flow and accumulator exchanges |
| `f_snapshot_done` | 47 | 46 | Accumulator exchange; generation load remains after invalidation |
| `fat_put_checked` | 280 | 279 | Default address size for a dword LEA result |

Other edits use incoming registers sooner or postpone work on error paths
without reducing total bytes. The premature-generation-load variant totaled
27,125 bytes but failed the existing read test; those extra three bytes of
apparent savings were rejected.

The corrected object preserves section names, relocation target lists, COMDAT
associations, and byte-identical unwind records relative to the release object.
The full 95-suite harness passes. The fresh historical and current builds used
fasmg `g.l8vn`; decoding used LLVM 22.1.4. These figures describe these source
revisions and macro definitions, not guaranteed sizes with every toolchain.

### Applying the policy across the library

The subsequent pass reviewed all 149 procedures and reduced code from
**27,128 to 26,975 bytes**, saving **153 bytes**. Forty procedures changed;
39 became smaller and one retained its size. No procedure grew. Including the
preceding volume review, the reduction from `v1.0.0` is **175 bytes**.

| Procedure | Before | After | Main change |
| --- | ---: | ---: | --- |
| `fat_dir_open` | 72 | 61 | Keep inputs volatile and retain the stable generation during cursor clearing |
| `fat_volume_init` | 281 | 267 | Preserve only the string destination register in this leaf |
| `fat_policy_init` | 186 | 172 | Consume incoming pointer/count registers without saved copies |
| `fat_view_open` | 307 | 295 | Keep copy/workspace arguments volatile until the final call |
| `f_check_owner_get` | 159 | 140 | Preserve only the report pointer across the callback; share restores |
| `fat_order_discard` | 56 | 48 | Order pointer is dead after reset; retain only the identity |
| `f_cursor_sector` | 180 | 174 | Preload stale status and remove its separate exit stub |
| `f_stream_prepare` | 80 | 72 | Common epilogue and direct stale guard |

This pass removed 20 remaining address-size prefixes from dword LEA calculations.
Prologue register saves fell from 415 to 397, and emitted returns from 161 to 156.
These are static totals across the library. Each removed save also removes its
matching restore from the affected invocation; five fewer encoded returns do
not mean five fewer returns executed by an operation.
All remaining multiple-return procedures are leaves without generated restores.
The named FSInfo layout in [fat/disk.inc](../fat/disk.inc) makes the reader and
formatter's signatures and adjacent hint fields explicit without changing their
emitted accesses.

`fat_dir_open` illustrates the useful value-lifetime transformation: generation
is read before clearing the disjoint cursor, then written to that cursor. No
call or generation-changing operation intervenes. The routine no longer retains
an identity pointer merely to load the generation afterward. Both versions load
generation once. The improvement removes two pushes and their two pops on every
invocation, four register copies on the successful path, and its redundant final
EAX clear. The 11-byte reduction accompanies less instruction-processing and
stack-memory work, with no need to assume a cycle cost for eliminated moves.

Frame size must still be measured as a whole. `f_check_owner_get` reduced four
pushes to one, while its fixed allocation grew from 40 to 48 bytes to preserve
alignment. Total entry stack consumption, excluding the caller's return address,
fell from 72 to 56 bytes. Counting only `sub rsp` would miss the improvement.

The object audit checks every prologue and return against its unwind records,
call alignment/home space, branch targets, relocation targets, and COMDAT
associations. The 95-suite run also supplies nonzero upper register bits for
four uint32 parameters: directory cluster, object-pool capacity, FAT-copy index,
and policy-ledger capacity. Register reuse must honor the declared low dword.
The linker matrices and independent image results are recorded in
[VALIDATION.md](VALIDATION.md).

### Lifetime and loop review against `b4b1c46`

The next pass examines all 22 implementation includes and all 149 procedures.
The submitted `fat_read_adaptive` edit exposes an argument-ownership error:
`FatTransfer` is in RDX, while `FatCheckedRead` is in R8. Checking the latter's
`reserved` field through RDX reads the high dword of the transfer offset. The
existing nonzero-reserved test fails on that version. The corrected code uses
R8; a complementary valid read beyond EOF with a nonzero high offset dword now
checks that the transfer cannot masquerade as the policy. Using the earliest
value means the earliest value of the **same object**.

Other submitted changes preserve their contracts. `fat_count_free` moves INC
before the value comparison; that CMP still supplies carry to ADC. The ordered
range reader keeps callback status in EAX and counts completed sectors in EDI,
so it can remove the status-transfer tail without changing failure precedence.
Keeping its requested count in ESI also removes a stack store and two possible
reloads: ESI already survives the callback by the public ABI.

Both FAT-write hooks now retain a running sector LBA in RSI and its exclusive
end in RBP. For the entry's sector offset `part`:

- Mirrored: `first = fat_start + part`, `end = first + fat_count * fat_sectors`.
- Selected FAT: `first = fat_start + active_fat * fat_sectors + part`,
  `end = first + fat_sectors`.
- Each successful copy advances `first` by `fat_sectors` and compares it unsigned
  with `end`. The endpoint includes `part` and is never read or written.

Mounted geometry bounds these addresses within the volume; the endpoint does
not require more than 32 bits for an accepted mount. The code uses 64-bit LBA
arithmetic to match the callback interface, with zero-extended geometry fields.
The loop removes two repeated address calculations per copy, retains the buffer
in outgoing R8 for the write, and advances through ECX to preserve successful
EAX. Setup still has a cost; normal one- or two-copy volumes do not imply a large
iteration saving. Sparse tests cover oversized FATs above the signed 32-bit LBA
boundary, one through 255 copies, and active-copy selection through index 15.
They check exact callback counts, high FAT-entry bits, neighboring entries,
staging, and cache invalidation for both hooks.

| Procedure | Before | After | Main change |
| --- | ---: | ---: | --- |
| `fat_put` | 237 | 221 | Running LBA/end pair; no local sector-offset spill |
| `fat_put_checked` | 279 | 265 | Same loop, retaining comparison and failure behavior |
| `fat_order_read_range` | 389 | 366 | Direct status exits and retained requested count |
| `f_order_read` | 102 | 90 | Keep the output in RDI through lookup and copy |
| `f_order_write` | 172 | 157 | Use the search helper's preserved arguments and returned slot |
| `f_read_bytes` | 284 | 269 | Reuse RSI for the sector index; keep copy length in EAX across REP |
| `f_nth` | 98 | 84 | Validate the next cluster once at the loop header, before use |
| `fat_dir_next` | 644 | 635 | Keep fragment/index temporaries volatile until their last use |
| `fat_salvage_plan` | 629 | 629 | Remove RSI preservation even though total bytes remain equal |

Procedure code falls from **26,975 to 26,748 bytes**, saving **227 bytes**.
Of 42 changed procedures, 31 shrink, seven retain their size, and four grow by
one byte when a direct incoming-register operand needs an extra prefix. Those
forms express the earlier available value without depending on its saved copy;
move elimination may already avoid the dependency on a particular processor.
No timing improvement is inferred from that choice.

Prologue saves fall from **397 to 372**, with matching restores removed. For
example, `f_order_write` now saves two registers instead of five; fixed allocation
grows from 32 to 40 bytes, while total entry stack consumption falls from 72 to
56 bytes. `f_read_bytes` removes its per-sector span store/reload as well as one
save/restore pair. The return count remains 156: several status tails disappear,
while the shared epilogues already provide a single return in those procedures.

The fresh baseline and revised objects pass all 149 frame, call, branch,
relocation and COMDAT checks. The **96-suite** run includes volatile callback
poisoning and the new argument/geometry cases; linker and independent image
checks are recorded in [VALIDATION.md](VALIDATION.md). No whole-library or
processor-specific speedup has been measured.

### Incoming-home storage after `033565e`

Twenty-eight procedures move small scalars or records into incoming home space.
Eighteen frames shrink by 16 bytes and ten by 32 bytes. Nine other scalar-only
candidates retain their original locals because alignment would absorb the
proposed saving. Large records remain in ordinary local storage.

| Procedure | Previous stack bytes | Current stack bytes |
| --- | ---: | ---: |
| `f_validate` | 72 | 56 |
| `f_read_record` | 72 | 40 |
| `f_write_bytes` | 104 | 72 |
| `f_create` | 728 | 696 |
| `fat_check_ownership` | 136 | 104 |
| `f_order_run` | 152 | 136 |
| `fat_order_read_range` | 120 | 88 |

These figures include register pushes and fixed allocation, excluding the
return address and incoming area already provided by the caller. Summed across
all procedures, declared footprints fall from **21,624 to 21,016 bytes**. The
**608-byte** difference is static accounting, not a measured call-stack peak.
Actual savings accumulate along the procedures active on a given call path.

The tradeoff is **88 additional code bytes**, from **26,748 to 26,836**. Four
procedures grow because of longer address and resulting branch encodings:
`f_resize` by six bytes, `fat_check_ownership` by 49, `f_order_run` by 18, and
`fat_stream_read_range` by 15. The other 24 relocated procedures retain their
code sizes. Instruction count remains **7,474**, with identical instruction
sequences after mapping each named stack field, frame allocation, and branch
destination. Saves/restores, loads/stores, and call counts remain unchanged.
The runtime rationale is lower stack cache pressure despite unchanged memory
instruction counts. Cache-miss and timing differences have not been measured.

All 149 procedures retain valid unwind, call alignment/home space, relocation
targets and COMDAT associations. All 96 execution suites and 312 linker checks
pass. The ABI probe now guards the caller's boundary and callbacks overwrite
their incoming homes; packed DWORDs must also survive a nested overwrite while
the real fifth argument remains intact. Metadata checks verify DWORD types and
positions, and three negative assembly probes reject invalid record layouts.

## 8. Review procedure

1. **Freeze the comparison.** Record the base revision and proposed source diff.
   Keep separate objects for each version; an existing object may predate edits.
   Record assembler and macro versions with private evidence.
2. **Trace meaning first.** Track live registers, flags, memory changes, value
   widths, and observation time through success, failure, and callbacks. Expand
   `fastcall` before relying on argument or target registers.
3. **Inspect the object.** Compare complete procedure sizes, instructions, branch
   destinations, relocations, saves, stack allocation, and unwind records.
   Separate code bytes from total object bytes and linked feature size.
   For each affected path, identify changes to instruction processing, memory
   operations, dependencies, fusion eligibility, and added branches or spills.
   Distinguish static counts from work performed on an invocation.
4. **Run the relevant behavior.** Exercise boundary and failure paths, volatile
   callback clobbers, nonvolatile preservation, and call alignment. Use existing
   regressions where they already expose the changed contract.
5. **Broaden when the boundary changes.** Procedure/COMDAT changes need link and
   replacement matrices. Calling-convention changes need ABI consumers. A speed
   claim needs measurements on the stated processors and workload.
6. **Explain the accepted result.** State which processor resources the change
   uses more efficiently, what work and bytes disappeared, why the semantics
   remain valid, and which checks support that conclusion. Give processor and
   workload scope for measured timing; do not turn a byte count into a speedup.

From the repository root, with tools configured as in [BUILD.md](BUILD.md):

```powershell
.\tests\win32\build.cmd test
llvm-objdump -dr --x86-asm-syntax=intel fat32.obj
llvm-readobj --sections --relocations --unwind fat32.obj
llvm-nm --undefined-only fat32.obj
```

Use isolated source trees for historical comparisons, with the same toolchain
and the appropriate historical include paths. Keep generated objects, traces,
and machine-specific transcripts in ignored build storage. Publish the source
reasoning, reproducible scope, and measured result.

[amd-sogs]: https://github.com/bitRAKE/amd_zen_sogs
[zen4-sog]: https://github.com/bitRAKE/amd_zen_sogs/blob/main/57647/57647_1.01.pdf
