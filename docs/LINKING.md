# Linking and consumer replacements

Each public function in `fat32.asm` has its own `COMDAT ANY` code section.
Private helpers and tables retain `NODUPLICATES`; their names are not an
extension ABI. Unwind and CodeView contributions are associated with their
owning code. Unreferenced features and discarded defaults leave no live
per-function metadata or compulsory workspace.

A consumer can implement a replacement under the same public symbol, using
the same Win64 calling convention, argument/result types, state layouts,
ownership and error contracts. Validate a replacement against the operations
and providers that will call it, including transitive library calls.

For an assembly replacement, use the ordinary procedure/unwind policy and:

```asm
public fat_get
section '.text$fat_get' code readable executable comdat any align 16
; proc fat_get ... with the documented ABI and complete replacement behavior
```

Place replacement objects before default objects, or replacement archives
before `fat32.lib`. Both alternative definitions must use compatible ANY
selection. The tested MSVC LINK and LLD combinations retain the first selected
definition. Avoid treating a mixed object/archive list as a simple textual
sequence: explicit objects are processed before lazy archive extraction.
Supply a replacement as an explicit object ahead of the default library when
its extraction cannot otherwise be established.

The [PE/COFF specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#comdat-sections-object-only)
permits any definition for ANY; first-selection behavior is a tested toolchain
contract, not a promise for every COFF linker. Keep link inputs ordered and
rerun `tests\win32\build.cmd shadow-test` when changing toolchains or packaging. Do not use
`/FORCE:MULTIPLE`: unexpected private duplicates must still fail.

All references, including calls made by other library functions, resolve to
the selected public definition. Selection applies to the whole linked program,
including transitive callers; it is not restricted to calls originating in the
consumer. Replacement authors own the effects on shared caches, versions,
partial results and error behavior. The library adds no runtime gate or policy
check to restrict that choice.

A replacement cannot call its own public name
to reach the discarded default. Compose a wrapper from separately named basic
operations, or introduce a reusable basic entry if needed. No runtime dispatch
table or platform-dependent library compilation is required.

`tests/win32/shadow.py` exercises both input orders with MSVC LINK and LLD, explicit
objects, archives and mixed inputs, debug and release links: 32 links. It checks
direct and internal calls, the map's winning definition, and retained unwind
records. Ordinary feature tests separately enforce code-size and dependency limits. Production
consumers must additionally verify their replacement's behavior and ABI;
successful selection alone does not validate a new filesystem policy.

## Force exclusion

A replacement can deliberately deny a feature. The
[read-only example](../example/readonly/README.md) supplies 20 stubs returning
`F_READONLY`, covering volume formatting, FAT update hooks, mutations, physical commits and writable staging-provider
construction. Accidental references then retain the rejecting stub while the
default implementation and its dependencies disappear. This is stronger than
merely hoping no current caller references a writer.

Its test roots every public API, proves the private mutation helpers are absent,
checks that attempted writes make no provider calls, and runs writable controls
without the stubs. Transfer/report outputs remain honest about zero completed
work. This policy has no cost for ordinary library consumers.

For mutation policy, an explicit consumer `fat_put` replacement may call
`fat_put_checked`. The latter implements the complete checked update and never
calls `fat_put`; both obey the outer transaction/publication contract in
[API.md](API.md). This selects the policy for allocation and freeing calls made
inside the library as well as direct advanced calls.
