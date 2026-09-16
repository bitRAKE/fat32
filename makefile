# Library-only NMAKE build, run from the repository root.
# Override tool locations in the environment or as NMAKE arguments.
!IFNDEF FASM2_ROOT
FASM2_ROOT = ..\fasm2
!ENDIF
fasm2 = $(FASM2_ROOT)\fasmg.exe

!IFDEF LLVM_BIN
llvm_readobj = "$(LLVM_BIN)\llvm-readobj.exe"
llvm_nm = "$(LLVM_BIN)\llvm-nm.exe"
llvm_objdump = "$(LLVM_BIN)\llvm-objdump.exe"
feature_llvm = --llvm-bin "$(LLVM_BIN)"
!ELSE
llvm_readobj = llvm-readobj.exe
llvm_nm = llvm-nm.exe
llvm_objdump = llvm-objdump.exe
!ENDIF

FAT_HEADERS = *.inc
FAT_SOURCES = fat\*.inc

.SUFFIXES:
.SUFFIXES: .asm .obj
.asm.obj:
	set INCLUDE=$(FASM2_ROOT)\include;$(INCLUDE)
	"$(fasm2)" -e 5 $< $@

all: fat32.lib
fat32.lib: fat32.obj
	lib /nologo /out:$@ $**
fat32.obj: fat32.asm $(FAT_HEADERS) $(FAT_SOURCES) common\policy.g

verify: fat32.obj
	$(llvm_readobj) --unwind fat32.obj
	$(llvm_nm) --undefined-only fat32.obj
	$(llvm_objdump) -d fat32.obj > fat32.objdump.txt

.SILENT:
