; Optional range callback over the order provider's logical view. A suspended
; backend range must retain the same owner lease as ordinary reads/commit.
section '.text$fat_order_read_range' code readable executable comdat any align 16
proc fat_order_read_range uses rbx rsi rdi rbp r13 r14 r15
	locals
		request FatRangeRequest
		first dq ?
		span dd ?
	endl
body:
	mov rbp, rdx
	mov rbx, [rcx + FatOrderRange.order]
	mov r13, [rcx + FatOrderRange.backend]
	mov dword [rbp + FatRangeRequest.done], 0
	mov eax, F_IO
	cmp dword [rbx + FatOrder.poisoned], 1
	je .done
	mov eax, F_BUSY
	cmp dword [rbx + FatOrder.active], 2
	je .done
	mov eax, F_ARGUMENT
	cmp qword [r13 + FatRangeOps.read], 0
	je .done
	cmp dword [r13 + FatRangeOps.maximum], 0
	je .done
	cmp dword [r13 + FatRangeOps.reserved], 0
	jne .done
	mov r14d, [rbx + FatOrder.ops.base.sector_bytes]
	mov eax, [rbp + FatRangeRequest.count]
	imul rax, r14
	add rax, [rbp + FatRangeRequest.data]
	jc .argument
	mov eax, [rbp + FatRangeRequest.count]
	add rax, [rbp + FatRangeRequest.lba]
	jc .range
	cmp rax, [rbx + FatOrder.ops.base.sectors]
	ja .range
.loop:
	mov eax, [rbp + FatRangeRequest.done]
	cmp eax, [rbp + FatRangeRequest.count]
	jae .ok
	mov r15, rax
	add r15, [rbp + FatRangeRequest.lba]
	mov [first], r15
	imul rax, r14
	add rax, [rbp + FatRangeRequest.data]
	mov [request.data], rax
	fastcall f_order_find, rbx, r15
	test eax, eax
	jnz .done
	test r10, r10
	jz .backend
	lea rsi, [r10 + sizeof.FatOrderSector]
	mov rdi, [request.data]
	mov ecx, r14d
	rep movsb
	inc dword [rbp + FatRangeRequest.done]
	jmp .loop
.backend:
	mov eax, [rbp + FatRangeRequest.count]
	sub eax, [rbp + FatRangeRequest.done]
	cmp eax, [r13 + FatRangeOps.maximum]
	cmova eax, [r13 + FatRangeOps.maximum]
	mov edi, eax
	mov esi, 1
.scan:
	cmp esi, edi
	jae .read
	inc r15
	fastcall f_order_find, rbx, r15
	test eax, eax
	jnz .done
	test r10, r10
	jnz .read
	inc esi
	jmp .scan
.read:
	mov rax, [first]
	mov [request.lba], rax
	mov [request.count], esi
	mov [span], esi
	mov dword [request.done], 0
	fastcall [r13 + FatRangeOps.read], [r13 + FatRangeOps.context], addr request
	mov edi, eax
	mov eax, [request.done]
	cmp eax, [span]
	ja .io
	add [rbp + FatRangeRequest.done], eax
	test edi, edi
	jnz .status
	cmp eax, [span]
	jne .io
	jmp .loop
.ok:
	xor eax, eax
.done:
	ret
.status:
	mov eax, edi
	jmp .done
.io:
	mov eax, F_IO
	jmp .done
.argument:
	mov eax, F_ARGUMENT
	jmp .done
.range:
	mov eax, F_RANGE
	jmp .done
endp
