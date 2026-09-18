# Windows test harness. Run from the repository root, normally via build.cmd.
# The root makefile owns only the filesystem library and its object policy.
!INCLUDE makefile

!IFNDEF WIN32JSON_ROOT
WIN32JSON_ROOT = ..\win32json
!ENDIF
# This root is independent of FASM2_ROOT. Only adapter/demo recipes use it.
# CALM call/type prerequisites and include order: docs/BUILD.md.
hostinc = $(WIN32JSON_ROOT)\generated\fasm2_calm\x64;$(WIN32JSON_ROOT);$(FASM2_ROOT)\include
HOST = build\win32
TEST_HEADERS = tests\win32\api.h tests\win32\*-api.h fat32.h
TEST_CASES = tests\win32\*.c

all: $(HOST)\tests.exe $(HOST)\imagecheck.exe $(HOST)\usbcheck.exe $(HOST)\repocheck.exe examples

$(HOST)\buffer.obj: tests\win32\buffer.asm tests\win32\buffer.inc $(FAT_HEADERS) common\policy.g
	if not exist $(HOST) mkdir $(HOST)
	set INCLUDE=$(hostinc);$(INCLUDE)
	"$(fasm2)" -e 5 tests\win32\buffer.asm $@
$(HOST)\win32.obj: tests\win32\win32.asm tests\win32\buffer.inc $(FAT_HEADERS) common\policy.g
	if not exist $(HOST) mkdir $(HOST)
	set INCLUDE=$(hostinc);$(INCLUDE)
	"$(fasm2)" -e 5 tests\win32\win32.asm $@
$(HOST)\abi.obj: tests\win32\abi.asm $(FAT_HEADERS) common\policy.g
	if not exist $(HOST) mkdir $(HOST)
	set INCLUDE=$(FASM2_ROOT)\include;$(INCLUDE)
	"$(fasm2)" -e 5 tests\win32\abi.asm $@
$(HOST)\sector.lib: $(HOST)\buffer.obj $(HOST)\win32.obj
	lib /nologo /out:$@ $**

$(HOST)\tests.exe: tests\win32\test.c $(TEST_CASES) $(TEST_HEADERS) $(HOST)\abi.obj fat32.lib $(HOST)\sector.lib
	cl /nologo /std:c17 /utf-8 /W4 /WX /Zi /Od /Fo:$(HOST)\test.obj /Fd:$(HOST)\tests.pdb /Fe:$@ tests\win32\test.c $(HOST)\abi.obj fat32.lib $(HOST)\sector.lib kernel32.lib /link /incremental:no
$(HOST)\imagecheck.exe: tests\win32\images.c $(TEST_HEADERS) fat32.lib
	if not exist $(HOST) mkdir $(HOST)
	cl /nologo /std:c17 /utf-8 /W4 /WX /O2 /Fo:$(HOST)\images.obj /Fe:$@ tests\win32\images.c fat32.lib /link /incremental:no
$(HOST)\usbcheck.exe: tests\win32\usb.c $(TEST_HEADERS) fat32.lib $(HOST)\sector.lib
	cl /nologo /std:c17 /utf-8 /W4 /WX /Zi /Od /Fo:$(HOST)\usb.obj /Fd:$(HOST)\usbcheck.pdb /Fe:$@ tests\win32\usb.c fat32.lib $(HOST)\sector.lib kernel32.lib /link /incremental:no
$(HOST)\repocheck.exe: tests\win32\repo.c $(TEST_HEADERS) fat32.lib $(HOST)\sector.lib
	cl /nologo /std:c17 /utf-8 /W4 /WX /Zi /Od /Fo:$(HOST)\repo.obj /Fd:$(HOST)\repocheck.pdb /Fe:$@ tests\win32\repo.c fat32.lib $(HOST)\sector.lib kernel32.lib /link /incremental:no

example\win32\demo.obj: example\win32\demo.asm tests\win32\buffer.inc $(FAT_HEADERS) common\policy.g
	set INCLUDE=$(hostinc);$(INCLUDE)
	"$(fasm2)" -e 5 example\win32\demo.asm $@
$(HOST)\fatdemo.exe: example\win32\demo.obj fat32.lib $(HOST)\sector.lib
	link @example\win32\demo.response /out:$@ $**
example\uefi\reader.obj: example\uefi\reader.asm example\uefi\reader.inc $(FAT_HEADERS) common\policy.g
example\readonly\readonly.obj: example\readonly\readonly.asm $(FAT_HEADERS) common\policy.g
$(HOST)\ueficheck.exe: example\uefi\test.c example\uefi\reader.obj fat32.h fat32.lib
	if not exist $(HOST) mkdir $(HOST)
	cl /nologo /std:c17 /utf-8 /W4 /WX /Zi /Od /Fo:$(HOST)\uefi-test.obj /Fd:$(HOST)\ueficheck.pdb /Fe:$@ example\uefi\test.c example\uefi\reader.obj fat32.lib /link /incremental:no

test: $(HOST)\tests.exe
	.\$(HOST)\tests.exe
examples: $(HOST)\fatdemo.exe example\uefi\reader.obj example\readonly\readonly.obj
examples-test: $(HOST)\ueficheck.exe readonly-test
	.\$(HOST)\ueficheck.exe
feature-test: fat32.lib shadow-test home-test
	py tests\win32\features.py --fasm-root "$(FASM2_ROOT)" $(feature_llvm) $(FEATURE_ARGS)
home-test: fat32.obj $(HOST)\abi.obj
	py tests\win32\home.py --fasm-root "$(FASM2_ROOT)" $(feature_llvm)
shadow-test: fat32.lib
	py tests\win32\shadow.py --fasm-root "$(FASM2_ROOT)" $(feature_llvm)
readonly-test: fat32.lib example\readonly\readonly.obj
	py tests\win32\readonly.py --fasm-root "$(FASM2_ROOT)" $(feature_llvm)

verify: host-verify
host-verify: $(HOST)\fatdemo.exe
	$(llvm_readobj) --unwind $(HOST)\buffer.obj $(HOST)\win32.obj
	$(llvm_readobj) --coff-imports $(HOST)\fatdemo.exe

# Short target names; all host products stay under ignored build/win32/.
sector.lib: $(HOST)\sector.lib
tests.exe: $(HOST)\tests.exe
imagecheck.exe: $(HOST)\imagecheck.exe
usbcheck.exe: $(HOST)\usbcheck.exe
repocheck.exe: $(HOST)\repocheck.exe
fatdemo.exe: $(HOST)\fatdemo.exe
ueficheck.exe: $(HOST)\ueficheck.exe
