; Geometry, bounded FAT access, and transaction/cache coherence.
; Internal calls use the same ABI as public calls. EAX is always a status.

proc fat_invalidate
	mov qword [rcx + FatIdentity.fat_lba], -1
	mov qword [rcx + FatIdentity.dir_lba], -1
	inc qword [rcx + FatIdentity.generation]
	mov dword [rcx + FatIdentity.next_free], 2
	mov dword [rcx + FatIdentity.free_hint], -1
	xor eax, eax
	ret
endp

; mount(identity, ops, OEM_map_or_null). Failed mounts leave magic clear.
proc fat_mount uses rbx rsi rdi
	mov rbx, rcx
	mov rsi, rdx
	mov qword [rbx + FatIdentity.oem], r8
	mov dword [rbx + FatIdentity.magic], 0
	mov dword [rbx + FatIdentity.transaction], 0
	mov qword [rbx + FatIdentity.ops], rsi
	fastcall fat_invalidate, rbx
	mov eax, dword [rsi + SectorOps.sector_bytes]
	cmp eax, 512
	jb .format
	cmp eax, F_MAX_SECTOR
	ja .format
	lea edx, [eax-1]
	test eax, edx
	jnz .format
	cmp qword [rsi + SectorOps.sectors], 1
	jb .format
	cmp qword [rsi + SectorOps.read], 0
	je .format
	mov dword [rbx + FatIdentity.sector_bytes], eax
	fastcall [rsi + SectorOps.read], [rsi + SectorOps.context], 0, addr rbx + FatIdentity.boot
	test eax, eax
	jnz .done
	lea rdi, [rbx + FatIdentity.boot]
	cmp word [rdi+510], 0AA55h
	jne .format
	movzx eax, word [rdi+11]
	cmp eax, [rbx + FatIdentity.sector_bytes]
	jne .format
	movzx eax, byte [rdi+13]
	test eax, eax
	jz .format
	cmp eax, 128
	ja .format
	lea edx, [eax-1]
	test eax, edx
	jnz .format
	mov dword [rbx + FatIdentity.cluster_sectors], eax
	imul eax, [rbx + FatIdentity.sector_bytes]
	cmp eax, 65536
	ja .format
	mov dword [rbx + FatIdentity.cluster_bytes], eax
	cmp word [rdi+17], 0       ; FAT32 has no fixed root array
	jne .format
	cmp word [rdi+19], 0
	jne .format
	cmp word [rdi+22], 0
	jne .format
	cmp word [rdi+42], 0       ; supported version 0.0
	jne .format
	movzx eax, word [rdi+14]
	test eax, eax
	jz .format
	mov dword [rbx + FatIdentity.fat_start], eax
	movzx eax, byte [rdi+16]
	test eax, eax
	jz .format
	mov dword [rbx + FatIdentity.fat_count], eax
	mov edx, dword [rdi+36]
	test edx, edx
	jz .format
	mov dword [rbx + FatIdentity.fat_sectors], edx
	imul rax, rdx
	mov edx, dword [rbx + FatIdentity.fat_start]
	add rax, rdx
	mov edx, dword [rdi+32]
	cmp rax, rdx
	jae .format
	cmp rdx, [rsi + SectorOps.sectors]
	ja .format
	mov dword [rbx + FatIdentity.total_sectors], edx
	mov dword [rbx + FatIdentity.data_start], eax
	sub edx, eax
	mov eax, edx
	xor edx, edx
	div dword [rbx + FatIdentity.cluster_sectors]
	cmp eax, 65525            ; classification is by cluster count
	jb .format
	cmp eax, 0FFFFFEEh        ; keep all reserved values out of data IDs
	ja .format
	mov dword [rbx + FatIdentity.cluster_count], eax
	lea ecx, [eax+2]
	mov eax, dword [rbx + FatIdentity.fat_sectors]
	mov edx, dword [rbx + FatIdentity.sector_bytes]
	imul rax, rdx
	shr rax, 2
	cmp rax, rcx
	jb .format
	mov eax, dword [rdi+44]
	cmp eax, 2
	jb .format
	cmp eax, ecx
	jae .format
	mov dword [rbx + FatIdentity.root_cluster], eax
	mov dword [rbx + FatIdentity.mirrored], 1
	mov dword [rbx + FatIdentity.active_fat], 0
	movzx eax, word [rdi+40]
	test eax, 80h
	jz .flags
	mov dword [rbx + FatIdentity.mirrored], 0
	and eax, 15
	cmp eax, [rbx + FatIdentity.fat_count]
	jae .format
	mov dword [rbx + FatIdentity.active_fat], eax
.flags:
	movzx eax, word [rdi+48]
	mov dword [rbx + FatIdentity.fsinfo], 0
	test eax, eax
	jz .noinfo
	cmp eax, [rbx + FatIdentity.fat_start]
	jae .noinfo
	mov dword [rbx + FatIdentity.fsinfo], eax
.noinfo:
	movzx eax, word [rdi+50]
	mov dword [rbx + FatIdentity.backup], 0
	test eax, eax
	jz .nobackup
	cmp eax, [rbx + FatIdentity.fat_start]
	jae .nobackup
	mov dword [rbx + FatIdentity.backup], eax
.nobackup:
	mov eax, dword [rdi+67]
	mov dword [rbx + FatIdentity.serial], eax
	mov dword [rbx + FatIdentity.magic], F_MAGIC
	; Validate the root chain before allowing directory traversal.
	fastcall fat_chain, rbx, [rbx + FatIdentity.root_cluster], addr rbx + FatIdentity.scratch
	test eax, eax
	jnz .failed
	xor eax, eax
	ret
.format:
	mov eax, F_FORMAT
.failed:
	mov dword [rbx + FatIdentity.magic], 0
.done:
	ret
endp

proc f_read_sector uses rbx
	mov rbx, qword [rcx + FatIdentity.ops]
	cmp rdx, [rbx + SectorOps.sectors]
	jae .range
	fastcall [rbx + SectorOps.read], [rbx + SectorOps.context], rdx, r8
	ret
.range:
	mov eax, F_RANGE
	ret
endp

proc f_write_sector uses rbx
	cmp dword [rcx + FatIdentity.transaction], 1
	jne .busy
	mov eax, dword [rcx + FatIdentity.total_sectors]
	cmp rdx, rax
	jae .range
	mov qword [rcx + FatIdentity.fat_lba], -1
	mov qword [rcx + FatIdentity.dir_lba], -1
	mov rbx, qword [rcx + FatIdentity.ops]
	fastcall [rbx + SectorOps.write], [rbx + SectorOps.context], rdx, r8
	ret
.range:
	mov eax, F_RANGE
	ret
.busy:
	mov eax, F_BUSY
	ret
endp

; Cached directory sector. Pointer is identity.directory after success.
proc f_directory uses rbx rsi
	mov rbx, rcx
	mov rsi, rdx
	cmp rdx, [rbx + FatIdentity.dir_lba]
	je .hit
	mov qword [rbx + FatIdentity.dir_lba], -1
	fastcall f_read_sector, rbx, rsi, addr rbx + FatIdentity.directory
	test eax, eax
	jnz .done
	mov qword [rbx + FatIdentity.dir_lba], rsi
.hit:
	xor eax, eax
.done:
	ret
endp

; fat_get(identity, cluster, out32), including reserved FAT[0] and FAT[1].
proc fat_get uses rbx rsi rdi
	mov rbx, rcx
	mov rsi, r8
	cmp dword [rbx + FatIdentity.magic], F_MAGIC
	jne .bad
	mov eax, dword [rbx + FatIdentity.cluster_count]
	add eax, 2
	cmp edx, eax
	jae .bad
	mov eax, edx
	shl rax, 2
	xor edx, edx
	mov ecx, dword [rbx + FatIdentity.sector_bytes]
	div rcx
	mov edi, edx
	mov edx, dword [rbx + FatIdentity.active_fat]
	imul edx, [rbx + FatIdentity.fat_sectors]
	add edx, [rbx + FatIdentity.fat_start]
	add rdx, rax
	cmp rdx, [rbx + FatIdentity.fat_lba]
	je .hit
	mov qword [rbx + FatIdentity.fat_lba], rdx
	fastcall f_read_sector, rbx, rdx, addr rbx + FatIdentity.fat
	test eax, eax
	jnz .failed
.hit:
	mov eax, dword [rbx + FatIdentity.fat + rdi]
	and eax, F_MASK
	mov dword [rsi], eax
	xor eax, eax
	ret
.failed:
	mov qword [rbx + FatIdentity.fat_lba], -1
	ret
.bad:
	mov eax, F_CORRUPT
	ret
endp

; Internal setter preserves the reserved high nibble independently in every
; FAT copy. Disabled mirroring touches only the selected active FAT.
proc f_put uses rbx rsi rdi r12 r13
	locals
		part dq ?
		old_value dd ?
		cluster_id dd ?
	endl
body:
	mov rbx, rcx
	mov r12d, r8d
	mov dword [cluster_id], edx
	fastcall fat_get, rbx, edx, addr old_value
	test eax, eax
	jnz .done
	mov edx, dword [cluster_id]
	mov eax, dword [rbx + FatIdentity.cluster_count]
	add eax, 2
	cmp edx, eax
	jae .bad
	mov eax, edx
	shl rax, 2
	xor edx, edx
	mov ecx, dword [rbx + FatIdentity.sector_bytes]
	div rcx
	mov edi, edx
	mov qword [part], rax
	xor esi, esi
	mov r13d, dword [rbx + FatIdentity.fat_count]
	cmp dword [rbx + FatIdentity.mirrored], 0
	jne .copy
	mov esi, dword [rbx + FatIdentity.active_fat]
	lea r13d, [esi+1]
.copy:
	mov eax, esi
	imul eax, [rbx + FatIdentity.fat_sectors]
	add eax, [rbx + FatIdentity.fat_start]
	add rax, [part]
	fastcall f_read_sector, rbx, rax, addr rbx + FatIdentity.fat
	test eax, eax
	jnz .done
	mov eax, dword [rbx + FatIdentity.fat + rdi]
	mov ecx, eax
	and ecx, F_MASK
	cmp ecx, dword [old_value]
	jne .bad
	and eax, 0F0000000h
	or eax, r12d
	mov dword [rbx + FatIdentity.fat + rdi], eax
	mov eax, esi
	imul eax, [rbx + FatIdentity.fat_sectors]
	add eax, [rbx + FatIdentity.fat_start]
	add rax, [part]
	fastcall f_write_sector, rbx, rax, addr rbx + FatIdentity.fat
	test eax, eax
	jnz .done
	inc esi
	cmp esi, r13d
	jb .copy
	xor eax, eax
.done:
	mov qword [rbx + FatIdentity.fat_lba], -1
	ret
.bad:
	mov eax, F_CORRUPT
	ret
endp

; Map a data cluster to a relative sector. Private leaf: EDX -> RAX.
proc f_cluster_sector
	lea eax, [edx-2]
	mov edx, dword [rcx + FatIdentity.cluster_sectors]
	imul rax, rdx
	mov edx, dword [rcx + FatIdentity.data_start]
	add rax, rdx
	ret
endp

; fat_chain(identity, first, {count,last}). Brent's cycle detector bounds a
; corrupt loop without allocating a volume-sized visited bitmap.
proc fat_chain uses rbx rsi rdi r12 r13 r14 r15
	locals
		next dd ?
	endl
body:
	mov rbx, rcx
	mov rsi, r8
	mov edi, edx
	mov qword [rsi], 0
	test edi, edi
	jz .ok
	mov r12d, edi          ; checkpoint
	mov r13d, 1            ; power
	xor r14d, r14d         ; distance
	xor r15d, r15d         ; count
.loop:
	cmp edi, 2
	jb .bad
	mov eax, dword [rbx + FatIdentity.cluster_count]
	inc eax
	cmp edi, eax
	ja .bad
	inc r15d
	cmp r15d, [rbx + FatIdentity.cluster_count]
	ja .bad
	fastcall fat_get, rbx, edi, addr next
	test eax, eax
	jnz .done
	mov eax, dword [next]
	cmp eax, F_EOC
	jae .end
	cmp eax, 0FFFFFF0h
	jae .bad
	inc r14d
	cmp eax, r12d
	je .bad
	cmp r14d, r13d
	jne .advance
	mov r12d, eax
	shl r13d, 1
	xor r14d, r14d
.advance:
	mov edi, eax
	jmp .loop
.end:
	mov dword [rsi], r15d
	mov dword [rsi+4], edi
.ok:
	xor eax, eax
.done:
	ret
.bad:
	mov eax, F_CORRUPT
	ret
endp

proc f_begin uses rbx rsi
	mov rbx, rcx
	cmp dword [rbx + FatIdentity.magic], F_MAGIC
	jne .bad
	cmp dword [rbx + FatIdentity.transaction], 0
	jne .busy
	mov rsi, qword [rbx + FatIdentity.ops]
	cmp qword [rsi + SectorOps.write], 0
	je .ro
	cmp qword [rsi + SectorOps.begin], 0
	je .ro
	cmp qword [rsi + SectorOps.end], 0
	je .ro
	fastcall [rsi + SectorOps.begin], [rsi + SectorOps.context]
	test eax, eax
	jnz .done
	mov dword [rbx + FatIdentity.transaction], 1
.done:
	ret
.ro:
	mov eax, F_READONLY
	ret
.busy:
	mov eax, F_BUSY
	ret
.bad:
	mov eax, F_FORMAT
	ret
endp

; FSInfo is advisory. Never turn a stale free-count hint into ENOSPC.
; Invalidate both valid primary and backup hints after allocation changes.
proc f_info_unknown uses rbx rsi rdi
	mov rbx, rcx
	mov esi, dword [rbx + FatIdentity.fsinfo]
	test esi, esi
	jz .ok
	xor edi, edi
.copy:
	fastcall f_read_sector, rbx, rsi, addr rbx + FatIdentity.scratch
	test eax, eax
	jnz .done
	cmp dword [rbx + FatIdentity.scratch], 41615252h
	jne .next
	cmp dword [rbx + FatIdentity.scratch+484], 61417272h
	jne .next
	cmp dword [rbx + FatIdentity.scratch+508], 0AA550000h
	jne .next
	mov qword [rbx + FatIdentity.scratch+488], -1
	fastcall f_write_sector, rbx, rsi, addr rbx + FatIdentity.scratch
	test eax, eax
	jnz .done
.next:
	test edi, edi
	jnz .ok
	inc edi
	mov eax, dword [rbx + FatIdentity.backup]
	test eax, eax
	jz .ok
	add esi, eax
	cmp esi, [rbx + FatIdentity.fat_start]
	jb .copy
.ok:
	xor eax, eax
.done:
	ret
endp

; finish(identity, status) publishes or rolls back the complete operation.
; Every finish invalidates snapshots, including after a rollback.
proc f_finish uses rbx rsi rdi
	mov rbx, rcx
	mov edi, edx
	test edi, edi
	jnz .end
	fastcall f_info_unknown, rbx
	mov edi, eax
.end:
	mov rsi, qword [rbx + FatIdentity.ops]
	xor edx, edx
	test edi, edi
	setz dl
	fastcall [rsi + SectorOps.end], [rsi + SectorOps.context], edx
	mov dword [rbx + FatIdentity.transaction], 0
	fastcall fat_invalidate, rbx
	mov eax, edi
	ret
endp

; Allocate one zero-filled cluster. Each failed write is rolled back by the
; caller's transaction; output is assigned only after the cluster is ready.
proc f_allocate uses rbx rsi rdi r12 r13
	locals
		value dd ?
	endl
body:
	mov rbx, rcx
	mov rsi, rdx
	mov edi, dword [rbx + FatIdentity.next_free]
	mov r12d, dword [rbx + FatIdentity.cluster_count]
.scan:
	fastcall fat_get, rbx, edi, addr value
	test eax, eax
	jnz .done
	cmp dword [value], 0
	je .found
	inc edi
	mov eax, dword [rbx + FatIdentity.cluster_count]
	inc eax
	cmp edi, eax
	jbe .within
	mov edi, 2
.within:
	dec r12d
	jnz .scan
	mov eax, F_NOSPACE
	ret
.found:
	fastcall f_put, rbx, edi, F_LAST
	test eax, eax
	jnz .done
	mov r12d, edi
	lea rdi, [rbx + FatIdentity.scratch]
	mov ecx, dword [rbx + FatIdentity.sector_bytes]
	xor eax, eax
	rep stosb
	fastcall f_cluster_sector, rbx, r12d
	mov r13, rax
	xor edi, edi
.zero:
	fastcall f_write_sector, rbx, r13, addr rbx + FatIdentity.scratch
	test eax, eax
	jnz .done
	inc r13
	inc edi
	cmp edi, [rbx + FatIdentity.cluster_sectors]
	jb .zero
	mov dword [rsi], r12d
	lea eax, [r12d+1]
	mov ecx, dword [rbx + FatIdentity.cluster_count]
	inc ecx
	cmp eax, ecx
	jbe .hint
	mov eax, 2
.hint:
	mov dword [rbx + FatIdentity.next_free], eax
	xor eax, eax
.done:
	ret
endp

; Free a prevalidated finite chain; caller has already checked ownership of
; its entry and must not pass the root or any shared directory chain.
proc f_free uses rbx rsi
	locals
		next dd ?
	endl
body:
	mov rbx, rcx
	mov esi, edx
.loop:
	test esi, esi
	jz .ok
	fastcall fat_get, rbx, esi, addr next
	test eax, eax
	jnz .done
	fastcall f_put, rbx, esi, 0
	test eax, eax
	jnz .done
	mov esi, dword [next]
	cmp esi, F_EOC
	jb .loop
.ok:
	xor eax, eax
.done:
	ret
endp
