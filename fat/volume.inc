; Geometry, bounded FAT access, and transaction/cache coherence.
; Internal calls use Win64 too. EAX is a status except where stated explicitly.

; RCX=FatIdentity* in-out -> EAX=F_OK. No I/O; increment generation and drop
; FAT/directory cache keys plus allocation hints. Caller serializes/disallows an
; active operation; mount uses this before publishing magic.
section '.text$fat_invalidate' code readable executable comdat any align 16
proc fat_invalidate
	mov qword [rcx + FatIdentity.fat_lba], -1
	mov qword [rcx + FatIdentity.dir_lba], -1
	inc qword [rcx + FatIdentity.generation]
	mov qword [rcx + FatIdentity.shared], 0
	mov dword [rcx + FatIdentity.next_free], 2
	mov dword [rcx + FatIdentity.free_hint], -1
	xor eax, eax
	ret
endp

; mount(identity, ops, OEM_map_or_null, workspace). Failure leaves magic clear.
; RCX=FatIdentity* out, RDX=SectorOps*, R8=OEM map or 0, R9=FatWorkspace*.
; EAX=status; F_OK publishes a mounted identity, failure leaves magic=0. Only
; ops/OEM/workspace data are retained. See fat32.inc for the full contract.
section '.text$fat_mount' code readable executable comdat any align 16
proc fat_mount uses rbx rsi rdi rbp
	mov rbx, rcx
	mov rsi, rdx
	mov rbp, r9
	mov qword [rcx + FatIdentity.oem], r8
	mov dword [rcx + FatIdentity.magic], 0
	mov dword [rcx + FatIdentity.transaction], 0
	mov qword [rcx + FatIdentity.ops], rdx
	fastcall fat_invalidate, rcx
	mov eax, dword [rsi + SectorOps.sector_bytes]
	cmp eax, 512
	jb .format
	cmp eax, F_MAX_SECTOR
	ja .format
	lea edx, [rax-1]
	test eax, edx
	jnz .format
	cmp qword [rsi + SectorOps.sectors], 1
	jb .format
	cmp qword [rsi + SectorOps.read], 0
	je .format
	mov dword [rbx + FatIdentity.sector_bytes], eax
	lea edx, [rax+rax*2]
	cmp dword [rbp + FatWorkspace.bytes], edx
	jb .memory
	cmp dword [rbp + FatWorkspace.reserved], 0
	jne .argument
	mov rdi, qword [rbp + FatWorkspace.data]
	add rdx, rdi
	jc .argument
	mov qword [rbx + FatIdentity.fat], rdi
	add rdi, rax
	mov qword [rbx + FatIdentity.directory], rdi
	add rdi, rax
	mov qword [rbx + FatIdentity.scratch], rdi
	fastcall [rsi + SectorOps.read], [rsi + SectorOps.context], 0, rdi
	test eax, eax
	jnz .done
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
	lea edx, [rax-1]
	test eax, edx
	jnz .format
	mov dword [rbx + FatIdentity.cluster_sectors], eax
	imul eax, [rbx + FatIdentity.sector_bytes]
	cmp eax, F_MAX_CLUSTER
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
	lea ecx, [rax+2]
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
	; Geometry only. Chain/redundancy diagnostics are independently selected.
	xor eax, eax
	jmp .done
.format:
	mov eax, F_FORMAT
	mov dword [rbx + FatIdentity.magic], 0
.done:
	ret
.memory:
	mov eax, F_MEMORY
	jmp .done
.argument:
	mov eax, F_ARGUMENT
	jmp .done
endp

; RCX=identity*, RDX=relative uint64 LBA, R8=writable sector_bytes buffer.
; EAX=status; bounded by provider extent. Output on error follows provider.
; R9 dispatch is consumed before CALL; nothing depends on it after callback.
section '.text$f_read_sector' code readable executable comdat align 16
proc f_read_sector
	mov r9, qword [rcx + FatIdentity.ops]
	mov eax, F_RANGE
	cmp rdx, [r9 + SectorOps.sectors]
	jae .done
	fastcall [r9 + SectorOps.read], [r9 + SectorOps.context], rdx, r8
.done:
	ret
endp

; RCX=identity*, RDX=relative uint64 LBA, R8=readable complete sector.
; Requires active transaction and LBA < total_sectors. EAX=status. Invalidate a
; cache before staging if its LBA is replaced or its buffer supplied the bytes
; (the caller may have repurposed it). Unrelated caches remain valid. Input may
; be an identity cache/scratch sector; rollback clears both keys in f_finish.
section '.text$f_write_sector' code readable executable comdat align 16
proc f_write_sector
	mov eax, F_BUSY
	cmp dword [rcx + FatIdentity.transaction], 1
	jne .done
	mov r9d, dword [rcx + FatIdentity.total_sectors]
	mov eax, F_RANGE
	cmp rdx, r9
	jae .done
	cmp rdx, [rcx + FatIdentity.fat_lba]
	je .fat_changed
	cmp r8, [rcx + FatIdentity.fat]
	jne .directory
.fat_changed:
	mov qword [rcx + FatIdentity.fat_lba], -1
.directory:
	cmp rdx, [rcx + FatIdentity.dir_lba]
	je .dir_changed
	cmp r8, [rcx + FatIdentity.directory]
	jne .write
.dir_changed:
	mov qword [rcx + FatIdentity.dir_lba], -1
.write:
	mov rax, qword [rcx + FatIdentity.ops]
	fastcall [rax + SectorOps.write], [rax + SectorOps.context], rdx, r8
.done:
	ret
endp

; Cached directory sector. Pointer is identity.directory after success.
; RCX=identity*, RDX=directory-sector LBA -> EAX=status.
; F_OK makes identity.directory contain that sector; no pointer return. A failed
; read leaves dir_lba invalid. Cached bytes are borrowed until the next I/O.
section '.text$f_directory' code readable executable comdat align 16
proc f_directory uses rbx rsi
	mov rbx, rcx
	mov rsi, rdx
	cmp rdx, [rcx + FatIdentity.dir_lba]
	je .hit
	mov qword [rcx + FatIdentity.dir_lba], -1
	fastcall f_read_sector, rcx, rdx, [rcx + FatIdentity.directory]
	test eax, eax
	jnz .done
	mov qword [rbx + FatIdentity.dir_lba], rsi
.hit:
	xor eax, eax
.done:
	ret
endp

; fat_get(identity, cluster, out32), including reserved FAT[0] and FAT[1].
; RCX=mounted identity*, EDX=FAT index, R8=uint32_t* out -> EAX=status.
; F_OK writes low 28 bits; output unchanged on failure. FAT[0]/FAT[1] allowed.
; RBX/RSI/RDI retain identity/output/byte offset across the sector callback.
section '.text$fat_get' code readable executable comdat any align 16
proc fat_get uses rbx rsi rdi
	mov eax, F_CORRUPT
	cmp dword [rcx + FatIdentity.magic], F_MAGIC
	jne .done
	mov edi, dword [rcx + FatIdentity.cluster_count]
	add edi, 2
	cmp edx, edi
	jae .done
	mov rbx, rcx
	mov eax, edx
	mov rsi, r8

	shl rax, 2
	mov ecx, dword [rcx + FatIdentity.sector_bytes]
	xor edx, edx
	div rcx
	mov edi, edx
	mov edx, dword [rbx + FatIdentity.active_fat]
	imul edx, [rbx + FatIdentity.fat_sectors]
	add edx, [rbx + FatIdentity.fat_start]
	add rdx, rax
	cmp rdx, [rbx + FatIdentity.fat_lba]
	je .hit
	mov qword [rbx + FatIdentity.fat_lba], rdx
	fastcall f_read_sector, rbx, rdx, [rbx + FatIdentity.fat]
	test eax, eax
	jnz .failed
.hit:
	mov rbx, qword [rbx + FatIdentity.fat]
	mov eax, dword [rbx + rdi]
	and eax, F_MASK
	mov dword [rsi], eax
	xor eax, eax
.done:
	ret
.failed:
	mov qword [rbx + FatIdentity.fat_lba], -1
	jmp .done
endp

; Basic mutation hook preserves the reserved high nibble independently in every
; FAT copy. Disabled mirroring touches only the selected active FAT.
; RCX=identity*, EDX=FAT index, R8D=new masked 28-bit value -> EAX=status.
; Active transaction required. No comparison of previous low bits.
; Any partial staging is rolled back by the outer mutation's f_finish.
; Every exit invalidates fat_lba because the cache payload may hold another copy.
section '.text$fat_put' code readable executable comdat any align 16
proc fat_put uses rbx rsi rdi r12 rbp
	mov rbx, rcx
	mov eax, F_CORRUPT
	cmp dword [rcx + FatIdentity.magic], F_MAGIC
	jne .done
	mov eax, F_ARGUMENT
	test r8d, 0F0000000h
	jnz .done
	mov eax, F_BUSY
	cmp dword [rcx + FatIdentity.transaction], 1
	jne .done
	mov r12d, r8d
	mov ecx, dword [rcx + FatIdentity.cluster_count]
	add ecx, 2
	mov eax, F_CORRUPT
	cmp edx, ecx
	jae .done
	mov eax, edx
	mov ecx, dword [rbx + FatIdentity.sector_bytes]
	shl rax, 2
	xor edx, edx
	div rcx
	mov edi, edx
	; Keep this entry's LBA and exclusive end across callbacks. The end includes
	; its sector offset; only earlier LBAs are accessed. Mount bounds the geometry.
	mov ecx, dword [rbx + FatIdentity.fat_sectors]
	mov esi, dword [rbx + FatIdentity.fat_start]
	add rsi, rax
	mov ebp, dword [rbx + FatIdentity.fat_count]
	imul rbp, rcx
	add rbp, rsi
	cmp dword [rbx + FatIdentity.mirrored], 0
	jne .copy
	mov eax, dword [rbx + FatIdentity.active_fat]
	imul rax, rcx
	add rsi, rax
	lea rbp, [rsi+rcx]
.copy:
	fastcall f_read_sector, rbx, rsi, [rbx + FatIdentity.fat]
	test eax, eax
	jnz .done
	mov r8, qword [rbx + FatIdentity.fat]
	mov eax, dword [r8 + rdi]
	and eax, 0F0000000h
	or eax, r12d
	mov dword [r8 + rdi], eax
	fastcall f_write_sector, rbx, rsi, r8
	test eax, eax
	jnz .done
	mov ecx, [rbx + FatIdentity.fat_sectors]
	add rsi, rcx
	cmp rsi, rbp
	jb .copy
.done:
	mov qword [rbx + FatIdentity.fat_lba], -1
	ret
endp

; Map a data cluster to a relative sector. Private leaf: EDX -> RAX.
; RCX=identity*, EDX=prevalidated data cluster -> RAX=uint64 relative LBA.
; Private leaf exception: preserves RCX (relied on at adjacent call sites),
; clobbers EDX/flags. No range check and no F_* status return.
section '.text$f_cluster_sector' code readable executable comdat align 16
proc f_cluster_sector
	lea eax, [rdx-2]
	mov edx, dword [rcx + FatIdentity.cluster_sectors]
	imul rax, rdx
	mov edx, dword [rcx + FatIdentity.data_start]
	add rax, rdx
	ret
endp

; fat_chain(identity, first, {count,last}). Brent's cycle detector bounds a
; corrupt loop without allocating a volume-sized visited bitmap.
; RCX=mounted identity*, EDX=first or 0, R8=uint32_t[2]* out.
; EAX=status. Zero output first; publish {count,last} only at valid EOC. First=0
; is empty/F_OK. All checkpoint/count state must survive calls to fat_get.
section '.text$fat_chain' code readable executable comdat any align 16
proc fat_chain uses rbx rsi rdi r12 r13 r14 rbp, next:4
	assert next+4-(parmbase@proc) <= 32
body:
	mov qword [r8], 0
	test edx, edx
	jz .ok
	mov rbx, rcx
	mov edi, edx
	mov rsi, r8

	mov r12d, edx          ; checkpoint
	mov r13d, 1            ; power
	xor r14d, r14d         ; distance
	xor ebp, ebp           ; count
.loop:
	cmp edi, 2
	jb .bad
	mov eax, dword [rbx + FatIdentity.cluster_count]
	inc eax
	cmp edi, eax
	ja .bad
	inc ebp
	cmp ebp, [rbx + FatIdentity.cluster_count]
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
	mov dword [rsi], ebp
	mov dword [rsi+4], edi
.ok:
	xor eax, eax
.done:
	ret
.bad:
	mov eax, F_CORRUPT
	jmp .done
endp

; RCX=identity* -> EAX=status. Require mounted, idle, writable provider.
; Set identity.transaction only after successful begin callback; on failure no
; finish is due. RDX holds ops only until callback dispatch; RBX survives it.
section '.text$f_begin' code readable executable comdat align 16
proc f_begin uses rbx
	mov eax, F_FORMAT
	cmp dword [rcx + FatIdentity.magic], F_MAGIC
	jne .done
	mov eax, F_BUSY
	cmp dword [rcx + FatIdentity.transaction], 0
	jne .done
	mov rdx, qword [rcx + FatIdentity.ops]
	; Validate the optional extension before starting any savepoint.
	mov eax, F_ARGUMENT
	cmp dword [rdx + SectorOps.reserved], 0
	je .provider
	cmp dword [rdx + SectorOps.reserved], sizeof.FatOrderedOps
	jne .done
	cmp dword [rdx + FatOrderedOps.revision], 1
	jne .done
	cmp dword [rdx + FatOrderedOps.flags], 0
	jne .done
	cmp qword [rdx + FatOrderedOps.barrier], 0
	je .done
.provider:
	mov eax, F_READONLY
	cmp qword [rdx + SectorOps.write], 0
	je .done
	cmp qword [rdx + SectorOps.begin], 0
	je .done
	cmp qword [rdx + SectorOps.end], 0
	je .done
	mov rbx, rcx
	fastcall [rdx + SectorOps.begin], [rdx + SectorOps.context]
	test eax, eax
	jnz .done
	mov dword [rbx + FatIdentity.transaction], 1
.done:
	ret
endp

; RCX=id*, EDX=phase reason. Stage a dependency boundary, never physical I/O.
; f_begin validated the extension. Base providers do nothing.
section '.text$f_order' code readable executable comdat align 16
proc f_order
	mov r9, [rcx + FatIdentity.ops]
	xor eax, eax
	cmp dword [r9 + SectorOps.reserved], 0
	je .done
	fastcall [r9 + FatOrderedOps.barrier], [r9 + SectorOps.context], edx
.done:
	ret
endp

; FSInfo is advisory. Never turn a stale free-count hint into ENOSPC.
; Invalidate both valid primary and backup hints after allocation changes.
; RCX=identity* with active transaction -> EAX=status.
; Set free/next hints unknown in valid primary/backup FSInfo sectors. Invalid or
; absent signatures are skipped. Uses/destroys identity.scratch.
section '.text$f_info_unknown' code readable executable comdat align 16
proc f_info_unknown uses rbx rsi rdi
	mov rbx, rcx
	mov esi, dword [rcx + FatIdentity.fsinfo]
	test esi, esi
	jz .ok
	xor edi, edi
.copy:
	fastcall f_read_sector, rbx, rsi, [rbx + FatIdentity.scratch]
	test eax, eax
	jnz .done
	mov r8, qword [rbx + FatIdentity.scratch]
	; All three FSInfo signatures must agree before changing either hint.
	cmp dword [r8 + FDiskInfo.lead_signature], FSI_LEAD_SIGNATURE
	jne .next
	cmp dword [r8 + FDiskInfo.structure_signature], FSI_STRUCTURE_SIGNATURE
	jne .next
	cmp dword [r8 + FDiskInfo.trail_signature], FSI_TRAIL_SIGNATURE
	jne .next
	; Adjacent free_count and next_free dwords both become unknown (FFFFFFFFh).
	mov qword [r8 + FDiskInfo.free_count], -1
	fastcall f_write_sector, rbx, rsi, r8
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
; Acceptance retains the allocation cursor and cache keys still valid after
; staging. Rollback discards both keys and restarts the bounded FAT search: a
; cache may contain rejected data and next_free may reflect rejected allocation.
; RCX=identity*, EDX=operation status; matching successful f_begin required.
; EAX=operation/FSInfo status. Accept only F_OK, otherwise rollback; always end
; the savepoint. end callback is void/cannot fail. Shared owners publish their
; object versions after this succeeds; rollback leaves those versions intact.
section '.text$f_finish' code readable executable comdat align 16
proc f_finish uses rbx rdi
	mov rbx, rcx
	mov eax, edx
	test edx, edx
	jnz .end
	fastcall f_info_unknown, rcx
.end:
	mov r8, qword [rbx + FatIdentity.ops]
	xor edx, edx
	test eax, eax
	setz dl
	xchg edi, eax
	fastcall [r8 + SectorOps.end], [r8 + SectorOps.context], edx
	mov dword [rbx + FatIdentity.transaction], 0
	inc dword [rbx + FatIdentity.completions]
	mov dword [rbx + FatIdentity.free_hint], -1
	test edi, edi
	jz .done
	mov qword [rbx + FatIdentity.fat_lba], -1
	mov qword [rbx + FatIdentity.dir_lba], -1
	mov dword [rbx + FatIdentity.next_free], 2
.done:
	xchg eax, edi
	ret
endp

; Legacy snapshot policy only. Core/shared transactions never call this.
; RCX=identity*, RDX=record* or zero, R8D=completed operation status.
; Retire snapshots/shared ownership and refresh only the successful output.
; f_finish already settled caches/allocation state; advancing the snapshot
; generation must not invoke the full external-edit reset in fat_invalidate.
section '.text$f_snapshot_done' code readable executable comdat align 16
proc f_snapshot_done
	inc qword [rcx + FatIdentity.generation]
	mov qword [rcx + FatIdentity.shared], 0
	mov eax, r8d
	test eax, eax
	jnz .done
	test rdx, rdx
	jz .done
	mov r8, qword [rcx + FatIdentity.generation]
	mov qword [rdx + FatEntry.generation], r8
.done:
	ret
endp

; Allocate one zero-filled cluster. Each failed write is rolled back by the
; caller's transaction; output is assigned only after the cluster is ready.
; RCX=identity*, RDX=uint32_t* out cluster -> EAX=status.
; Active transaction required. F_OK allocates EOC, zeroes all cluster sectors,
; publishes output and leaves scratch[0..sector_bytes) zero. Failure leaves out
; unchanged; caller must roll back partially staged FAT/data with f_finish.
section '.text$f_allocate' code readable executable comdat align 16
proc f_allocate uses rbx rsi rdi rbp r13, value:4
	assert value+4-(parmbase@proc) <= 32
body:
	mov rbx, rcx
	mov rsi, rdx
	mov edi, dword [rcx + FatIdentity.next_free]
	mov ebp, dword [rcx + FatIdentity.cluster_count]
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
	dec ebp
	jnz .scan
	mov eax, F_NOSPACE
	jmp .done
.found:
	fastcall fat_put, rbx, edi, F_LAST
	test eax, eax
	jnz .done
	mov ebp, edi
	mov rdi, qword [rbx + FatIdentity.scratch]
	mov ecx, dword [rbx + FatIdentity.sector_bytes]
	xor eax, eax
	rep stosb
	fastcall f_cluster_sector, rbx, ebp
	mov r13, rax
	xor edi, edi
.zero:
	fastcall f_write_sector, rbx, r13, [rbx + FatIdentity.scratch]
	test eax, eax
	jnz .done
	inc r13
	inc edi
	cmp edi, [rbx + FatIdentity.cluster_sectors]
	jb .zero
	mov dword [rsi], ebp
	lea eax, [rbp+1]
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
; RCX=identity*, EDX=first or 0 -> EAX=status. Active transaction required.
; Caller has validated the entire chain and ownership; 0 is a no-op. Stages FAT
; entries as free; caller handles rollback on error. Does not erase data bytes.
section '.text$f_free' code readable executable comdat align 16
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
	fastcall fat_put, rbx, esi, 0
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
