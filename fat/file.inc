; File data and metadata. Public mutations work on a private entry snapshot;
; a failed operation changes neither caller metadata nor the sector overlay.

; Verify snapshot, slot membership, and raw bytes before touching a file.
; RCX=identity*, RDX=const FatEntry* -> EAX=status; no entry modification.
; Check generation, parent/slot bounds, raw SFN and cluster/size agreement.
; R10 is a cache pointer used only after the final call, so needs no save.
proc f_validate uses rbx rsi
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
	mov r10d, dword [offset]
	add r10, rbx
	add r10, FatIdentity.directory
	iterate n, 0,8,16,24
		mov rax, qword [r10+n]
		cmp rax, qword [rsi + FatEntry.raw+n]
		jne .stale
	end iterate
	cmp byte [r10], 0
	je .bad
	cmp byte [r10], 0E5h
	je .bad
	test byte [r10+11], 8
	jnz .bad
	movzx eax, word [r10+20]
	shl eax, 16
	mov ax, word [r10+26]
	and eax, F_MASK
	cmp eax, [rsi + FatEntry.cluster]
	jne .bad
	mov eax, dword [r10+28]
	cmp eax, [rsi + FatEntry.size]
	jne .bad
	xor eax, eax
.done:
	ret
.bad:
	mov eax, F_CORRUPT
	jmp .done
.stale:
	mov eax, F_STALE
	jmp .done
endp

; Check ordinary file and full chain length, not merely the requested prefix.
; RCX=identity*, RDX=const FatEntry* -> EAX=status.
; Validate snapshot, ordinary-file attributes and full chain with at least
; ceil(size/cluster_bytes) clusters. Extra allocated clusters are permitted.
proc f_file uses rbx rsi
	locals
		chain dq ?
	endl
body:
	mov rbx, rcx
	mov rsi, rdx
	fastcall f_validate, rcx, rdx
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
	jmp .done
endp

; Walk a prevalidated chain to index. Output cluster at R9.
; RCX=identity*, EDX=first cluster, R8D=zero-based index, R9=uint32_t* out.
; EAX=status. Caller prevalidates chain and index < count. Output unchanged on
; error; no allocation/transaction. Index 0 returns the supplied first cluster.
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
	jmp .done
endp

; Transfer a bounded byte range in a prevalidated, sufficiently long chain.
; mode 0 reads, 1 writes, 2 zeroes. Always uses a complete sector buffer.
; RCX=identity*, EDX=first cluster, R8=FatTransfer* in-out, R9D=mode.
; Modes: 0 read, 1 write, 2 zero (data unused). EAX=status; done is reset then
; counts successfully transferred sectors/spans, including a prefix on error.
; Caller validates the full range/chain; modes 1/2 require an active transaction.
; Destroys scratch; performs partial-sector read/modify/write when editing.
proc f_bytes uses rbx rsi rdi r12 r13 r14
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
	fastcall f_read_sector, rcx, rax, addr rcx + FatIdentity.scratch
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

; RCX=identity*, RDX=const FatEntry* file, R8=FatTransfer* in-out.
; EAX=status; done reset/clamped to EOF, partial prefix reported on read failure.
; Neither entry nor offset/length changes. See fat32.inc for buffer/alias rules.
proc fat_read uses rbx rsi rdi
	locals
		request FatTransfer
	endl
body:
	mov rbx, rcx
	mov rsi, rdx
	mov rdi, r8
	mov dword [rdi + FatTransfer.done], 0
	fastcall f_file, rcx, rdx
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
	jmp .done
endp

; Write the raw SFN to its slot without disturbing neighboring entries/LFNs.
; RCX=identity*, RDX=const validated FatEntry* -> EAX=status.
; Active transaction required. Replace only its 32-byte SFN in the cached sector
; and stage it. Does not refresh generation; input entry stays unchanged.
proc f_store uses rbx rsi
	mov rbx, rcx
	mov rsi, rdx
	fastcall f_directory, rcx, [rdx + FatEntry.sector]
	test eax, eax
	jnz .done
	mov r9d, dword [rsi + FatEntry.offset]
	lea r9, [rbx + FatIdentity.directory+r9]
	iterate n, 0,8,16,24
		mov rax, qword [rsi + FatEntry.raw+n]
		mov qword [r9+n], rax
	end iterate
	fastcall f_write_sector, rbx, [rsi + FatEntry.sector], addr rbx + FatIdentity.directory
.done:
	ret
endp

; Resize within an existing transaction. Entry is a private work copy.
; RCX=identity*, RDX=private FatEntry* in-out, R8D=new uint32 byte size.
; EAX=status. Caller owns active transaction and validated file. Update chain,
; zero exposed bytes and stage SFN; failure may alter work copy/staged sectors.
; Caller must roll back and never publish the work copy on failure.
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
	fastcall fat_chain, rcx, [rdx + FatEntry.cluster], addr chain
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

; RCX=identity*, RDX=FatEntry* file in-out, R8=uint64 new byte size.
; EAX=status. Reject >0xFFFFFFFF/readonly; transaction updates a private copy.
; Publish refreshed entry only after acceptance. Equal size still transacts.
proc fat_resize uses rbx rsi rdi r12 r13
	locals
		entry FatEntry
	endl
body:
	mov rbx, rcx
	mov r12, rdx
	mov r13, r8
	mov eax, F_RANGE
	shr r8, 32
	jnz .done
	fastcall f_file, rcx, rdx
	test eax, eax
	jnz .done
	mov eax, F_READONLY
	test byte [r12 + FatEntry.raw+11], 1
	jnz .done
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
endp

; RCX=identity*, RDX=FatEntry* file in-out, R8=FatTransfer* in-out.
; EAX=status. Nonzero writes are atomic in staging; publish entry/done only after
; acceptance. Failure leaves entry unchanged, done=0; zero length validates only.
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
	fastcall f_file, rcx, rdx
	test eax, eax
	jnz .done
	mov eax, F_READONLY
	test byte [r12 + FatEntry.raw+11], 1
	jnz .done
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
	jmp .done
endp

; Caller supplies packed timestamps. Structural attributes cannot be changed
; by set_info; name, first cluster, and size have dedicated operations.
; RCX=identity*, RDX=FatEntry* in-out, R8=const FatStamp* -> EAX=status.
; Allow readonly changes, forbid structural attributes, bound create_tenth.
; Other timestamp bitfields are caller-validated. Publish only after acceptance.
proc fat_set_info uses rbx rsi rdi r12 r13
	locals
		entry FatEntry
	endl
body:
	mov rbx, rcx
	mov r12, rdx
	mov r13, r8
	fastcall f_validate, rcx, rdx
	test eax, eax
	jnz .done
	mov dl, byte [r13 + FatStamp.attributes]
	xor dl, [r12 + FatEntry.raw+11]
	mov eax, F_ARGUMENT
	test dl, 0D8h
	jnz .done
	cmp byte [r13 + FatStamp.create_tenth], 199
	ja .done
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
endp
