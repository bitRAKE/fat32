; Creation, directory growth, and removal live beside the file routines;
; the sector layer never sees a name, cluster number, or directory entry.

; Validate a NUL-terminated component; EDX receives length on success.
; RCX=const NUL-terminated UTF-16 component -> EAX=status, EDX=length on F_OK.
; Private leaf; rejects malformed pairs, forbidden chars, empty/overlong names,
; trailing dot/space. R9/flags clobbered; length unspecified on error.
section '.text$f_name_check' code readable executable comdat align 16
proc f_name_check
	xor edx, edx
	xor r9d, r9d            ; pending high surrogate
.loop:
	movzx eax, word [rcx+rdx*2]
	test eax, eax
	jz .end
	cmp edx, 255
	jae .bad
	cmp eax, 32
	jb .bad
	cmp eax, 0FFFFh
	je .bad
	iterate ch, '"','*','/',':','<','>','?','\','|'
		cmp eax, ch
		je .bad
	end iterate
	test r9d, r9d
	jz .surrogate
	cmp eax, 0DC00h
	jb .bad
	cmp eax, 0DFFFh
	ja .bad
	xor r9d, r9d
	jmp .next
.surrogate:
	cmp eax, 0D800h
	jb .next
	cmp eax, 0DFFFh
	ja .next
	cmp eax, 0DBFFh
	ja .bad
	mov r9d, 1
.next:
	inc edx
	jmp .loop
.end:
	test edx, edx
	jz .bad
	test r9d, r9d
	jnz .bad
	movzx eax, word [rcx+rdx*2-2]
	cmp eax, '.'
	je .bad
	cmp eax, ' '
	je .bad
	xor eax, eax
	ret
.bad:
	mov eax, F_NAME
	ret
endp

; Find a unique legal SFN alias, checking raw aliases AND visible LFN names.
; F0000001 .. FFFFFFFF: no locale-dependent lossy Unicode conversion.
; RCX=identity*, EDX=parent first cluster, R8=11-byte writable SFN buffer.
; EAX=status. Search candidate aliases against raw SFN and decoded LFN names;
; publish only on success. Caller has mounted/readable parent; no I/O writes.
section '.text$f_alias' code readable executable comdat align 16
proc f_alias uses rbx rsi rdi rbp r13
	locals
		cursor FatCursor
		found FatEntry
		alias FatEntry
	endl
body:
	mov rbx, rcx
	mov ebp, edx
	mov r13, r8
	mov esi, 1
.candidate:
	lea rdi, [alias.raw]
	mov ecx, 11
	mov al, ' '
	rep stosb
	mov byte [alias.raw], 'F'
	mov byte [alias.raw+12], 0
	mov eax, esi
	mov ecx, 7
.hex:
	mov edx, eax
	and edx, 15
	add edx, '0'
	cmp edx, '9'
	jbe .digit
	add edx, 7
.digit:
	mov byte [alias.raw+rcx], dl
	shr eax, 4
	dec ecx
	jnz .hex
	fastcall f_short_name, rbx, addr alias
	fastcall fat_dir_open, rbx, ebp, addr cursor
	test eax, eax
	jnz .done
.scan:
	fastcall fat_dir_next, rbx, addr cursor, addr found
	cmp eax, F_END
	je .free
	test eax, eax
	jnz .done
	lea rcx, [alias.raw]
	lea rdx, [found.raw]
	xor eax, eax
.compare:
	mov r8b, byte [rcx+rax]
	cmp r8b, [rdx+rax]
	jne .long
	inc eax
	cmp eax, 11
	jb .compare
	jmp .collision
.long:
	fastcall f_name_equal, addr alias.name, addr found.name
	test eax, eax
	jz .scan
.collision:
	inc esi
	cmp esi, 0FFFFFFFh
	jbe .candidate
	mov eax, F_NOSPACE
	jmp .done
.free:
	lea rsi, [alias.raw]
	mov rdi, r13
	mov ecx, 11
	rep movsb
	xor eax, eax
.done:
	ret
endp

; Find a contiguous logical slot run, extending a full directory as needed.
; Return {start qword, consumed_end dword}; creation prepares a new end marker
; before publishing the name if a following allocated slot exists.
; RCX=identity*, EDX=parent cluster, R8D=needed slots (>0),
; R9=12-byte {uint64 start; uint32 consumed_end;}* out -> EAX=status.
; Active transaction required; may append zeroed clusters. Output may be partial
; on failure. The caller must write the name set/end marker and finish/rollback.
section '.text$f_slots' code readable executable comdat align 16
proc f_slots uses rbx rsi r12 rbp
	locals
		cursor FatCursor
		allocated dd ?
		past_end dd ?
	endl
body:
	mov rbx, rcx
	mov r12d, r8d
	mov rbp, r9
	xor esi, esi
	mov dword [past_end], 0
	fastcall fat_dir_open, rcx, edx, addr cursor
	test eax, eax
	jnz .done
.scan:
	fastcall f_cursor_sector, rbx, addr cursor
	cmp eax, F_END
	je .extend
	test eax, eax
	jnz .done
	mov eax, dword [cursor.offset]
	mov r10, qword [rbx + FatIdentity.directory]
	movzx eax, byte [r10+rax]
	cmp dword [past_end], 0
	jne .free
	test eax, eax
	jz .endmarker
	cmp eax, 0E5h
	je .free
	xor esi, esi
	jmp .advance
.endmarker:
	mov dword [past_end], 1
.free:
	test esi, esi
	jnz .count
	mov rax, qword [cursor.index]
	mov qword [rbp], rax
.count:
	inc esi
	cmp esi, r12d
	je .found
.advance:
	add dword [cursor.offset], 32
	inc qword [cursor.index]
	jmp .scan
.extend:
	fastcall f_allocate, rbx, addr allocated
	test eax, eax
	jnz .done
	fastcall f_order, rbx, FO_PREPARED
	test eax, eax
	jnz .done
	fastcall fat_put, rbx, [cursor.cluster], [allocated]
	test eax, eax
	jnz .done
	mov eax, dword [allocated]
	mov dword [cursor.cluster], eax
	mov dword [cursor.sector_in], 0
	mov dword [cursor.offset], 0
	mov dword [cursor.ended], 0
	mov dword [past_end], 1
	jmp .scan
.found:
	mov eax, dword [past_end]
	mov dword [rbp+8], eax
	xor eax, eax
.done:
	ret
endp

; Create file or empty directory with a UTF-16 long name. Every new name has
; a collision-checked short alias. Default timestamps are zero (unspecified).
; RCX=identity*, EDX=parent, R8=const FatCreate*, R9=FatEntry* out;
; fifth stack argument [ignore]=old SFN index+1 for rename, 0 for normal create.
; EAX=status. Requires active transaction; does not begin/end it. Output changes
; only on F_OK, but generation is provisional until wrapper accepts the operation.
; First four formal names exist to place the fifth argument at its Win64 offset.
section '.text$f_create' code readable executable comdat align 16
proc f_create uses rbx rsi rdi rbp r13 r14 r15, id_arg, parent_arg, request_arg, out_arg, ignore
	locals
		entry FatEntry
		first dq ?
		consumed_end dd ?
		location dq ?
		offset dd ?
		allocated dd ?
		checksum dd ?
		chain dq ?
	endl
body:
	mov rbx, rcx
	mov ebp, edx
	mov r13, r8
	mov r14, r9
	mov eax, F_ARGUMENT
	cmp dword [r13 + FatCreate.directory], 1
	ja .done
	fastcall f_name_check, [r13 + FatCreate.name]
	test eax, eax
	jnz .done
	mov r15d, edx
	fastcall fat_lookup, rbx, ebp, [r13 + FatCreate.name], addr entry
	test eax, eax
	jnz .lookup_result
	mov rax, qword [entry.index]
	inc rax
	cmp rax, qword [ignore]
	jne .exists
	jmp .prepare
.lookup_result:
	cmp eax, F_NOTFOUND
	jne .done
.prepare:
	lea rdi, [entry]
	xor eax, eax
	mov ecx, sizeof.FatEntry
	rep stosb
	mov dword [entry.parent], ebp
	mov dword [entry.name_length], r15d
	mov rsi, qword [r13 + FatCreate.name]
	lea rdi, [entry.name]
	lea ecx, [r15d+1]
	rep movsw
	mov byte [entry.raw+11], 20h
	cmp dword [r13 + FatCreate.directory], 0
	je .alias
	mov byte [entry.raw+11], 10h
.alias:
	fastcall f_alias, rbx, ebp, addr entry.raw
	test eax, eax
	jnz .done
	fastcall f_checksum, addr entry.raw
	mov dword [checksum], eax
	lea eax, [r15d+12]
	xor edx, edx
	mov ecx, 13
	div ecx
	mov dword [entry.lfn_count], eax
	cmp dword [r13 + FatCreate.directory], 0
	je .slots
	fastcall f_allocate, rbx, addr allocated
	test eax, eax
	jnz .finish
	mov eax, dword [allocated]
	mov dword [entry.cluster], eax
	mov word [entry.raw+26], ax
	shr eax, 16
	mov word [entry.raw+20], ax
	; f_allocate left scratch zero-filled. Construct dot and dot-dot.
	mov rax, 202020202020202Eh
	mov r10, qword [rbx + FatIdentity.scratch]
	mov qword [r10], rax
	mov qword [r10+32], rax
	mov byte [r10+33], '.'
	mov word [r10+8], 2020h
	mov byte [r10+10], 20h
	mov word [r10+40], 2020h
	mov byte [r10+42], 20h
	mov byte [r10+11], 10h
	mov byte [r10+43], 10h
	mov eax, dword [allocated]
	mov word [r10+26], ax
	shr eax, 16
	mov word [r10+20], ax
	mov eax, ebp
	cmp eax, [rbx + FatIdentity.root_cluster]
	jne .parent
	xor eax, eax            ; FAT32 root's dot-dot encoding is zero
.parent:
	mov word [r10+58], ax
	shr eax, 16
	mov word [r10+52], ax
	fastcall f_cluster_sector, rbx, [allocated]
	fastcall f_write_sector, rcx, rax, [rcx + FatIdentity.scratch]
	test eax, eax
	jnz .finish
.slots:
	mov r8d, dword [entry.lfn_count]
	inc r8d
	fastcall f_slots, rbx, ebp, r8d, addr first
	test eax, eax
	jnz .finish
	mov eax, dword [entry.lfn_count]
	add rax, [first]
	mov qword [entry.index], rax
	cmp dword [consumed_end], 0
	je .prepared
	; Bytes after an end marker are unspecified. Install the new terminator
	; and neutralize occupied-looking slots while preserving all old zero
	; markers. This phase cannot expose stale entries when the name is published.
	fastcall fat_chain, rbx, ebp, addr chain
	test eax, eax
	jnz .finish
	mov eax, dword [chain]
	mov ecx, dword [rbx + FatIdentity.cluster_bytes]
	imul rax, rcx
	shr rax, 5
	mov r8, qword [entry.index]
	inc r8
	cmp r8, rax
	jae .neutralize
	fastcall f_slot, rbx, ebp, r8, addr location
	test eax, eax
	jnz .finish
	fastcall f_directory, rbx, [location]
	test eax, eax
	jnz .finish
	mov edi, dword [offset]
	mov r10, [rbx + FatIdentity.directory]
	mov byte [r10+rdi], 0
	fastcall f_write_sector, rbx, [location], [rbx + FatIdentity.directory]
	test eax, eax
	jnz .finish
.neutralize:
	mov rsi, [first]
.neutral_slot:
	fastcall f_slot, rbx, ebp, rsi, addr location
	test eax, eax
	jnz .finish
	fastcall f_directory, rbx, [location]
	test eax, eax
	jnz .finish
	mov edi, [offset]
	mov r10, [rbx + FatIdentity.directory]
	cmp byte [r10+rdi], 0
	je .neutral_next
	cmp byte [r10+rdi], 0E5h
	je .neutral_next
	mov byte [r10+rdi], 0E5h
	fastcall f_write_sector, rbx, [location], [rbx + FatIdentity.directory]
	test eax, eax
	jnz .finish
.neutral_next:
	inc rsi
	cmp rsi, [entry.index]
	jbe .neutral_slot
.prepared:
	fastcall f_order, rbx, FO_PREPARED
	test eax, eax
	jnz .finish
	mov esi, dword [entry.lfn_count]
.long:
	mov eax, dword [entry.lfn_count]
	sub eax, esi
	add rax, [first]
	fastcall f_slot, rbx, ebp, rax, addr location
	test eax, eax
	jnz .finish
	fastcall f_directory, rbx, [location]
	test eax, eax
	jnz .finish
	mov edi, dword [offset]
	add rdi, qword [rbx + FatIdentity.directory]
	mov rax, -1
	iterate n, 0,8,16,24
		mov qword [rdi+n], rax
	end iterate
	mov eax, esi
	cmp esi, [entry.lfn_count]
	jne .ordinal
	or eax, 40h
.ordinal:
	mov byte [rdi], al
	mov byte [rdi+11], 0Fh
	mov byte [rdi+12], 0
	mov eax, dword [checksum]
	mov byte [rdi+13], al
	mov word [rdi+26], 0
	lea eax, [esi-1]
	imul eax, 13
	xor ecx, ecx
	lea r9, [f_lfn_offsets]
.chars:
	movzx edx, byte [r9+rcx]
	cmp eax, r15d
	ja .pad
	mov r8w, word [entry.name+rax*2]
	mov word [rdi+rdx], r8w
.pad:
	inc eax
	inc ecx
	cmp ecx, 13
	jb .chars
	fastcall f_write_sector, rbx, [location], [rbx + FatIdentity.directory]
	test eax, eax
	jnz .finish
	dec esi
	jnz .long
	fastcall f_slot, rbx, ebp, [entry.index], addr location
	test eax, eax
	jnz .finish
	mov rax, qword [location]
	mov qword [entry.sector], rax
	mov eax, dword [offset]
	mov dword [entry.offset], eax
	fastcall f_order, rbx, FO_PUBLISH
	test eax, eax
	jnz .finish
	fastcall f_store, rbx, addr entry
.finish:
	test eax, eax
	jnz .done
	mov rax, qword [rbx + FatIdentity.generation]
	mov qword [entry.generation], rax
	lea rsi, [entry]
	mov rdi, r14
	mov ecx, sizeof.FatEntry
	rep movsb
	xor eax, eax
.done:
	ret
.exists:
	mov eax, F_EXISTS
	jmp .done
endp

; The public wrapper owns operation acceptance and the caller's output.
; RCX=identity*, EDX=parent cluster, R8=const FatCreate*, R9=FatEntry* out.
; EAX=status. Begin/end an isolated operation, then publish a fresh entry only
; on F_OK. Name/request/output must not overlap; see fat32.inc.
section '.text$fat_create' code readable executable comdat any align 16
proc fat_create uses rbx rsi rdi rbp r13 r14
	locals
		entry FatEntry
	endl
body:
	mov rbx, rcx
	mov ebp, edx
	mov r13, r8
	mov r14, r9
	fastcall f_begin, rcx
	test eax, eax
	jnz .done
	fastcall f_create, rbx, ebp, r13, addr entry, 0
	fastcall f_finish, rbx, eax
	fastcall f_snapshot_done, rbx, addr entry, eax
	test eax, eax
	jnz .done
	mov rax, qword [rbx + FatIdentity.generation]
	mov qword [entry.generation], rax
	lea rsi, [entry]
	mov rdi, r14
	mov ecx, sizeof.FatEntry
	rep movsb
	xor eax, eax
.done:
	ret
endp

; Delete only the name records, preserving the data chain (shared by rename).
; RCX=identity*, RDX=const existing FatEntry* -> EAX=status.
; Active transaction required. Re-enumerate parent to identify only valid LFN
; records attached to the SFN, then mark that set deleted. Preserve the chain.
; Initial setup consumes RCX/RDX directly; saved RBX/R12 are for later calls.
section '.text$f_erase' code readable executable comdat align 16
proc f_erase uses rbx rsi rbp r13
	locals
		cursor FatCursor
		entry FatEntry
		location dq ?
		offset dd ?
	endl
body:
	mov rbx, rcx
	mov rbp, rdx
	fastcall fat_dir_open, rcx, [rdx + FatEntry.parent], addr cursor
	test eax, eax
	jnz .done
.find:
	fastcall fat_dir_next, rbx, addr cursor, addr entry
	test eax, eax
	jnz .done
	mov rax, qword [entry.index]
	cmp rax, qword [rbp + FatEntry.index]
	jne .find
	mov esi, dword [entry.lfn_count]
	mov r13, qword [entry.index]
	sub r13, rsi
.erase:
	fastcall f_slot, rbx, [rbp + FatEntry.parent], r13, addr location
	test eax, eax
	jnz .done
	fastcall f_directory, rbx, [location]
	test eax, eax
	jnz .done
	mov eax, dword [offset]
	mov r10, qword [rbx + FatIdentity.directory]
	mov byte [r10+rax], 0E5h
	fastcall f_write_sector, rbx, [location], [rbx + FatIdentity.directory]
	test eax, eax
	jnz .done
	inc r13
	sub esi, 1
	jae .erase
.done:
	ret
endp

; Rename within the same directory, including case-only changes. Allocate a
; new checked name set, preserve metadata and chain, then erase the old name.
; RCX=identity*, RDX=FatEntry* in-out, R8=const UTF-16 new component.
; EAX=status. Same-parent operation; preserve chain/non-name metadata. Reject
; readonly/dot records. Publish new snapshot only after the transaction accepts.
section '.text$f_rename_record' code readable executable comdat align 16
proc f_rename_record uses rbx rsi rdi rbp r13
	locals
		entry FatEntry
		request FatCreate
	endl
body:
	mov rbx, rcx
	mov rbp, rdx
	mov r13, r8
	fastcall f_basic_record, rcx, rdx
	test eax, eax
	jnz .done
	mov eax, F_ARGUMENT
	cmp byte [rbp + FatEntry.raw], '.'
	je .done
	mov eax, F_READONLY
	test byte [rbp + FatEntry.raw+11], 1
	jnz .done
	mov qword [request.name], r13
	mov dword [request.directory], 0
	mov dword [request.reserved], 0
	fastcall f_begin, rbx
	test eax, eax
	jnz .done
	mov rax, qword [rbp + FatEntry.index]
	inc rax
	fastcall f_create, rbx, [rbp + FatEntry.parent], addr request, addr entry, rax
	test eax, eax
	jnz .finish
	lea rsi, [rbp + FatEntry.raw+11]
	lea rdi, [entry.raw+11]
	mov ecx, 21
	rep movsb
	; A fresh LFN supplies case; retain all non-name metadata, clear NT case.
	and byte [entry.raw+12], not 18h
	mov eax, dword [rbp + FatEntry.cluster]
	mov dword [entry.cluster], eax
	mov eax, dword [rbp + FatEntry.size]
	mov dword [entry.size], eax
	fastcall f_store, rbx, addr entry
	test eax, eax
	jnz .finish
	fastcall f_order, rbx, FO_PUBLISH
	test eax, eax
	jnz .finish
	fastcall f_erase, rbx, rbp
.finish:
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

; RCX=identity*, RDX=const FatEntry* -> EAX=status.
; Require ordinary file/empty directory and nonshared chain. Reject readonly,
; dot/root/self links; ignore only exact dot records while checking emptiness.
; Success invalidates the input snapshot but does not overwrite its memory.
section '.text$f_remove_record' code readable executable comdat align 16
proc f_remove_record uses rbx rsi rbp
	locals
		cursor FatCursor
		child FatEntry
		chain dq ?
	endl
body:
	mov rbx, rcx
	mov rbp, rdx
	fastcall f_basic_record, rcx, rdx
	test eax, eax
	jnz .done
	mov eax, F_READONLY
	test byte [rbp + FatEntry.raw+11], 1
	jnz .done
	cmp byte [rbp + FatEntry.raw], '.'
	je .bad
	mov eax, dword [rbp + FatEntry.cluster]
	cmp eax, [rbx + FatIdentity.root_cluster]
	je .bad
	cmp eax, [rbp + FatEntry.parent]
	je .bad
	fastcall fat_chain, rbx, eax, addr chain
	test eax, eax
	jnz .done
	test byte [rbp + FatEntry.raw+11], 10h
	jz .begin
	fastcall fat_dir_open, rbx, [rbp + FatEntry.cluster], addr cursor
	test eax, eax
	jnz .done
.children:
	fastcall fat_dir_next, rbx, addr cursor, addr child
	cmp eax, F_END
	je .begin
	test eax, eax
	jnz .done
	; Only exact dot and dot-dot records are ignorable.
	cmp byte [child.raw], '.'
	jne .notempty
	mov al, byte [child.raw+1]
	cmp al, ' '
	je .dot
	cmp al, '.'
	jne .notempty
.dot:
	lea rsi, [child.raw+2]
	mov ecx, 9
.spaces:
	cmp byte [rsi], ' '
	jne .notempty
	inc rsi
	dec ecx
	jnz .spaces
	jmp .children
.begin:
	fastcall f_begin, rbx
	test eax, eax
	jnz .done
	fastcall f_erase, rbx, rbp
	test eax, eax
	jnz .finish
	fastcall f_order, rbx, FO_UNPUBLISH
	test eax, eax
	jnz .finish
	fastcall f_free, rbx, [rbp + FatEntry.cluster]
.finish:
	fastcall f_finish, rbx, eax
.done:
	ret
.notempty:
	mov eax, F_NOTEMPTY
	jmp .done
.bad:
	mov eax, F_CORRUPT
	jmp .done
endp
