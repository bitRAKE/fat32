; File data/metadata primitives operate on a compact canonical record.
; Snapshot validation is separately selected by the legacy API wrappers.

; Cheap in-memory lifetime/location checks, no directory reread or chain scan.
; RCX=identity*, RDX=FatRecord* -> EAX=status. Preserves RCX/RDX/R8/R9.
section '.text$f_basic_record' code readable executable comdat align 16
proc f_basic_record
	mov eax, F_FORMAT
	cmp dword [rcx + FatIdentity.magic], F_MAGIC
	jne .done
	mov r10, qword [rcx + FatIdentity.generation]
	mov eax, F_STALE
	cmp r10, [rdx + FatRecord.generation]
	jne .done
	mov eax, F_CORRUPT
	mov r10d, dword [rdx + FatRecord.parent]
	sub r10d, 2
	cmp r10d, [rcx + FatIdentity.cluster_count]
	jae .done
	mov r10, qword [rdx + FatRecord.sector]
	mov r11d, dword [rcx + FatIdentity.data_start]
	cmp r10, r11
	jb .done
	mov r11d, dword [rcx + FatIdentity.total_sectors]
	cmp r10, r11
	jae .done
	mov r10d, dword [rdx + FatRecord.offset]
	test r10d, 31
	jnz .done
	cmp r10d, [rcx + FatIdentity.sector_bytes]
	jae .done
	cmp byte [rdx + FatRecord.raw], 0
	je .done
	cmp byte [rdx + FatRecord.raw], 0E5h
	je .done
	test byte [rdx + FatRecord.raw+11], 8
	jnz .done
	xor eax, eax
.done:
	ret
endp

; Ordinary file plus a usable first cluster when data exists. No full audit.
; RCX=identity*, RDX=record* -> EAX=status; RCX/RDX/R8/R9 preserved.
section '.text$f_basic_file' code readable executable comdat align 16
proc f_basic_file
	fastcall f_basic_record, rcx, rdx
	test eax, eax
	jnz .done
	mov eax, F_CORRUPT
	test byte [rdx + FatRecord.raw+11], 10h
	jnz .done
	mov r10d, dword [rdx + FatRecord.cluster]
	test r10d, r10d
	jnz .cluster
	cmp dword [rdx + FatRecord.size], 0
	jne .done
	jmp .ok
.cluster:
	sub r10d, 2
	cmp r10d, [rcx + FatIdentity.cluster_count]
	jae .done
.ok:
	xor eax, eax
.done:
	ret
endp

; Verify snapshot, slot membership, and raw bytes before touching a file.
; RCX=identity*, RDX=const FatEntry* -> EAX=status; no entry modification.
; Check generation, parent/slot bounds, raw SFN and cluster/size agreement.
; R10 is a cache pointer used only after the final call, so needs no save.
section '.text$f_validate' code readable executable comdat align 16
proc f_validate uses rbx rsi, location:8, offset:4
	assert offset+4-(parmbase@proc) <= 32
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
	add r10, qword [rbx + FatIdentity.directory]
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
section '.text$f_file' code readable executable comdat align 16
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

; Walk only as far as the requested index, bounded by volume cluster count.
; RCX=identity*, EDX=first cluster, R8D=zero-based index, R9=uint32_t* out.
; EAX=status. Validate each encountered ID; output unchanged on error. No full
; chain audit. Index 0 returns the supplied first cluster after checking its ID.
section '.text$f_nth' code readable executable comdat align 16
proc f_nth uses rbx rsi rdi rbp
	locals
		next dd ?
	endl
body:
	mov rbx, rcx
	mov esi, edx
	mov edi, r8d
	mov rbp, r9
	cmp edi, [rbx + FatIdentity.cluster_count]
	jae .bad
.loop:
	lea eax, [rsi-2]
	cmp eax, [rbx + FatIdentity.cluster_count]
	jae .bad
	test edi, edi
	jz .found
	fastcall fat_get, rbx, esi, addr next
	test eax, eax
	jnz .done
	mov esi, dword [next]
	dec edi
	jmp .loop
.found:
	mov dword [rbp], esi
	xor eax, eax
.done:
	ret
.bad:
	mov eax, F_CORRUPT
	jmp .done
endp

; Read a range already clamped to file size, checking encountered links locally.
; RCX=identity*, EDX=first cluster, R8=FatTransfer* in-out; EAX=status.
; Separate from mutation transfers so read-only consumers do not retain writes.
; done counts the completed prefix even when a provider read fails.
section '.text$f_read_bytes' code readable executable comdat align 16
proc f_read_bytes uses rbx rsi rdi rbp r13, cluster:4, inside:4, remaining:4
	assert remaining+4-(parmbase@proc) <= 32
body:
	mov rbx, rcx
	mov rbp, r8
	mov dword [rbp + FatTransfer.done], 0
	cmp dword [rbp + FatTransfer.length], 0
	je .ok
	mov esi, edx
	mov rax, qword [rbp + FatTransfer.offset]
	xor edx, edx
	div dword [rbx + FatIdentity.cluster_bytes]
	mov dword [inside], edx
	mov ecx, dword [rbx + FatIdentity.cluster_count]
	sub ecx, eax
	mov dword [remaining], ecx
	fastcall f_nth, rbx, esi, eax, addr cluster
	test eax, eax
	jnz .done
.loop:
	mov eax, dword [cluster]
	sub eax, 2
	cmp eax, [rbx + FatIdentity.cluster_count]
	jae .bad
	mov eax, dword [inside]
	xor edx, edx
	div dword [rbx + FatIdentity.sector_bytes]
	mov r13d, edx
	mov esi, eax
	fastcall f_cluster_sector, rbx, [cluster]
	add rax, rsi
	fastcall f_read_sector, rcx, rax, [rcx + FatIdentity.scratch]
	test eax, eax
	jnz .done
	mov eax, dword [rbp + FatTransfer.length]
	sub eax, [rbp + FatTransfer.done]
	mov ecx, dword [rbx + FatIdentity.sector_bytes]
	sub ecx, r13d
	cmp ecx, eax
	cmova ecx, eax
	mov eax, ecx          ; REP MOVSB preserves this completed span
	mov edi, dword [rbp + FatTransfer.done]
	add rdi, [rbp + FatTransfer.data]
	mov rsi, qword [rbx + FatIdentity.scratch]
	add rsi, r13
	rep movsb
	add [rbp + FatTransfer.done], eax
	add [inside], eax
	mov eax, dword [rbp + FatTransfer.done]
	cmp eax, [rbp + FatTransfer.length]
	je .ok
	mov eax, dword [inside]
	cmp eax, [rbx + FatIdentity.cluster_bytes]
	jb .loop
	mov dword [inside], 0
	dec dword [remaining]
	jz .bad
	fastcall fat_get, rbx, [cluster], addr cluster
	test eax, eax
	jnz .done
	jmp .loop
.ok:
	xor eax, eax
.done:
	ret
.bad:
	mov eax, F_CORRUPT
	jmp .done
endp

; Modify a bounded byte range in a prevalidated, sufficiently long chain.
; mode 1 writes, 2 zeroes. Always uses a complete sector buffer.
; RCX=identity*, EDX=first cluster, R8=FatTransfer* in-out, R9D=mode.
; Modes: 1 write, 2 zero (data unused). EAX=status; done is reset then
; counts successfully transferred sectors/spans, including a prefix on error.
; Caller validates the full range/chain; modes 1/2 require an active transaction.
; Destroys scratch; performs partial-sector read/modify/write when editing.
section '.text$f_write_bytes' code readable executable comdat align 16
proc f_write_bytes uses rbx rsi rdi rbp r13, cluster:4, inside:4, mode:4, span:4, lba:8, remaining:4
	assert remaining+4-(parmbase@proc) <= 32
body:
	mov rbx, rcx
	mov rbp, r8
	mov dword [mode], r9d
	mov dword [rbp + FatTransfer.done], 0
	cmp dword [rbp + FatTransfer.length], 0
	je .ok
	mov esi, edx
	mov rax, qword [rbp + FatTransfer.offset]
	xor edx, edx
	mov ecx, dword [rbx + FatIdentity.cluster_bytes]
	div rcx
	mov dword [inside], edx
	mov ecx, dword [rbx + FatIdentity.cluster_count]
	sub ecx, eax
	mov dword [remaining], ecx
	fastcall f_nth, rbx, esi, eax, addr cluster
	test eax, eax
	jnz .done
.loop:
	mov eax, dword [cluster]
	sub eax, 2
	cmp eax, [rbx + FatIdentity.cluster_count]
	jae .bad
	mov eax, dword [inside]
	xor edx, edx
	div dword [rbx + FatIdentity.sector_bytes]
	mov r13d, edx           ; offset inside sector
	mov esi, eax           ; sector inside cluster
	fastcall f_cluster_sector, rbx, [cluster]
	add rax, rsi
	mov qword [lba], rax
	fastcall f_read_sector, rcx, rax, [rcx + FatIdentity.scratch]
	test eax, eax
	jnz .done
	mov eax, dword [rbp + FatTransfer.length]
	sub eax, [rbp + FatTransfer.done]
	mov ecx, dword [rbx + FatIdentity.sector_bytes]
	sub ecx, r13d
	cmp ecx, eax
	cmova ecx, eax
	mov dword [span], ecx
	mov esi, dword [rbp + FatTransfer.done]
	add rsi, [rbp + FatTransfer.data]
	mov rdi, qword [rbx + FatIdentity.scratch]
	add rdi, r13
	cmp dword [mode], 2
	je .zero
	rep movsb
	jmp .write
.zero:
	xor eax, eax
	rep stosb
.write:
	fastcall f_write_sector, rbx, [lba], [rbx + FatIdentity.scratch]
	test eax, eax
	jnz .done
.advance:
	mov eax, dword [span]
	add [rbp + FatTransfer.done], eax
	add [inside], eax
	mov eax, dword [rbp + FatTransfer.done]
	cmp eax, [rbp + FatTransfer.length]
	je .ok
	mov eax, dword [inside]
	cmp eax, [rbx + FatIdentity.cluster_bytes]
	jb .loop
	mov dword [inside], 0
	dec dword [remaining]
	jz .bad
	fastcall fat_get, rbx, [cluster], addr cluster
	test eax, eax
	jnz .done
	jmp .loop
.ok:
	xor eax, eax
.done:
	ret
.bad:
	mov eax, F_CORRUPT
	jmp .done
endp

; RCX=identity*, RDX=const FatEntry* file, R8=FatTransfer* in-out.
; EAX=status; done reset/clamped to EOF, partial prefix reported on read failure.
; Neither entry nor offset/length changes. See fat32.inc for buffer/alias rules.
section '.text$f_read_record' code readable executable comdat align 16
proc f_read_record uses rdi
	home_struct request, FatTransfer, 0
body:
	mov rdi, r8
	mov dword [r8 + FatTransfer.done], 0
	fastcall f_basic_file, rcx, rdx
	test eax, eax
	jnz .done
	; f_basic_file preserves the input registers; only the transfer survives I/O.
	mov eax, dword [rdx + FatEntry.size]
	mov r9, qword [r8 + FatTransfer.offset]
	cmp r9, rax
	jae .ok
	sub eax, r9d
	cmp eax, [r8 + FatTransfer.length]
	cmova eax, [r8 + FatTransfer.length]
	mov dword [request.length], eax
	mov qword [request.offset], r9
	mov rax, qword [r8 + FatTransfer.data]
	mov qword [request.data], rax
	fastcall f_read_bytes, rcx, [rdx + FatEntry.cluster], addr request
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
section '.text$f_store' code readable executable comdat align 16
proc f_store uses rbx rsi
	mov rbx, rcx
	mov rsi, rdx
	fastcall f_directory, rcx, [rdx + FatEntry.sector]
	test eax, eax
	jnz .done
	mov r9d, dword [rsi + FatEntry.offset]
	add r9, qword [rbx + FatIdentity.directory]
	iterate n, 0,8,16,24
		mov rax, qword [rsi + FatEntry.raw+n]
		mov qword [r9+n], rax
	end iterate
	fastcall f_write_sector, rbx, [rsi + FatEntry.sector], [rbx + FatIdentity.directory]
.done:
	ret
endp

; Resize within an existing transaction. Entry is a private work copy.
; RCX=identity*, RDX=private FatEntry* in-out, R8D=new uint32 byte size,
; R9D=defer SFN publication (only for a following growth write).
; EAX=status. Caller owns active transaction and validated file. Update chain,
; zero exposed bytes and stage SFN; failure may alter work copy/staged sectors.
; Caller must roll back and never publish the work copy on failure.
section '.text$f_resize' code readable executable comdat align 16
proc f_resize uses rbx rsi rdi r12 r13 rbp, defer:4
	locals
		chain dq ?
		new_cluster dd ?
		tail dd ?
		new_first dd ?
		shrinking dd ?
	endl
	assert defer+4-(parmbase@proc) <= 32
	home_struct request, FatTransfer, 8
	assert defer+4 <= request
body:
	mov rbx, rcx
	mov rsi, rdx
	mov r12d, r8d
	mov [defer], r9d
	mov dword [shrinking], 0
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
	mov ebp, dword [chain+4]
	cmp edi, r13d
	jbe .prepare_growth
	; A growth write may consume existing excess allocation; keep that tail
	; until a separate resize deliberately trims it.
	cmp dword [defer], 0
	je .shrink
	jmp .size
.prepare_growth:
	mov dword [new_first], 0
	xor ebp, ebp
.grow:
	cmp edi, r13d
	jae .attach
	fastcall f_allocate, rbx, addr new_cluster
	test eax, eax
	jnz .done
	test ebp, ebp
	jz .first
	fastcall fat_put, rbx, ebp, [new_cluster]
	test eax, eax
	jnz .done
	jmp .linked
.first:
	mov eax, dword [new_cluster]
	mov dword [new_first], eax
.linked:
	mov ebp, dword [new_cluster]
	inc edi
	jmp .grow
.attach:
	cmp dword [new_first], 0
	je .size
	fastcall f_order, rbx, FO_PREPARED
	test eax, eax
	jnz .done
	cmp dword [chain+4], 0
	je .attach_first
	fastcall fat_put, rbx, [chain+4], [new_first]
	test eax, eax
	jnz .done
	jmp .size
.attach_first:
	mov eax, [new_first]
	mov [rsi + FatEntry.cluster], eax
	jmp .size
.shrink:
	mov dword [shrinking], 1
	test r13d, r13d
	jz .all
	lea r8d, [r13-1]
	fastcall f_nth, rbx, [rsi + FatEntry.cluster], r8d, addr new_cluster
	test eax, eax
	jnz .done
	fastcall fat_get, rbx, [new_cluster], addr tail
	test eax, eax
	jnz .done
	jmp .size
.all:
	mov eax, [rsi + FatEntry.cluster]
	mov [tail], eax
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
	fastcall f_write_bytes, rbx, [rsi + FatEntry.cluster], addr request, 2
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
	cmp dword [defer], 0
	jne .ok
	fastcall f_order, rbx, FO_PUBLISH
	test eax, eax
	jnz .done
	fastcall f_store, rbx, rsi
	test eax, eax
	jnz .done
	cmp dword [shrinking], 0
	je .done
	fastcall f_order, rbx, FO_UNPUBLISH
	test eax, eax
	jnz .done
	test r13d, r13d
	jz .free
	fastcall fat_put, rbx, [new_cluster], F_LAST
	test eax, eax
	jnz .done
	fastcall f_order, rbx, FO_DETACH
	test eax, eax
	jnz .done
.free:
	fastcall f_free, rbx, [tail]
.done:
	ret
.ok:
	xor eax, eax
	jmp .done
endp

; RCX=identity*, RDX=FatEntry* file in-out, R8=uint64 new byte size.
; EAX=status. Reject >0xFFFFFFFF/readonly; transaction updates a private copy.
; Publish refreshed entry only after acceptance. Equal size still transacts.
section '.text$f_resize_record' code readable executable comdat align 16
proc f_resize_record uses rbx rsi rdi rbp r13
	locals
		entry FatRecord
	endl
body:
	mov rbx, rcx
	mov rbp, rdx
	mov r13, r8
	mov eax, F_RANGE
	shr r8, 32
	jnz .done
	fastcall f_basic_file, rcx, rdx
	test eax, eax
	jnz .done
	mov eax, F_READONLY
	test byte [rbp + FatEntry.raw+11], 1
	jnz .done
	mov rsi, rbp
	lea rdi, [entry]
	mov ecx, sizeof.FatRecord
	rep movsb
	fastcall f_begin, rbx
	test eax, eax
	jnz .done
	fastcall f_resize, rbx, addr entry, r13d, 0
	fastcall f_finish, rbx, eax
	test eax, eax
	jnz .done
	mov rax, qword [rbx + FatIdentity.generation]
	mov qword [entry.generation], rax
	lea rsi, [entry]
	mov rdi, rbp
	mov ecx, sizeof.FatRecord
	rep movsb
	xor eax, eax
.done:
	ret
endp

; RCX=identity*, RDX=FatEntry* file in-out, R8=FatTransfer* in-out.
; EAX=status. Nonzero writes are atomic in staging; publish entry/done only after
; acceptance. Failure leaves entry unchanged, done=0; zero length validates only.
section '.text$f_write_record' code readable executable comdat align 16
proc f_write_record uses rbx rsi rdi r12 rbp r14
	locals
		entry FatRecord
	endl
	home_struct request, FatTransfer, 0
body:
	mov rbx, rcx
	mov r12, rdx
	mov rbp, r8
	mov dword [rbp + FatTransfer.done], 0
	mov r14, qword [rbp + FatTransfer.offset]
	mov eax, dword [rbp + FatTransfer.length]
	add r14, rax
	jc .range
	mov rax, r14
	shr rax, 32
	jnz .range
	fastcall f_basic_file, rcx, rdx
	test eax, eax
	jnz .done
	mov eax, F_READONLY
	test byte [r12 + FatEntry.raw+11], 1
	jnz .done
	cmp dword [rbp + FatTransfer.length], 0
	je .ok
	mov rsi, r12
	lea rdi, [entry]
	mov ecx, sizeof.FatRecord
	rep movsb
	mov rsi, rbp
	lea rdi, [request]
	mov ecx, sizeof.FatTransfer
	rep movsb
	fastcall f_begin, rbx
	test eax, eax
	jnz .done
	cmp r14d, [entry.size]
	jbe .write
	fastcall f_resize, rbx, addr entry, r14d, 1
	test eax, eax
	jnz .finish
.write:
	fastcall f_write_bytes, rbx, [entry.cluster], addr request, 1
	test eax, eax
	jnz .finish
	or byte [entry.raw+11], 20h
	fastcall f_order, rbx, FO_PUBLISH
	test eax, eax
	jnz .finish
	fastcall f_store, rbx, addr entry
.finish:
	fastcall f_finish, rbx, eax
	test eax, eax
	jnz .done
	mov rax, qword [rbx + FatIdentity.generation]
	mov qword [entry.generation], rax
	lea rsi, [entry]
	mov rdi, r12
	mov ecx, sizeof.FatRecord
	rep movsb
	mov eax, dword [request.done]
	mov dword [rbp + FatTransfer.done], eax
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
section '.text$f_set_record' code readable executable comdat align 16
proc f_set_record uses rbx rsi rdi r12 rbp
	locals
		entry FatRecord
	endl
body:
	mov rbx, rcx
	mov r12, rdx
	mov rbp, r8
	fastcall f_basic_record, rcx, rdx
	test eax, eax
	jnz .done
	mov dl, byte [rbp + FatStamp.attributes]
	xor dl, [r12 + FatEntry.raw+11]
	mov eax, F_ARGUMENT
	test dl, 0D8h
	jnz .done
	cmp byte [rbp + FatStamp.create_tenth], 199
	ja .done
	mov rsi, r12
	lea rdi, [entry]
	mov ecx, sizeof.FatRecord
	rep movsb
	mov ax, word [rbp + FatStamp.create_time]
	mov word [entry.raw+14], ax
	mov ax, word [rbp + FatStamp.create_date]
	mov word [entry.raw+16], ax
	mov ax, word [rbp + FatStamp.access_date]
	mov word [entry.raw+18], ax
	mov ax, word [rbp + FatStamp.write_time]
	mov word [entry.raw+22], ax
	mov ax, word [rbp + FatStamp.write_date]
	mov word [entry.raw+24], ax
	mov al, byte [rbp + FatStamp.create_tenth]
	mov byte [entry.raw+13], al
	mov al, byte [rbp + FatStamp.attributes]
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
	mov ecx, sizeof.FatRecord
	rep movsb
	xor eax, eax
.done:
	ret
endp
