# Object-first build. Environment or NMAKE arguments may override tool roots.
!IFNDEF FASM2_ROOT
FASM2_ROOT = ..\fasm2
!ENDIF
!IFNDEF WIN32JSON_ROOT
WIN32JSON_ROOT = ..\win32json
!ENDIF
fasm2root = $(FASM2_ROOT)
win32json = $(WIN32JSON_ROOT)
fasm2 = $(fasm2root)\fasmg.exe
calminc = $(win32json)\generated\fasm2_calm\x64;$(win32json);$(fasm2root)\include
!IFDEF LLVM_BIN
llvm_readobj = "$(LLVM_BIN)\llvm-readobj.exe"
llvm_nm = "$(LLVM_BIN)\llvm-nm.exe"
llvm_objdump = "$(LLVM_BIN)\llvm-objdump.exe"
!ELSE
llvm_readobj = llvm-readobj.exe
llvm_nm = llvm-nm.exe
llvm_objdump = llvm-objdump.exe
!ENDIF

.SUFFIXES:
.SUFFIXES: .asm .obj
.asm.obj:
	set INCLUDE=$(calminc);$(INCLUDE)
	"$(fasm2)" -e 5 $< $@

all: fat32.lib sector.lib fatdemo.exe
fat32.lib: fat32.obj
	lib /nologo /out:$@ $**
sector.lib: buffer.obj win32.obj
	lib /nologo /out:$@ $**
fat32.obj: fat32.inc common\policy.g fat\volume.inc fat\directory.inc fat\file.inc fat\create.inc
buffer.obj win32.obj: fat32.inc buffer.inc common\policy.g
example.obj: fat32.inc buffer.inc common\policy.g
fatdemo.exe: example.obj fat32.lib sector.lib
	link @example.response $**

test: tests.exe
	.\tests.exe
tests\abi.obj: tests\abi.asm fat32.inc common\policy.g
tests.exe: tests\test.c tests\api.h tests\abi.obj fat32.lib sector.lib
	cl /nologo /std:c17 /utf-8 /W4 /WX /Zi /Od /Fe:$@ tests\test.c tests\abi.obj fat32.lib sector.lib kernel32.lib /link /incremental:no

usbcheck.exe: tests\usb.c tests\api.h fat32.lib sector.lib
	cl /nologo /std:c17 /utf-8 /W4 /WX /Zi /Od /Fe:$@ tests\usb.c fat32.lib sector.lib kernel32.lib /link /incremental:no

repocheck.exe: tests\repo.c tests\api.h fat32.lib sector.lib
	cl /nologo /std:c17 /utf-8 /W4 /WX /Zi /Od /Fe:$@ tests\repo.c fat32.lib sector.lib kernel32.lib /link /incremental:no

examples: fat32.lib examples\uefi\reader.obj
examples\uefi\reader.obj: examples\uefi\reader.asm examples\uefi\reader.inc fat32.inc common\policy.g
examples-test: ueficheck.exe
	.\ueficheck.exe
ueficheck.exe: examples\uefi\test.c tests\api.h examples\uefi\reader.obj fat32.lib
	cl /nologo /std:c17 /utf-8 /W4 /WX /Zi /Od /Fo:examples\uefi\test.obj /Fe:$@ examples\uefi\test.c examples\uefi\reader.obj fat32.lib /link /incremental:no

verify: all
	$(llvm_readobj) --unwind fat32.obj buffer.obj win32.obj
	$(llvm_nm) --undefined-only fat32.obj
	$(llvm_objdump) -d fat32.obj > fat32.objdump.txt
	$(llvm_readobj) --coff-imports fatdemo.exe

.SILENT:
