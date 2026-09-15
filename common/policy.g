; Object policy adapted from common\policy.g.
; Core objects use struct/proc only. Win32 adapters opt into the projection.
if ~ definite FAT32_POLICY_INCLUDED
FAT32_POLICY_INCLUDED := 1
include 'dd.inc'
include 'align.inc'
include 'format.inc'
include 'x86-2.inc'
use AMD64

; Match format.inc's deferred extension protocol for the NEWCOFF variant.
macro format?.MS64? variant
	match =NEWCOFF?, variant
		format?.default_extension = 'obj'
		__include 'format/newcoffms.inc'
		use64
	else
		format.MS64 variant
	end match
end macro
NEWCOFF.DEBUG := 6
format MS64 NEWCOFF
include 'macro/proc64.inc'
if definite FAT32_WIN32
	include 'generated/fasm2_calm/x64/windows.inc'
else
	include 'macro/struct.inc'
end if

; Preserve the grouped USES list through the debug wrapper, as in hexed.
; Each public routine saves its nonvolatile registers: no ambient RBX.
macro fat32_debug_prologue procname,flag,parmbytes,localbytes,reglist
	cvproc procname
	static_rsp_prologue procname,flag,parmbytes,localbytes,<reglist>
	match any, reglist
		cvframe framebytes@proc, reglist
	else
		cvframe framebytes@proc
	end match
end macro
macro fat32_debug_close procname,flag,parmbytes,localbytes,reglist
	cvendp
	static_rsp_close procname,flag,parmbytes,localbytes,<reglist>
end macro
prologue@proc equ fat32_debug_prologue
epilogue@proc equ static_rsp_epilogue
close@proc equ fat32_debug_close
newcoff_debug_procs
section '.text' code readable executable align 16
end if
