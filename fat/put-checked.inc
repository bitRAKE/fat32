; Optional checked mutation hook preserves the reserved high nibble independently in every
; FAT copy. Disabled mirroring touches only the selected active FAT.
; RCX=identity*, EDX=FAT index, R8D=new masked 28-bit value -> EAX=status.
; Called within an existing mutation, never a standalone transaction.
; Active transaction required. Preserve each mirror's high nibble; reject low-bit
; disagreement. Any partial staging is rolled back by the caller's f_finish.
; Every exit invalidates fat_lba because the cache payload may hold another copy.
section '.text$fat_put_checked' code readable executable comdat any align 16
proc fat_put_checked uses rbx rsi rdi r12 rbp
	locals
		part dq ?
		old_value dd ?
		cluster_id dd ?
	endl
body:
	mov rbx, rcx
	mov eax, F_CORRUPT
	cmp dword [rbx + FatIdentity.magic], F_MAGIC
	jne .done
	mov eax, F_ARGUMENT
	test r8d, 0F0000000h
	jnz .done
	mov eax, F_BUSY
	cmp dword [rbx + FatIdentity.transaction], 1
	jne .done
	mov r12d, r8d
	mov dword [cluster_id], edx
	fastcall fat_get, rcx, edx, addr old_value
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
	mov ebp, dword [rbx + FatIdentity.fat_count]
	cmp dword [rbx + FatIdentity.mirrored], 0
	jne .copy
	mov esi, dword [rbx + FatIdentity.active_fat]
	lea ebp, [rsi+1]
.copy:
	mov eax, esi
	imul eax, [rbx + FatIdentity.fat_sectors]
	add eax, [rbx + FatIdentity.fat_start]
	add rax, [part]
	fastcall f_read_sector, rbx, rax, [rbx + FatIdentity.fat]
	test eax, eax
	jnz .done
	mov r9, qword [rbx + FatIdentity.fat]
	mov eax, dword [r9 + rdi]
	mov ecx, eax
	and ecx, F_MASK
	cmp ecx, dword [old_value]
	jne .bad
	and eax, 0F0000000h
	or eax, r12d
	mov dword [r9 + rdi], eax
	mov eax, esi
	imul eax, [rbx + FatIdentity.fat_sectors]
	add eax, [rbx + FatIdentity.fat_start]
	add rax, [part]
	fastcall f_write_sector, rbx, rax, [rbx + FatIdentity.fat]
	test eax, eax
	jnz .done
	inc esi
	cmp esi, ebp
	jb .copy
	xor eax, eax
.done:
	mov qword [rbx + FatIdentity.fat_lba], -1
	ret
.bad:
	mov eax, F_CORRUPT
	jmp .done
endp
