; Shared MS64 NEWCOFF object, static-RSP, unwind, and CodeView policy.
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

; Preserve the grouped USES list through the debug wrapper.
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
; Static RSP addressing leaves RBP available as a general nonvolatile register.
; Prefer pointer/32-bit state there when the full routine encodes smaller.
; Declare RBP in USES; its save/restore and unwind entry remain mandatory.
newcoff_debug_procs
section '.text' code readable executable align 16

; Source call-site policy (the installed fastcall macro is not a parallel move):
; Arguments are assigned LEFT TO RIGHT: RCX, RDX, R8, R9, then stack arguments.
; Identical source/destination registers emit no MOV. Save a live input for later
; calls when needed, but use its incoming register at the first call while valid.
; Never refer to an old argument register after an earlier argument overwrites
; it; use the saved copy in that case. Stack arguments can also use RAX scratch.
; Treat volatile registers as dead across calls, except an explicitly documented
; private helper guarantee (f_cluster_sector preserves RCX).
;
; RET expands to the full static-RSP restore sequence. Framed routines share one
; exit; status-only guards load EAX before adjacent CMP/TEST + conditional jump.
; Leaves with a bare one-byte RET may retain separate value-return paths.
end if
