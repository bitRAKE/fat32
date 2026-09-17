# Building the library

`fat32.asm` assembles the complete library into one AMD64 COFF object. Individual
functions and their metadata are independently selectable at link time. Packaging
that object as `fat32.lib` adds no runtime dependency.

## Required assembler capabilities

Use fasmg with the fasm2 includes supporting `MS64 NEWCOFF`, COMDAT ANY and
NODUPLICATES sections, associative unwind/CodeView contributions, and the
`newcoff_debug_procs` / static-RSP procedure macros. Typed PROC names must
reserve their declared byte width (`:4` advances four bytes), permitting packed
incoming-home storage. `common/policy.g` selects this policy and supplies bounded
`home_struct` records and scalar DWORD debug types. An older include set without
these capabilities is not a compatible toolchain; the linker feature tests check
the emitted object contract.

The root makefile uses NMAKE and the Microsoft COFF librarian. From the repository
root in an x64 developer prompt:

```cmd
nmake /nologo
nmake /nologo verify
```

| Setting | Meaning |
| --- | --- |
| `FASM2_ROOT` | fasm2 directory containing `fasmg.exe` and `include/`; default `../fasm2` |
| `LLVM_BIN` | Optional LLVM executable directory for `verify`; otherwise use PATH |

Set these in the environment or as NMAKE arguments. `verify` reports unwind
records and undefined core symbols and writes the disassembly. The library
object must have no undefined external symbols.

To assemble an object directly with an explicitly configured tool directory:

```cmd
set INCLUDE=%FASM2_ROOT%\include
"%FASM2_ROOT%\fasmg.exe" -e 5 fat32.asm fat32.obj
```

Link the object or archive into a consumer using the documented Win64 ABI.
See [LINKING.md](LINKING.md) before supplying replacements or changing linkers.
There are no Win32 bindings, CRT, or operating-system imports in the core.

## Windows development harness

[tests/win32](../tests/win32/README.md) owns Visual Studio discovery, the generated
Win32 bindings, test adapters, and executable build rules. Its bootstrap can also
build just the library when a developer prompt is unavailable:

```cmd
tests\win32\build.cmd fat32.lib
tests\win32\build.cmd all test examples-test feature-test verify
```

Host programs and the test-only `sector.lib` go under ignored `build/win32/`.
The root `all` target remains library-only. Example assembly objects remain
beside their source files; generated captures and reports stay under `build/`.
