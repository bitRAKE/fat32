; File data and metadata. Public mutations work on a private entry snapshot;
; a failed operation changes neither caller metadata nor the sector overlay.

; Verify snapshot, slot membership, and raw bytes before touching a file.
proc f_validate uses rbx rsi rdi
	locals
		location dq ?
		offset dd ?
	endl
body:
	mov rbx, rcx
	mov rsi, rdx
	cmp dword [rbx + FatIdentity.magic], F_MAGIC
	jne .bad
	mov rax, qword [rbx + FatIdentity.generation]
	cmp rax, [rsi + FatEntry.generation]
	jne .stale
	mov eax, dword [rsi + FatEntry.parent]
	cmp eax, 2
	jb .bad
	mov ecx, dword [rbx + FatIdentity.cluster_count]
	inc ecx
	cmp eax, ecx
	ja .bad
	fastcall f_slot, rbx, eax, [rsi + FatEntry.index], addr location
	test eax, eax
	jnz .done
	mov rax, qword [location]
	cmp rax, [rsi + FatEntry.sector]
	jne .bad
	mov eax, dword [offset]
	cmp eax, [rsi + FatEntry.offset]
	jne .bad
	fastcall f_directory, rbx, [location]
	test eax, eax
	jnz .done
	mov edi, dword [offset]
	add rdi, rbx
	add rdi, FatIdentity.directory
	iterate n, 0,8,16,24
		mov rax, qword [rdi+n]
		cmp rax, qword [rsi + FatEntry.raw+n]
		jne .stale
	end iterate
	cmp byte [rdi], 0
	je .bad
	cmp byte [rdi], 0E5h
	je .bad
	test byte [rdi+11], 8
	jnz .bad
	movzx eax, word [rdi+20]
	shl eax, 16
	mov ax, word [rdi+26]
	and eax, F_MASK
	cmp eax, [rsi + FatEntry.cluster]
	jne .bad
	mov eax, dword [rdi+28]
	cmp eax, [rsi + FatEntry.size]
	jne .bad
	xor eax, eax
.done:
	ret
.bad:
	mov eax, F_CORRUPT
	ret
.stale:
	mov eax, F_STALE
	ret
endp

; Check ordinary file and full chain length, not merely the requested prefix.
proc f_file uses rbx rsi
	locals
		chain dq ?
	endl
body:
	mov rbx, rcx
	mov rsi, rdx
	fastcall f_validate, rbx, rsi
	test eax, eax
	jnz .done
	test byte [rsi + FatEntry.raw+11], 18h
	jnz .bad
	fastcall fat_chain, rbx, [rsi + FatEntry.cluster], addr chain
	test eax, eax
	jnz .done
	mov eax, dword [rsi + FatEntry.size]
	mov ecx, dword [rbx + FatIdentity.cluster_bytes]
	lea rax, [rax+rcx-1]
	xor edx, edx
	div rcx
	cmp eax, dword [chain]
	ja .bad
	xor eax, eax
.done:
	ret
.bad:
	mov eax, F_CORRUPT
	ret
endp

; Walk a prevalidated chain to index. Output cluster at R9.
proc f_nth uses rbx rsi rdi r12
	locals
		next dd ?
	endl
body:
	mov rbx, rcx
	mov esi, edx
	mov edi, r8d
	mov r12, r9
.loop:
	test edi, edi
	jz .found
	fastcall fat_get, rbx, esi, addr next
	test eax, eax
	jnz .done
	mov esi, dword [next]
	cmp esi, 2
	jb .bad
	mov eax, dword [rbx + FatIdentity.cluster_count]
	inc eax
	cmp esi, eax
	ja .bad
	dec edi
	jmp .loop
.found:
	mov dword [r12], esi
	xor eax, eax
.done:
	ret
.bad:
	mov eax, F_CORRUPT
	ret
endp

; Transfer a bounded byte range in a prevalidated, sufficiently long chain.
; mode 0 reads, 1 writes, 2 zeroes. Always uses a complete sector buffer.
proc f_bytes uses rbx rsi rdi r12 r13 r14 r15
	locals
		cluster dd ?
		inside dd ?
		mode dd ?
		span dd ?
		lba dq ?
	endl
body:
	mov rbx, rcx
	mov r12, r8
	mov dword [mode], r9d
	mov dword [r12 + FatTransfer.done], 0
	cmp dword [r12 + FatTransfer.length], 0
	je .ok
	mov esi, edx
	mov rax, qword [r12 + FatTransfer.offset]
	xor edx, edx
	mov ecx, dword [rbx + FatIdentity.cluster_bytes]
	div rcx
	mov dword [inside], edx
	fastcall f_nth, rbx, esi, eax, addr cluster
	test eax, eax
	jnz .done
.loop:
	mov eax, dword [inside]
	xor edx, edx
	div dword [rbx + FatIdentity.sector_bytes]
	mov r13d, edx           ; offset inside sector
	mov r14d, eax           ; sector inside cluster
	fastcall f_cluster_sector, rbx, [cluster]
	add rax, r14
	mov qword [lba], rax
	fastcall f_read_sector, rbx, rax, addr rbx + FatIdentity.scratch
	test eax, eax
	jnz .done
	mov eax, dword [r12 + FatTransfer.length]
	sub eax, [r12 + FatTransfer.done]
	mov ecx, dword [rbx + FatIdentity.sector_bytes]
	sub ecx, r13d
	cmp ecx, eax
	cmova ecx, eax
	mov dword [span], ecx
	mov esi, dword [r12 + FatTransfer.done]
	add rsi, [r12 + FatTransfer.data]
	lea rdi, [rbx + FatIdentity.scratch+r13]
	cmp dword [mode], 0
	je .read
	cmp dword [mode], 2
	je .zero
	rep movsb
	jmp .write
.zero:
	xor eax, eax
	rep stosb
.write:
	fastcall f_write_sector, rbx, [lba], addr rbx + FatIdentity.scratch
	test eax, eax
	jnz .done
	jmp .advance
.read:
	xchg rdi, rsi
	rep movsb
.advance:
	mov eax, dword [span]
	add [r12 + FatTransfer.done], eax
	add [inside], eax
	mov eax, dword [r12 + FatTransfer.done]
	cmp eax, [r12 + FatTransfer.length]
	je .ok
	mov eax, dword [inside]
	cmp eax, [rbx + FatIdentity.cluster_bytes]
	jb .loop
	mov dword [inside], 0
	fastcall fat_get, rbx, [cluster], addr cluster
	test eax, eax
	jnz .done
	jmp .loop
.ok:
	xor eax, eax
.done:
	ret
endp

proc fat_read uses rbx rsi rdi
	locals
		request FatTransfer
	endl
body:
	mov rbx, rcx
	mov rsi, rdx
	mov rdi, r8
	mov dword [rdi + FatTransfer.done], 0
	fastcall f_file, rbx, rsi
	test eax, eax
	jnz .done
	mov eax, dword [rsi + FatEntry.size]
	mov rdx, qword [rdi + FatTransfer.offset]
	cmp rdx, rax
	jae .ok
	sub eax, edx
	cmp eax, [rdi + FatTransfer.length]
	cmova eax, [rdi + FatTransfer.length]
	mov dword [request.length], eax
	mov qword [request.offset], rdx
	mov rax, qword [rdi + FatTransfer.data]
	mov qword [request.data], rax
	fastcall f_bytes, rbx, [rsi + FatEntry.cluster], addr request, 0
	mov edx, dword [request.done]
	mov dword [rdi + FatTransfer.done], edx
.done:
	ret
.ok:
	xor eax, eax
	ret
endp

; Write the raw SFN to its slot without disturbing neighboring entries/LFNs.
proc f_store uses rbx rsi rdi
	mov rbx, rcx
	mov rsi, rdx
	fastcall f_directory, rbx, [rsi + FatEntry.sector]
	test eax, eax
	jnz .done
	mov edi, dword [rsi + FatEntry.offset]
	lea rdi, [rbx + FatIdentity.directory+rdi]
	iterate n, 0,8,16,24
		mov rax, qword [rsi + FatEntry.raw+n]
		mov qword [rdi+n], rax
	end iterate
	fastcall f_write_sector, rbx, [rsi + FatEntry.sector], addr rbx + FatIdentity.directory
.done:
	ret
endp

; Resize within an existing transaction. Entry is a private work copy.
proc f_resize uses rbx rsi rdi r12 r13 r14
	locals
		chain dq ?
		new_cluster dd ?
		tail dd ?
		request FatTransfer
	endl
body:
	mov rbx, rcx
	mov rsi, rdx
	mov r12d, r8d
	fastcall fat_chain, rbx, [rsi + FatEntry.cluster], addr chain
	test eax, eax
	jnz .done
	mov eax, r12d
	mov ecx, dword [rbx + FatIdentity.cluster_bytes]
	lea rax, [rax+rcx-1]
	xor edx, edx
	div rcx
	mov r13d, eax          ; target cluster count
	mov edi, dword [chain]
	mov r14d, dword [chain+4]
	cmp edi, r13d
	ja .shrink
.grow:
	cmp edi, r13d
	jae .size
	fastcall f_allocate, rbx, addr new_cluster
	test eax, eax
	jnz .done
	test r14d, r14d
	jz .first
	fastcall f_put, rbx, r14d, [new_cluster]
	test eax, eax
	jnz .done
	jmp .linked
.first:
	mov eax, dword [new_cluster]
	mov dword [rsi + FatEntry.cluster], eax
.linked:
	mov r14d, dword [new_cluster]
	inc edi
	jmp .grow
.shrink:
	test r13d, r13d
	jz .all
	lea r8d, [r13d-1]
	fastcall f_nth, rbx, [rsi + FatEntry.cluster], r8d, addr new_cluster
	test eax, eax
	jnz .done
	fastcall fat_get, rbx, [new_cluster], addr tail
	test eax, eax
	jnz .done
	fastcall f_put, rbx, [new_cluster], F_LAST
	test eax, eax
	jnz .done
	fastcall f_free, rbx, [tail]
	test eax, eax
	jnz .done
	jmp .size
.all:
	fastcall f_free, rbx, [rsi + FatEntry.cluster]
	test eax, eax
	jnz .done
	mov dword [rsi + FatEntry.cluster], 0
.size:
	mov eax, dword [rsi + FatEntry.size]
	cmp r12d, eax
	jbe .metadata
	mov qword [request.offset], rax
	mov ecx, r12d
	sub ecx, eax
	mov dword [request.length], ecx
	mov qword [request.data], 0
	fastcall f_bytes, rbx, [rsi + FatEntry.cluster], addr request, 2
	test eax, eax
	jnz .done
.metadata:
	mov dword [rsi + FatEntry.size], r12d
	mov dword [rsi + FatEntry.raw+28], r12d
	mov eax, dword [rsi + FatEntry.cluster]
	mov word [rsi + FatEntry.raw+26], ax
	shr eax, 16
	mov word [rsi + FatEntry.raw+20], ax
	or byte [rsi + FatEntry.raw+11], 20h
	fastcall f_store, rbx, rsi
.done:
	ret
endp

proc fat_resize uses rbx rsi rdi r12 r13
	locals
		entry FatEntry
	endl
body:
	mov rbx, rcx
	mov r12, rdx
	mov r13, r8
	shr r8, 32
	jnz .range
	fastcall f_file, rbx, r12
	test eax, eax
	jnz .done
	test byte [r12 + FatEntry.raw+11], 1
	jnz .ro
	mov rsi, r12
	lea rdi, [entry]
	mov ecx, sizeof.FatEntry
	rep movsb
	fastcall f_begin, rbx
	test eax, eax
	jnz .done
	fastcall f_resize, rbx, addr entry, r13d
	fastcall f_finish, rbx, eax
	test eax, eax
	jnz .done
	mov rax, qword [rbx + FatIdentity.generation]
	mov qword [entry.generation], rax
	lea rsi, [entry]
	mov rdi, r12
	mov ecx, sizeof.FatEntry
	rep movsb
	xor eax, eax
.done:
	ret
.range:
	mov eax, F_RANGE
	ret
.ro:
	mov eax, F_READONLY
	ret
endp

proc fat_write uses rbx rsi rdi r12 r13 r14
	locals
		entry FatEntry
		request FatTransfer
	endl
body:
	mov rbx, rcx
	mov r12, rdx
	mov r13, r8
	mov dword [r13 + FatTransfer.done], 0
	mov r14, qword [r13 + FatTransfer.offset]
	mov eax, dword [r13 + FatTransfer.length]
	add r14, rax
	jc .range
	mov rax, r14
	shr rax, 32
	jnz .range
	fastcall f_file, rbx, r12
	test eax, eax
	jnz .done
	test byte [r12 + FatEntry.raw+11], 1
	jnz .ro
	cmp dword [r13 + FatTransfer.length], 0
	je .ok
	mov rsi, r12
	lea rdi, [entry]
	mov ecx, sizeof.FatEntry
	rep movsb
	mov rsi, r13
	lea rdi, [request]
	mov ecx, sizeof.FatTransfer
	rep movsb
	fastcall f_begin, rbx
	test eax, eax
	jnz .done
	cmp r14d, [entry.size]
	jbe .write
	fastcall f_resize, rbx, addr entry, r14d
	test eax, eax
	jnz .finish
.write:
	fastcall f_bytes, rbx, [entry.cluster], addr request, 1
	test eax, eax
	jnz .finish
	or byte [entry.raw+11], 20h
	fastcall f_store, rbx, addr entry
.finish:
	fastcall f_finish, rbx, eax
	test eax, eax
	jnz .done
	mov rax, qword [rbx + FatIdentity.generation]
	mov qword [entry.generation], rax
	lea rsi, [entry]
	mov rdi, r12
	mov ecx, sizeof.FatEntry
	rep movsb
	mov eax, dword [request.done]
	mov dword [r13 + FatTransfer.done], eax
.ok:
	xor eax, eax
.done:
	ret
.range:
	mov eax, F_RANGE
	ret
.ro:
	mov eax, F_READONLY
	ret
endp

; Caller supplies packed timestamps. Structural attributes cannot be changed
; by set_info; name, first cluster, and size have dedicated operations.
proc fat_set_info uses rbx rsi rdi r12 r13
	locals
		entry FatEntry
	endl
body:
	mov rbx, rcx
	mov r12, rdx
	mov r13, r8
	fastcall f_validate, rbx, r12
	test eax, eax
	jnz .done
	mov al, byte [r13 + FatStamp.attributes]
	xor al, [r12 + FatEntry.raw+11]
	test al, 0D8h
	jnz .argument
	cmp byte [r13 + FatStamp.create_tenth], 199
	ja .argument
	mov rsi, r12
	lea rdi, [entry]
	mov ecx, sizeof.FatEntry
	rep movsb
	mov ax, word [r13 + FatStamp.create_time]
	mov word [entry.raw+14], ax
	mov ax, word [r13 + FatStamp.create_date]
	mov word [entry.raw+16], ax
	mov ax, word [r13 + FatStamp.access_date]
	mov word [entry.raw+18], ax
	mov ax, word [r13 + FatStamp.write_time]
	mov word [entry.raw+22], ax
	mov ax, word [r13 + FatStamp.write_date]
	mov word [entry.raw+24], ax
	mov al, byte [r13 + FatStamp.create_tenth]
	mov byte [entry.raw+13], al
	mov al, byte [r13 + FatStamp.attributes]
	mov byte [entry.raw+11], al
	fastcall f_begin, rbx
	test eax, eax
	jnz .done
	fastcall f_store, rbx, addr entry
	fastcall f_finish, rbx, eax
	test eax, eax
	jnz .done
	mov rax, qword [rbx + FatIdentity.generation]
	mov qword [entry.generation], rax
	lea rsi, [entry]
	mov rdi, r12
	mov ecx, sizeof.FatEntry
	rep movsb
	xor eax, eax
.done:
	ret
.argument:
	mov eax, F_ARGUMENT
	ret
endp
