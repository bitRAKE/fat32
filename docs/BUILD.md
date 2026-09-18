# Building the library

`fat32.asm` assembles the complete library into one AMD64 COFF object. Individual
functions and their metadata are independently selectable at link time. Packaging
that object as `fat32.lib` adds no runtime dependency.

## Include dependencies

There are two separate assembly dependencies:

| Source / target | External includes |
| --- | --- |
| `fat32.asm` / `fat32.obj`, `fat32.lib` | [bitRAKE/fasm2](https://github.com/bitRAKE/fasm2), selected by `FASM2_ROOT` |
| UEFI and read-only example objects; `tests/win32/abi.asm` | The same fasm2 includes |
| `tests/win32/buffer.asm`, `tests/win32/win32.asm`, `example/win32/demo.asm` | fasm2 plus the generated CALM projection from [bitRAKE/win32json](https://github.com/bitRAKE/win32json), selected independently by `WIN32JSON_ROOT` |

`FASM2_ROOT` and `WIN32JSON_ROOT` are build settings, not assembler directives.
The makefiles translate them into the `INCLUDE` search path for each object.
Neither root implies or discovers the other. Relative roots are resolved from
the FAT32 repository root; the defaults assume sibling checkouts.

The source-level switch is in [common/policy.g](../common/policy.g):

```text
common/policy.g
  always: FASM2_ROOT/include/
    dd.inc, align.inc, format.inc, x86-2.inc
    format/newcoffms.inc -> newcoffcv.inc -> newcoffproc64.inc
    macro/proc64.inc, macro/struct.inc
  when FAT32_WIN32 is defined:
    WIN32JSON_ROOT/generated/fasm2_calm/x64/windows.inc
      runtime/static_call64.g, windows.g, selected types and API contracts
```

Only the three Win32 sources listed above define `FAT32_WIN32` before including
the policy. They also select the Win32 types and `kernel32` contracts they need.
The projection supplies Win32 declarations and call lowering, including its
`fastcall` implementation. The core uses fasm2's procedure/call macros directly.
The public FAT32 `.inc` declarations and implementations under `fat/` are local
to this repository.

For a core object the build prepends `FASM2_ROOT/include` to `INCLUDE`. For a
Win32 adapter or demo it prepends, in order:

1. `WIN32JSON_ROOT/generated/fasm2_calm/x64`
2. `WIN32JSON_ROOT`
3. `FASM2_ROOT/include`

The existing `INCLUDE` follows those paths. C harness sources use the Visual
Studio / Windows SDK headers; win32json is an assembly dependency. Invoking the
Windows build wrapper does not itself add a Win32 dependency to the core:
`tests\win32\build.cmd fat32.lib` still builds from the fasm2 includes alone.

## Required assembler capabilities

Use fasmg with the bitRAKE/fasm2 includes supporting `MS64 NEWCOFF`, COMDAT ANY and
NODUPLICATES sections, associative unwind/CodeView contributions, and the
`newcoff_debug_procs` / static-RSP procedure macros. Typed PROC names must
reserve their declared byte width (`:4` advances four bytes), permitting packed
incoming-home storage. `common/policy.g` selects this policy and supplies bounded
`home_struct` records. The fasm2 CodeView wrapper must infer scalar DWORD debug
types from four-byte labels. An older include set without these capabilities is
not a compatible toolchain; the linker feature tests check the emitted object
contract.

The validated fasm2 include revision is
[`659ab68`](https://github.com/bitRAKE/fasm2/commit/659ab689ce492c487b6c825c4c11383c6759684c),
which includes the DWORD CodeView correction. FAT32 uses it directly without a
local debug-type override.
The fasmg engine version alone does not identify these macro capabilities.

## Library build

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

Win32 adapter builds additionally require the consolidated CALM projection at
`WIN32JSON_ROOT/generated/fasm2_calm/x64/`, containing `windows.inc`,
`runtime/static_call64.g`, and the selected modules under `types/`. The separate
`generated/fasm2/` projection is not the include surface used here. See the
[harness prerequisite](../tests/win32/README.md#projection-prerequisite) before
using a fresh win32json checkout.

For explicit roots, with both dependencies beside this repository:

```cmd
tests\win32\build.cmd all test FASM2_ROOT=..\fasm2 WIN32JSON_ROOT=..\win32json
```

Host programs and the test-only `sector.lib` go under ignored `build/win32/`.
The root `all` target remains library-only. Example assembly objects remain
beside their source files; generated captures and reports stay under `build/`.
