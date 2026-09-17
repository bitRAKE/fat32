; ABI regression support; linked only into tests.exe, never production libs.
include '../../common/policy.g'
include '../../fat32.inc'

; RCX=SectorOps* forwarding target; remaining arguments already in RDX/R8.
; Check entry stack alignment, call the real callback, preserve only EAX status,
; then overwrite incoming home space and every other volatile integer register.
; This prevents a lucky C
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
		iterate offset, 0,8,16,24
			mov qword [parmbase@proc+offset], 055555555h
		end iterate
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
; Return EAX=corruption mask (bits 0..7: nonvolatile GPRs; bit 8: home overflow),
; and save target's EAX to result.
; Uses only RSP-relative locals after the target returns; even corrupt nonvolatile
; registers cannot compromise the probe or its caller's saved register values.
proc abi_probe uses rbx rbp rsi rdi r12 r13 r14 r15
	locals
		home_end dq 4 dup ?
		request dq ?
		status dd ?
	endl
body:
	iterate offset, 0,8,16,24
		mov qword [home_end+offset], 012345678h
	end iterate
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
	iterate offset, 0,8,16,24
		cmp qword [home_end+offset], 012345678h
		setne r11b
		movzx r11d, r11b
		shl r11d, 8
		or eax, r11d
	end iterate
	mov r10, qword [request]
	mov edx, dword [status]
	mov dword [r10+40], edx
	ret
endp

; Four physical argument homes provide eight DWORD scratch fields. The real
; fifth argument stays at entry RSP+40; nested callees may overwrite their homes.
proc abi_home_fields uses rbx rsi, a:4, b:4, c:4, d:4, e:4, f:4, g:4, h:4, fifth
    assert h+4-(parmbase@proc) = 32
    assert fifth-(parmbase@proc) = 32
    iterate field, a,b,c,d,e,f,g,h
        assert field-a = (%-1)*4
        mov [field], 10203040h+%
    end iterate
    fastcall abi_home_poison
    mov eax, F_ARGUMENT
    cmp qword [fifth], 076543210h
    jne .done
    iterate field, a,b,c,d,e,f,g,h
        cmp [field], 10203040h+%
        jne .done
    end iterate
    xor eax, eax
.done:
    ret
endp

proc abi_home_poison
    iterate offset, 0,8,16,24
        mov qword [parmbase@proc+offset], 055555555h
    end iterate
    ret
endp

public abi_home_probe
proc abi_home_probe
    locals
        guard dq ?
    endl
body:
    mov qword [guard], 012345678h
    fastcall abi_home_fields, 0, 0, 0, 0, 076543210h
    cmp qword [guard], 012345678h
    je .done
    mov eax, F_ARGUMENT
.done:
    ret
endp
