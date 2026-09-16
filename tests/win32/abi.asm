; ABI regression support; linked only into tests.exe, never production libs.
include '../../common/policy.g'
include '../../fat32.inc'

; RCX=SectorOps* forwarding target; remaining arguments already in RDX/R8.
; Check entry stack alignment, call the real callback, preserve only EAX status,
; then overwrite every other volatile integer register. This prevents a lucky C
; callback register allocation from hiding a caller's ABI violation.
macro poison_callback name*,slot*
	public name
	proc name
		lea r10, [rsp+framebytes@proc]
		and r10d, 15
		mov eax, F_ARGUMENT
		cmp r10d, 8
		jne .poison
		mov r11, qword [rcx+slot]
		mov rcx, qword [rcx+SectorOps.context]
		fastcall r11, rcx, rdx, r8
	.poison:
		mov ecx, 0C1C1C1C1h
		mov edx, 0D2D2D2D2h
		mov r8d, 088888888h
		mov r9d, 099999999h
		mov r10d, 0AAAAAAAAh
		mov r11d, 0BBBBBBBBh
		ret
	endp
end macro

poison_callback abi_flush, SectorOps.flush
poison_callback abi_read, SectorOps.read
poison_callback abi_write, SectorOps.write
poison_callback abi_begin, SectorOps.begin
poison_callback abi_end, SectorOps.end
poison_callback abi_range, FatRangeOps.read

public abi_probe
; RCX={uint64 target,args[4]; uint32 result;}* in-out.
; Return EAX=nonvolatile GPR corruption mask, and save target's EAX to result.
; Uses only RSP-relative locals after the target returns; even corrupt nonvolatile
; registers cannot compromise the probe or its caller's saved register values.
proc abi_probe uses rbx rbp rsi rdi r12 r13 r14 r15
	locals
		request dq ?
		status dd ?
	endl
body:
	mov qword [request], rcx
	iterate reg, rbx,rbp,rsi,rdi,r12,r13,r14,r15
		mov reg, 1020304050607080h + %
	end iterate
	mov r10, rcx
	mov r11, qword [r10]
	fastcall r11, qword [r10+8], qword [r10+16], qword [r10+24], qword [r10+32]
	mov dword [status], eax
	xor eax, eax
	iterate reg, rbx,rbp,rsi,rdi,r12,r13,r14,r15
		mov r10, 1020304050607080h + %
		cmp reg, r10
		setne r11b
		movzx r11d, r11b
		shl r11d, %-1
		or eax, r11d
	end iterate
	mov r10, qword [request]
	mov edx, dword [status]
	mov dword [r10+40], edx
	ret
endp
