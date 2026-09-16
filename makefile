# One object-first trajectory, matching hexed.
fasm2root = ..\fasm2
win32json = ..\win32json
fasm2 = $(fasm2root)\fasmg.exe
calminc = $(win32json)\generated\fasm2_calm\x64;$(win32json);$(fasm2root)\include
llvmbin = ..\llvm\bin

.SUFFIXES:
.SUFFIXES: .asm .obj
.asm.obj:
	set INCLUDE=$(calminc);$(INCLUDE)
	$(fasm2) -e 5 $< $@

all: fat32.lib sector.lib fatdemo.exe
fat32.lib: fat32.obj
	lib /nologo /out:$@ $**
sector.lib: buffer.obj win32.obj
	lib /nologo /out:$@ $**
fat32.obj: fat32.h common\policy.g fat\volume.inc fat\directory.inc fat\file.inc fat\create.inc
buffer.obj win32.obj: fat32.h buffer.h common\policy.g
example.obj: fat32.h buffer.h common\policy.g
fatdemo.exe: example.obj fat32.lib sector.lib
	link @example.response $**

test: tests.exe
	.\tests.exe
tests.exe: tests\test.c tests\api.h fat32.lib sector.lib
	cl /nologo /std:c17 /utf-8 /W4 /WX /Zi /Od /Fe:$@ tests\test.c fat32.lib sector.lib kernel32.lib /link /incremental:no

usbcheck.exe: tests\usb.c tests\api.h fat32.lib sector.lib
	cl /nologo /std:c17 /utf-8 /W4 /WX /Zi /Od /Fe:$@ tests\usb.c fat32.lib sector.lib kernel32.lib /link /incremental:no

repocheck.exe: tests\repo.c tests\api.h fat32.lib sector.lib
	cl /nologo /std:c17 /utf-8 /W4 /WX /Zi /Od /Fe:$@ tests\repo.c fat32.lib sector.lib kernel32.lib /link /incremental:no

verify: all
	"$(llvmbin)\llvm-readobj.exe" --unwind fat32.obj buffer.obj win32.obj
	"$(llvmbin)\llvm-nm.exe" --undefined-only fat32.obj
	"$(llvmbin)\llvm-objdump.exe" -d fat32.obj > fat32.objdump.txt
	"$(llvmbin)\llvm-readobj.exe" --coff-imports fatdemo.exe

.SILENT:
