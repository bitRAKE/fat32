# Force-exclude FAT32 writes with link-time stubs

Link the object built from [readonly.asm](readonly.asm) before the normal
library to make a consumer read-only even when it accidentally references
writing APIs:

```text
link ... consumer.obj example\readonly\readonly.obj fat32.lib
lld-link ... consumer.obj example\readonly\readonly.obj fat32.lib
```

The replacement object exports the same `COMDAT ANY` public symbols as the
defaults. The selected stubs return `F_READONLY`; the default mutation bodies
and their private helpers are discarded under `/OPT:REF`. The normal library
remains unchanged, and consumers that omit this object pay no code or state
cost. See [LINKING.md](../../docs/LINKING.md) for selection and archive rules.

This demonstrates force exclusion: calling a disabled operation cannot pull
its implementation back into the image. Link order is part of the build's
policy. Keep the replacement object before a directly linked `fat32.obj` too.

## What is disabled

The 20 replacements cover both format executors, both advanced FAT update hooks, all seven shared-handle mutation APIs, all six
snapshot mutation APIs, both physical commit APIs, and `fat_order_init`.
Blocking that initializer also excludes its writable staging callbacks.
The complete symbol list is at the top of [readonly.asm](readonly.asm).

- Writes return `F_READONLY` with `done=0`; handle positions remain unchanged.
- Create, resize, rename, delete and metadata changes return `F_READONLY`
  without changing their objects or output entries/handles.
- Commit reports return `F_READONLY`, `FE_NONE`, and zero attempted writes
  and flushes. Existing order state is untouched.
- Ordered-provider construction returns `F_READONLY` without changing its
  output. Use an ordinary sector provider for reading.

Required output pointers remain valid according to the normal API contract.
Stubs never report a successful operation that they did not perform. Mount,
lookup, reads, diagnostics, streams and ordinary handle lifetime operations
remain available. Writable handles may still be opened; their mutations fail.

The guarantee covers media mutation through this selected FAT32 implementation.
Normal reads still update in-memory caches and handles. Application code and
provider callbacks retain whatever direct device access the consumer gives
them; these stubs do not intercept that separate access.

## Build and test

From the library root in the configured x64 developer environment:

```text
tests\win32\build.cmd examples
tests\win32\build.cmd readonly-test
```

`examples-test` also includes this test. Override `FASM2_ROOT`, `WIN32JSON_ROOT`
and `LLVM_BIN` as described in the root build guide when tools are elsewhere.

[test.c](test.c) mounts a deterministic memory-backed FAT32 volume, opens a
writable handle, and reads a known file before and after attempting every
blocked operation. It checks output preservation, zero completed bytes,
guarded commit reports and a call through `fat_call_locked`. The provider
deliberately accepts writes: controls without the stubs modify the file.
With the stubs there are zero write, transaction-begin/end or flush callbacks.
No disk device is opened.

[tests/readonly.py](../../tests/win32/readonly.py) runs 16 executable links across
MSVC LINK and LLD, direct/default-archive inputs and debug/release, with and
without the replacement object. Every public library symbol is rooted,
including mutation symbols. Map checks verify all replacements win and the
private mutation/physical-commit helpers are absent; PE checks verify no OS/CRT
imports and valid unwind ranges. The stub object also has associated unwind
and CodeView metadata for each function.

The test explicitly classifies all 62 current public entries. Adding an API
requires reviewing that classification, so a future recovery
writer cannot silently become an unexamined exception to this profile.
