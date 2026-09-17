; Caller-funded hash-indexed phase versions. This module contains no allocator,
; OS imports or global state. The ordinary filesystem never references it.

; RCX=order*. Reset arena/index only; preserve poison and first failure evidence.
section '.text$f_order_reset' code readable executable comdat align 16
proc f_order_reset uses rdi
	mov r10, rcx
	mov rdi, [r10 + FatOrder.index]
	mov ecx, [r10 + FatOrder.slots]
	xor eax, eax
	rep stosd
	mov dword [r10 + FatOrder.used], 0
	mov dword [r10 + FatOrder.phase], 0
	mov dword [r10 + FatOrder.unique], 0
	mov dword [r10 + FatOrder.active], 0
	mov dword [r10 + FatOrder.accepted], 0
	ret
endp

; RCX=fresh order*, RDX=borrowed backend*, R8=workspace descriptor*. The
; descriptor is borrowed for this call; its data/index arrays are retained.
section '.text$fat_order_init' code readable executable comdat any align 16
proc fat_order_init uses rbx rdi
	; Backend/workspace stay volatile; neither is needed after the final reset.
	mov rbx, rcx
	mov r9, r8
	mov r8, rdx
	mov eax, F_ARGUMENT
	cmp qword [r8 + SectorOps.read], 0
	je .done
	cmp qword [r8 + SectorOps.sectors], 0
	je .done
	cmp dword [r9 + FatOrderWorkspace.reserved], 0
	jne .done
	mov ecx, [r8 + SectorOps.sector_bytes]
	cmp ecx, 512
	jb .done
	cmp ecx, F_MAX_SECTOR
	ja .done
	lea edx, [rcx-1]
	test ecx, edx
	jnz .done
	add ecx, sizeof.FatOrderSector
	mov rax, [r9 + FatOrderWorkspace.bytes]
	add rax, [r9 + FatOrderWorkspace.data]
	jc .argument
	mov rax, [r9 + FatOrderWorkspace.bytes]
	xor edx, edx
	div rcx
	test rax, rax
	jz .memory
	mov edx, 0FFFFFFFCh
	cmp rax, rdx
	ja .argument
	mov r10d, eax
	mov r11d, ecx
	mov ecx, [r9 + FatOrderWorkspace.slots]
	cmp ecx, 2
	jb .argument
	lea edx, [rcx-1]
	test ecx, edx
	jnz .argument
	mov eax, ecx
	shl rax, 2
	add rax, [r9 + FatOrderWorkspace.index]
	jc .argument
	mov rdi, rbx
	mov ecx, sizeof.FatOrder
	xor eax, eax
	rep stosb
	mov [rbx + FatOrder.capacity], r10d
	mov [rbx + FatOrder.stride], r11d
	mov eax, [r9 + FatOrderWorkspace.slots]
	mov [rbx + FatOrder.slots], eax
	mov rax, [r9 + FatOrderWorkspace.data]
	mov [rbx + FatOrder.data], rax
	mov rax, [r9 + FatOrderWorkspace.index]
	mov [rbx + FatOrder.index], rax
	mov [rbx + FatOrder.backend], r8
	mov [rbx + FatOrder.ops.base.context], rbx
	lea rax, [f_order_read]
	mov [rbx + FatOrder.ops.base.read], rax
	lea rax, [f_order_write]
	mov [rbx + FatOrder.ops.base.write], rax
	lea rax, [f_order_begin]
	mov [rbx + FatOrder.ops.base.begin], rax
	lea rax, [f_order_end]
	mov [rbx + FatOrder.ops.base.end], rax
	lea rax, [f_order_barrier]
	mov [rbx + FatOrder.ops.barrier], rax
	mov dword [rbx + FatOrder.ops.revision], 1
	mov dword [rbx + FatOrder.ops.base.reserved], sizeof.FatOrderedOps
	mov rax, [r8 + SectorOps.sectors]
	mov [rbx + FatOrder.ops.base.sectors], rax
	mov eax, [r8 + SectorOps.sector_bytes]
	mov [rbx + FatOrder.ops.base.sector_bytes], eax
	fastcall f_order_reset, rbx
.done:
	ret
.argument:
	mov eax, F_ARGUMENT
	jmp .done
.memory:
	mov eax, F_MEMORY
	jmp .done
endp

; RCX=order*, RDX=LBA. EAX=status, R11=index slot*, R10=latest node or zero.
; Preserves RCX/RDX, clobbers R8/R9. A half-full linear-probing hash has no
; filesystem meaning; it indexes pending sectors across all completed phases.
section '.text$f_order_find' code readable executable comdat align 16
proc f_order_find
	mov r8, rdx
	shr r8, 32
	xor r8d, edx
	imul r8d, 09E3779B1h
	mov r9d, [rcx + FatOrder.slots]
.probe:
	mov eax, [rcx + FatOrder.slots]
	dec eax
	and r8d, eax
	mov r11, [rcx + FatOrder.index]
	lea r11, [r11+r8*4]
	mov eax, [r11]
	test eax, eax
	jz .empty
	dec eax
	mov r10d, [rcx + FatOrder.stride]
	imul rax, r10
	mov r10, [rcx + FatOrder.data]
	add r10, rax
	cmp [r10 + FatOrderSector.lba], rdx
	je .found
	inc r8d
	dec r9d
	jnz .probe
	mov eax, F_MEMORY
	ret
.empty:
	xor r10d, r10d
.found:
	xor eax, eax
	ret
endp

section '.text$f_order_read' code readable executable comdat align 16
proc f_order_read uses rbx rsi rdi rbp
	mov rbx, rcx
	mov rbp, r8
	mov eax, F_IO
	cmp dword [rbx + FatOrder.poisoned], 1
	je .done
	mov eax, F_BUSY
	cmp dword [rbx + FatOrder.active], 2
	je .done
	mov eax, F_RANGE
	cmp rdx, [rbx + FatOrder.ops.base.sectors]
	jae .done
	fastcall f_order_find, rcx, rdx
	test eax, eax
	jnz .done
	test r10, r10
	jz .backend
	lea rsi, [r10 + sizeof.FatOrderSector]
	mov rdi, rbp
	mov ecx, [rbx + FatOrder.ops.base.sector_bytes]
	rep movsb
	xor eax, eax
.done:
	ret
.backend:
	mov rax, [rbx + FatOrder.backend]
	fastcall [rax + SectorOps.read], [rax + SectorOps.context], rdx, rbp
	jmp .done
endp

section '.text$f_order_write' code readable executable comdat align 16
proc f_order_write uses rbx rsi rdi r12 rbp
	mov rbx, rcx
	mov rsi, r8
	mov r12, rdx
	mov eax, F_BUSY
	cmp dword [rbx + FatOrder.active], 1
	jne .done
	mov eax, F_RANGE
	cmp rdx, [rbx + FatOrder.ops.base.sectors]
	jae .done
	fastcall f_order_find, rcx, rdx
	test eax, eax
	jnz .done
	mov rbp, r11
	test r10, r10
	jz .unique
	mov eax, [rbx + FatOrder.phase]
	cmp [r10 + FatOrderSector.phase], eax
	je .copy
	jmp .allocate
.unique:
	mov eax, [rbx + FatOrder.slots]
	shr eax, 1
	cmp [rbx + FatOrder.unique], eax
	jae .memory
.allocate:
	mov eax, [rbx + FatOrder.used]
	cmp eax, [rbx + FatOrder.capacity]
	jae .memory
	cmp dword [rbp], 0
	jne .version
	inc dword [rbx + FatOrder.unique]
.version:
	inc dword [rbx + FatOrder.used]
	mov edx, [rbx + FatOrder.used]
	mov [rbp], edx
	mov edx, [rbx + FatOrder.stride]
	imul rax, rdx
	mov r10, [rbx + FatOrder.data]
	add r10, rax
	mov [r10 + FatOrderSector.lba], r12
	mov eax, [rbx + FatOrder.phase]
	mov [r10 + FatOrderSector.phase], eax
	mov dword [r10 + FatOrderSector.reason], 0
.copy:
	lea rdi, [r10 + sizeof.FatOrderSector]
	mov ecx, [rbx + FatOrder.ops.base.sector_bytes]
	rep movsb
	xor eax, eax
.done:
	ret
.memory:
	mov eax, F_MEMORY
	jmp .done
endp

section '.text$f_order_begin' code readable executable comdat align 16
proc f_order_begin
	mov eax, [rcx + FatOrder.error]
	cmp dword [rcx + FatOrder.poisoned], 0
	jne .done
	mov eax, F_BUSY
	cmp dword [rcx + FatOrder.active], 0
	jne .done
	cmp dword [rcx + FatOrder.accepted], 0
	jne .done
	mov dword [rcx + FatOrder.active], 1
	xor eax, eax
.done:
	ret
endp

section '.text$f_order_end' code readable executable comdat align 16
proc f_order_end
	test edx, edx
	jz .rollback
	mov dword [rcx + FatOrder.active], 0
	mov dword [rcx + FatOrder.accepted], 1
	jmp .done
.rollback:
	fastcall f_order_reset, rcx
.done:
	ret
endp

section '.text$f_order_barrier' code readable executable comdat align 16
proc f_order_barrier
	mov eax, F_BUSY
	cmp dword [rcx + FatOrder.active], 1
	jne .done
	mov eax, F_LIMIT
	cmp dword [rcx + FatOrder.phase], 0FFFFFFFBh
	jae .done
	mov eax, [rcx + FatOrder.used]
	test eax, eax
	jz .advance
	dec eax
	mov r8d, [rcx + FatOrder.stride]
	imul rax, r8
	add rax, [rcx + FatOrder.data]
	mov r8d, [rcx + FatOrder.phase]
	cmp [rax + FatOrderSector.phase], r8d
	jne .advance
	mov [rax + FatOrderSector.reason], edx
.advance:
	inc dword [rcx + FatOrder.phase]
	xor eax, eax
.done:
	ret
endp

section '.text$fat_order_discard' code readable executable comdat any align 16
proc fat_order_discard uses rsi
	mov rsi, rdx
	mov eax, F_ARGUMENT
	cmp [rsi + FatIdentity.ops], rcx
	jne .done
	mov eax, F_BUSY
	cmp dword [rcx + FatOrder.active], 0
	jne .done
	fastcall f_order_reset, rcx
	fastcall fat_invalidate, rsi
.done:
	ret
endp

; Private execution context. Explicit verification hook keeps the strict
; policy independently discardable; ordinary commit has no reference to it.
struct FatCommitState
	?order dq ?
	?identity dq ?
	?report dq ?
	?verify dq ?
	?backend dq ?
ends

; RCX=state*, RDX=LBA; read backend into identity.fat, never the overlay.
section '.text$f_order_load' code readable executable comdat align 16
proc f_order_load
	mov r10, [rcx + FatCommitState.report]
	mov dword [r10 + FatCommitReport.operation], FI_READ
	mov [r10 + FatCommitReport.lba], rdx
	mov r8, [rcx + FatCommitState.identity]
	mov r8, [r8 + FatIdentity.fat]
	mov rax, [rcx + FatCommitState.backend]
	fastcall [rax + SectorOps.read], [rax + SectorOps.context], rdx, r8
	ret
endp

section '.text$f_order_flush' code readable executable comdat align 16
proc f_order_flush
	mov r10, [rcx + FatCommitState.report]
	mov dword [r10 + FatCommitReport.operation], FI_FLUSH
	mov qword [r10 + FatCommitReport.lba], -1
	inc qword [r10 + FatCommitReport.flushes]
	mov rax, [rcx + FatCommitState.backend]
	fastcall [rax + SectorOps.flush], [rax + SectorOps.context]
	ret
endp

; RCX=state*, RDX=LBA, R8=expected sector*. Effect becomes uncertain BEFORE
; dispatch: even a callback reporting failure may already have changed media.
section '.text$f_order_send' code readable executable comdat align 16
proc f_order_send uses rbx rsi rdi
	mov rbx, rcx
	mov rsi, rdx
	mov rdi, r8
	mov r10, [rbx + FatCommitState.report]
	mov dword [r10 + FatCommitReport.effect], FE_UNCERTAIN
	mov dword [r10 + FatCommitReport.operation], FI_WRITE
	mov [r10 + FatCommitReport.lba], rdx
	inc qword [r10 + FatCommitReport.writes]
	mov rax, [rbx + FatCommitState.backend]
	fastcall [rax + SectorOps.write], [rax + SectorOps.context], rdx, r8
	test eax, eax
	jnz .done
	cmp qword [rbx + FatCommitState.verify], 0
	je .done
	fastcall [rbx + FatCommitState.verify], rbx, rsi, rdi
.done:
	ret
endp

; RCX=state*, RDX=LBA, R8=expected. Strict policy's extra flush and backend
; readback. Uses a distinct scratch buffer; never reads the pending overlay.
section '.text$f_order_verify' code readable executable comdat align 16
proc f_order_verify uses rbx rsi rdi rbp
	mov rbx, rcx
	mov rbp, rdx
	mov rsi, r8
	fastcall f_order_flush, rcx
	test eax, eax
	jnz .done
	mov r10, [rbx + FatCommitState.report]
	mov dword [r10 + FatCommitReport.operation], FI_VERIFY
	mov [r10 + FatCommitReport.lba], rbp
	mov rdi, [rbx + FatCommitState.identity]
	mov rdi, [rdi + FatIdentity.scratch]
	mov rax, [rbx + FatCommitState.backend]
	fastcall [rax + SectorOps.read], [rax + SectorOps.context], rbp, rdi
	test eax, eax
	jnz .done
	mov rcx, [rbx + FatCommitState.backend]
	mov ecx, [rcx + SectorOps.sector_bytes]
	repe cmpsb
	je .done
	mov eax, F_VERIFY
.done:
	ret
endp

; RCX=id*, RDX=LBA -> EAX=1 for the first sector of any FAT copy, else 0.
; Volatile registers clobbered. Caller retains its LBA independently.
section '.text$f_order_reserved' code readable executable comdat align 16
proc f_order_reserved
	mov eax, [rcx + FatIdentity.fat_start]
	cmp rdx, rax
	jb .no
	mov r8d, [rcx + FatIdentity.data_start]
	cmp rdx, r8
	jae .no
	sub rdx, rax
	mov rax, rdx
	xor edx, edx
	mov r9d, [rcx + FatIdentity.fat_sectors]
	div r9
	test rdx, rdx
	sete al
	movzx eax, al
	ret
.no:
	xor eax, eax
	ret
endp

section '.text$fat_order_commit' code readable executable comdat any align 16
proc fat_order_commit
	fastcall f_order_run, rcx, rdx, r8, 0
	ret
endp

section '.text$fat_order_commit_verified' code readable executable comdat any align 16
proc fat_order_commit_verified
	lea r9, [f_order_verify]
	fastcall f_order_run, rcx, rdx, r8, r9
	ret
endp

; RCX=order*, RDX=id*, R8=report*, R9=optional verification hook.
section '.text$f_order_run' code readable executable comdat align 16
proc f_order_run uses rbx rsi rdi rbp r13 r14 r15
	locals
		state FatCommitState
		first dd ?
		limit dd ?
		mode dd ?
		cursor dd ?
	endl
body:
	mov rbx, rcx
	mov rsi, rdx
	mov rbp, r8
	mov [state.order], rbx
	mov [state.identity], rsi
	mov [state.report], rbp
	mov [state.verify], r9
	mov r15, [rbx + FatOrder.backend]
	mov [state.backend], r15
	mov rdi, rbp
	mov ecx, sizeof.FatCommitReport
	xor eax, eax
	rep stosb
	mov qword [rbp + FatCommitReport.lba], -1
	mov dword [rbp + FatCommitReport.phase], FP_ADMIT
	mov eax, F_ARGUMENT
	cmp [rsi + FatIdentity.ops], rbx
	jne .result
	mov eax, F_FORMAT
	cmp dword [rsi + FatIdentity.magic], F_MAGIC
	jne .result
	mov eax, F_BUSY
	cmp dword [rbx + FatOrder.active], 0
	jne .result
	cmp dword [rsi + FatIdentity.transaction], 0
	jne .result
	cmp dword [rbx + FatOrder.poisoned], 0
	jne .previous
	cmp dword [rbx + FatOrder.used], 0
	je .empty
	mov eax, F_READONLY
	cmp qword [r15 + SectorOps.write], 0
	je .result
	cmp qword [r15 + SectorOps.flush], 0
	je .result
	mov dword [rbx + FatOrder.active], 2
	mov qword [rsi + FatIdentity.fat_lba], -1
	mov dword [first], 0
	mov eax, [rsi + FatIdentity.fat_count]
	mov [limit], eax
	cmp dword [rsi + FatIdentity.mirrored], 0
	jne .admit
	mov eax, [rsi + FatIdentity.active_fat]
	mov [first], eax
	inc eax
	mov [limit], eax
.admit:
	mov dword [mode], 0
.copies:
	mov r13d, [first]
.copy:
	mov r14d, r13d
	imul r14d, [rsi + FatIdentity.fat_sectors]
	add r14d, [rsi + FatIdentity.fat_start]
	fastcall f_order_load, addr state, r14
	test eax, eax
	jnz .failure
	mov rdi, [rsi + FatIdentity.fat]
	cmp dword [mode], 0
	jne .change
	mov eax, [rdi+4]
	and eax, 03FFFFFFh
	cmp eax, 03FFFFFFh
	jne .damaged
	mov eax, [rdi+4]
	and eax, 0C000000h
	cmp eax, 0C000000h
	jne .attention
	jmp .next_copy
.change:
	and dword [rdi+4], not 08000000h
	cmp dword [mode], 2
	jne .send_status
	or dword [rdi+4], 08000000h
.send_status:
	fastcall f_order_send, addr state, r14, rdi
	test eax, eax
	jnz .failure
.next_copy:
	inc r13d
	cmp r13d, [limit]
	jb .copy
	cmp dword [mode], 0
	jne .status_flush
	mov dword [mode], 1
	mov dword [rbp + FatCommitReport.phase], FP_DIRTY
	jmp .copies
.status_flush:
	fastcall f_order_flush, addr state
	test eax, eax
	jnz .failure
	cmp dword [mode], 2
	je .success
	mov dword [cursor], 0
.node:
	mov eax, [cursor]
	mov edx, [rbx + FatOrder.stride]
	imul rax, rdx
	mov r13, [rbx + FatOrder.data]
	add r13, rax
	mov eax, [r13 + FatOrderSector.phase]
	mov [rbp + FatCommitReport.phase], eax
	mov r14, [r13 + FatOrderSector.lba]
	fastcall f_order_reserved, rsi, r14
	lea r8, [r13 + sizeof.FatOrderSector]
	test eax, eax
	jz .send_node
	; Overlay payloads were staged before the initial dirty marker. Never let
	; a sector version containing FAT[1] silently make the volume clean early.
	mov rdi, [rsi + FatIdentity.fat]
	mov r9, rsi
	mov rsi, r8
	mov ecx, [rbx + FatOrder.ops.base.sector_bytes]
	rep movsb
	mov rsi, r9
	mov r8, [rsi + FatIdentity.fat]
	and dword [r8+4], not 08000000h
.send_node:
	fastcall f_order_send, addr state, r14, r8
	test eax, eax
	jnz .failure
	inc dword [cursor]
	mov eax, [cursor]
	cmp eax, [rbx + FatOrder.used]
	jae .last_phase
	mov edx, [rbx + FatOrder.stride]
	add r13, rdx
	mov eax, [r13 + FatOrderSector.phase]
	cmp eax, [rbp + FatCommitReport.phase]
	je .node
	fastcall f_order_flush, addr state
	test eax, eax
	jnz .failure
	jmp .node
.last_phase:
	fastcall f_order_flush, addr state
	test eax, eax
	jnz .failure
	mov dword [mode], 2
	mov dword [rbp + FatCommitReport.phase], FP_CLEAN
	jmp .copies
.success:
	mov dword [rbp + FatCommitReport.effect], FE_COMMITTED
.empty:
	fastcall f_order_reset, rbx
	xor eax, eax
	jmp .result
.damaged:
	mov eax, F_CORRUPT
	jmp .restrict
.attention:
	mov eax, F_ATTENTION
.restrict:
	mov dword [rbx + FatOrder.poisoned], 2
.failure:
	mov dword [rbx + FatOrder.active], 0
	mov [rbp + FatCommitReport.status], eax
	cmp dword [rbp + FatCommitReport.effect], FE_UNCERTAIN
	jne .remember
	mov dword [rbx + FatOrder.poisoned], 1
	fastcall fat_invalidate, rsi
	mov eax, [rbp + FatCommitReport.status]
.remember:
	cmp dword [rbx + FatOrder.poisoned], 0
	je .result
	mov [rbx + FatOrder.error], eax
	mov edx, [rbp + FatCommitReport.phase]
	mov [rbx + FatOrder.error_phase], edx
	mov edx, [rbp + FatCommitReport.operation]
	mov [rbx + FatOrder.error_operation], edx
	mov rdx, [rbp + FatCommitReport.lba]
	mov [rbx + FatOrder.error_lba], rdx
	mov rdx, [rbp + FatCommitReport.writes]
	mov [rbx + FatOrder.writes], rdx
	mov rdx, [rbp + FatCommitReport.flushes]
	mov [rbx + FatOrder.flushes], rdx
.result:
	mov [rbp + FatCommitReport.status], eax
	ret
.previous:
	mov eax, [rbx + FatOrder.error_phase]
	mov [rbp + FatCommitReport.phase], eax
	mov eax, [rbx + FatOrder.error_operation]
	mov [rbp + FatCommitReport.operation], eax
	mov rax, [rbx + FatOrder.error_lba]
	mov [rbp + FatCommitReport.lba], rax
	mov rax, [rbx + FatOrder.writes]
	mov [rbp + FatCommitReport.writes], rax
	mov rax, [rbx + FatOrder.flushes]
	mov [rbp + FatCommitReport.flushes], rax
	cmp dword [rbx + FatOrder.poisoned], 1
	jne .previous_status
	mov dword [rbp + FatCommitReport.effect], FE_UNCERTAIN
.previous_status:
	mov eax, [rbx + FatOrder.error]
	jmp .result
endp
