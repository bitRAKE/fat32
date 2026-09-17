; Optional checked mutation hook preserves the reserved high nibble independently in every
; FAT copy. Disabled mirroring touches only the selected active FAT.
; RCX=identity*, EDX=FAT index, R8D=new masked 28-bit value -> EAX=status.
; Called within an existing mutation, never a standalone transaction.
; Active transaction required. Preserve each mirror's high nibble; reject low-bit
; disagreement. Any partial staging is rolled back by the caller's f_finish.
; Every exit invalidates fat_lba because the cache payload may hold another copy.
section '.text$fat_put_checked' code readable executable comdat any align 16
proc fat_put_checked uses rbx rsi rdi r12 rbp, old_value:4, cluster_id:4
	assert cluster_id+4-(parmbase@proc) <= 32

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
	mov dword [cluster_id], edx
	fastcall fat_get, rcx, edx, addr old_value
	test eax, eax
	jnz .done
	mov edx, dword [rbx + FatIdentity.cluster_count]
	mov eax, dword [cluster_id]
	add edx, 2
	cmp eax, edx
	jae .bad
	shl rax, 2
	mov ecx, dword [rbx + FatIdentity.sector_bytes]
	xor edx, edx
	div rcx
	mov edi, edx
	; As in fat_put, use a 64-bit LBA/end pair, including the sector offset.
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
	mov ecx, eax
	and ecx, F_MASK
	cmp ecx, dword [old_value]
	jne .bad
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
.bad:
	mov eax, F_CORRUPT
	jmp .done
endp
