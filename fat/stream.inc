; Optional reusable maps. Base file operations never reference this module.
section '.text$fat_stream_open' code readable executable comdat any align 16
proc fat_stream_open uses rbx rsi rdi rbp r13 r15
	locals
		next dd ?
		count dd ?
		owner dq ?
		object dq ?
	endl
body:
	mov rbp, rdx
	mov r13, r8
	fastcall f_handle, rcx
	test eax, eax
	jnz .done
	mov [owner], r10
	mov [object], r11
	mov rbx, [r10 + FatVolume.identity]
	mov eax, F_ARGUMENT
	test dword [rcx + FatHandle.access], FH_READ
	jz .done
	cmp dword [r13 + FatStreamWorkspace.reserved], 0
	jne .done
	mov eax, F_BUSY
	cmp qword [rbp + FatHandle.volume], 0
	jne .done
	fastcall f_basic_file, rbx, r11
	test eax, eax
	jnz .done
	mov r11, [object]
	mov eax, [r11 + FatRecord.size]
	mov ecx, [rbx + FatIdentity.cluster_bytes]
	lea rax, [rax+rcx-1]
	xor edx, edx
	div rcx
	mov [count], eax
	cmp eax, [rbx + FatIdentity.cluster_count]
	ja .bad
	cmp eax, [r13 + FatStreamWorkspace.capacity]
	ja .memory
	mov edi, [r13 + FatStreamWorkspace.capacity]
	shl rdi, 2
	add rdi, [r13 + FatStreamWorkspace.map]
	jc .argument
	mov r15, [r13 + FatStreamWorkspace.map]
	mov esi, [r11 + FatRecord.cluster]
	xor edi, edi
	cmp dword [count], 0
	je .bind
.map:
	lea eax, [rsi-2]
	cmp eax, [rbx + FatIdentity.cluster_count]
	jae .bad
	mov [r15+rdi*4], esi
	inc edi
	cmp edi, [count]
	jae .bind
	fastcall fat_get, rbx, esi, addr next
	test eax, eax
	jnz .done
	mov esi, [next]
	jmp .map
.bind:
	fastcall f_bind_handle, [owner], [object], rbp, FH_READ
	test eax, eax
	jnz .done
	mov r11, [object]
	mov rax, [r11 + FatObject.chain_version]
	mov [rbp + FatStream.version], rax
	mov [rbp + FatStream.map], r15
	mov eax, [count]
	mov [rbp + FatStream.count], eax
	mov eax, [r13 + FatStreamWorkspace.capacity]
	mov [rbp + FatStream.capacity], eax
	xor eax, eax
.done:
	ret
.argument:
	mov eax, F_ARGUMENT
	jmp .done
.memory:
	mov eax, F_MEMORY
	jmp .done
.bad:
	mov eax, F_CORRUPT
	jmp .done
endp

section '.text$fat_stream_close' code readable executable comdat any align 16
proc fat_stream_close
	fastcall fat_close, rcx
	ret
endp

; RCX=stream*, RDX=transfer*. EAX=status, R10=id*, R9D=clamped byte count.
; Preserves RCX/RDX/R8. No I/O. Failed/stale reads report zero completed bytes.
section '.text$f_stream_prepare' code readable executable comdat align 16
proc f_stream_prepare
	mov dword [rdx + FatTransfer.done], 0
	fastcall f_handle, rcx
	test eax, eax
	jnz .done
	mov r9, qword [rcx + FatStream.version]
	mov eax, F_STALE
	cmp r9, [r11 + FatObject.chain_version]
	jne .done
	mov r10, [r10 + FatVolume.identity]
	mov r9d, [r11 + FatRecord.size]
	sub r9, [rdx + FatTransfer.offset]
	jb .empty
	mov eax, [rdx + FatTransfer.length]
	cmp r9, rax
	cmova r9, rax
	xor eax, eax
.done:
	ret
.empty:
	xor r9d, r9d
	xor eax, eax
	jmp .done
endp

; RCX=id*, RDX=stream*, R8=file offset. EAX=status, R10=LBA, R11D=sector offset.
section '.text$f_stream_lba' code readable executable comdat align 16
proc f_stream_lba
	mov r9, rdx
	mov rax, r8
	xor edx, edx
	div dword [rcx + FatIdentity.cluster_bytes]
	cmp eax, [r9 + FatStream.count]
	jae .bad
	mov r8, [r9 + FatStream.map]
	mov r10d, [r8+rax*4]
	sub r10d, 2
	cmp r10d, [rcx + FatIdentity.cluster_count]
	jae .bad
	mov eax, edx
	xor edx, edx
	div dword [rcx + FatIdentity.sector_bytes]
	mov r11d, edx
	mov r8d, [rcx + FatIdentity.cluster_sectors]
	imul r10, r8
	add r10, rax
	mov eax, [rcx + FatIdentity.data_start]
	add r10, rax
	xor eax, eax
	ret
.bad:
	mov eax, F_CORRUPT
	ret
endp

; Optional transport-facing sector map; preserve canonical stream lifetime and
; keep FAT geometry/cluster translation out of consumers. No provider access.
section '.text$fat_stream_sector' code readable executable comdat any align 16
proc fat_stream_sector uses rbx rsi rdi
	mov rbx, rcx
	mov rsi, rdx
	mov rdi, r8
	fastcall f_handle, rcx
	test eax, eax
	jnz .done
	mov r9, qword [rbx + FatStream.version]
	mov eax, F_STALE
	cmp r9, [r11 + FatObject.chain_version]
	jne .done
	mov r10, [r10 + FatVolume.identity]
	mov eax, [r11 + FatRecord.size]
	test eax, eax
	jz .end
	dec eax
	xor edx, edx
	div dword [r10 + FatIdentity.sector_bytes]
	cmp rsi, rax
	ja .end
	mov r8d, [r10 + FatIdentity.sector_bytes]
	imul r8, rsi
	fastcall f_stream_lba, r10, rbx, r8
	test eax, eax
	jnz .done
	mov [rdi], r10
.done:
	ret
.end:
	mov eax, F_END
	jmp .done
endp

section '.text$fat_stream_read' code readable executable comdat any align 16
proc fat_stream_read uses rbx rsi rdi r12 r13 rbp
	locals
		span dd ?
		inside dd ?
	endl
body:
	mov r12, rcx
	mov r13, rdx
	fastcall f_stream_prepare, rcx, rdx
	test eax, eax
	jnz .done
	mov rbx, r10
	mov ebp, r9d
.loop:
	test ebp, ebp
	jz .ok
	mov r8d, [r13 + FatTransfer.done]
	add r8, [r13 + FatTransfer.offset]
	fastcall f_stream_lba, rbx, r12, r8
	test eax, eax
	jnz .done
	mov [inside], r11d
	mov eax, [rbx + FatIdentity.sector_bytes]
	sub eax, r11d
	cmp eax, ebp
	cmova eax, ebp
	mov [span], eax
	mov edi, [r13 + FatTransfer.done]
	add rdi, [r13 + FatTransfer.data]
	cmp eax, [rbx + FatIdentity.sector_bytes]
	jne .partial
	fastcall f_read_sector, rbx, r10, rdi
	test eax, eax
	jnz .done
	jmp .advance
.partial:
	fastcall f_read_sector, rbx, r10, [rbx + FatIdentity.scratch]
	test eax, eax
	jnz .done
	mov esi, [inside]
	add rsi, [rbx + FatIdentity.scratch]
	mov ecx, [span]
	rep movsb
.advance:
	mov eax, [span]
	add [r13 + FatTransfer.done], eax
	sub ebp, eax
	jmp .loop
.ok:
	xor eax, eax
.done:
	ret
endp

; Coalesce physically adjacent mapped clusters, bounded by the provider maximum.
; Partial edge sectors reuse the ordinary stream primitive. No FAT/dir I/O.
section '.text$fat_stream_read_range' code readable executable comdat any align 16
proc fat_stream_read_range uses rbx rsi r12 r13 r14 rbp
	locals
		request FatRangeRequest
		partial FatTransfer
		wanted dd ?
		position dq ?
	endl
body:
	mov r12, rcx
	mov r13, rdx
	mov r14, r8
	fastcall f_stream_prepare, rcx, rdx
	test eax, eax
	jnz .done
	mov rbx, r10
	mov ebp, r9d
	mov eax, F_ARGUMENT
	cmp qword [r14 + FatRangeOps.read], 0
	je .done
	cmp dword [r14 + FatRangeOps.maximum], 0
	je .done
	cmp dword [r14 + FatRangeOps.reserved], 0
	jne .done
.loop:
	test ebp, ebp
	jz .ok
	mov r8d, [r13 + FatTransfer.done]
	add r8, [r13 + FatTransfer.offset]
	mov [position], r8
	fastcall f_stream_lba, rbx, r12, r8
	test eax, eax
	jnz .done
	mov [request.lba], r10
	mov eax, [r13 + FatTransfer.done]
	add rax, [r13 + FatTransfer.data]
	mov [request.data], rax
	test r11d, r11d
	jnz .partial
	cmp ebp, [rbx + FatIdentity.sector_bytes]
	jb .partial
	mov eax, ebp
	xor edx, edx
	div dword [rbx + FatIdentity.sector_bytes]
	cmp eax, [r14 + FatRangeOps.maximum]
	cmova eax, [r14 + FatRangeOps.maximum]
	mov [wanted], eax
	mov rax, [position]
	xor edx, edx
	div dword [rbx + FatIdentity.cluster_bytes]
	mov ecx, eax
	mov eax, [rbx + FatIdentity.cluster_bytes]
	sub eax, edx
	xor edx, edx
	div dword [rbx + FatIdentity.sector_bytes]
	mov esi, eax
	mov r8, [r12 + FatStream.map]
.extend:
	cmp esi, [wanted]
	jae .full
	mov eax, [r8+rcx*4]
	inc ecx
	cmp ecx, [r12 + FatStream.count]
	jae .request
	inc eax
	cmp eax, [r8+rcx*4]
	jne .request
	add esi, [rbx + FatIdentity.cluster_sectors]
	jmp .extend
.full:
	mov esi, [wanted]
.request:
	mov [request.count], esi
	mov dword [request.done], 0
	fastcall [r14 + FatRangeOps.read], [r14 + FatRangeOps.context], addr request
	mov edx, [request.done]
	cmp edx, esi
	ja .io
	imul edx, [rbx + FatIdentity.sector_bytes]
	add [r13 + FatTransfer.done], edx
	sub ebp, edx
	test eax, eax
	jnz .done
	cmp esi, [request.done]
	jne .io
	jmp .loop
.partial:
	mov [partial.data], rax
	mov rax, [position]
	mov [partial.offset], rax
	mov eax, [rbx + FatIdentity.sector_bytes]
	sub eax, r11d
	cmp eax, ebp
	cmova eax, ebp
	mov [partial.length], eax
	fastcall fat_stream_read, r12, addr partial
	mov edx, [partial.done]
	add [r13 + FatTransfer.done], edx
	sub ebp, edx
	test eax, eax
	jnz .done
	jmp .loop
.ok:
	xor eax, eax
.done:
	ret
.io:
	mov eax, F_IO
	jmp .done
endp
