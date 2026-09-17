; Compatibility snapshot API. Strong preflight and whole-volume snapshot
; invalidation are deliberately outside the normal shared-file primitives.
section '.text$fat_read' code readable executable comdat any align 16
proc fat_read uses rbx rsi rdi
	mov rbx, rcx
	mov rsi, rdx
	mov rdi, r8
	mov dword [r8 + FatTransfer.done], 0
	fastcall f_file, rcx, rdx
	test eax, eax
	jnz .done
	fastcall f_read_record, rbx, rsi, rdi
.done:
	ret
endp

section '.text$fat_write' code readable executable comdat any align 16
proc fat_write uses rbx rsi rdi rbp
	mov rbx, rcx
	mov rsi, rdx
	mov rdi, r8
	mov dword [r8 + FatTransfer.done], 0
	fastcall f_file, rcx, rdx
	test eax, eax
	jnz .done
	mov ebp, dword [rbx + FatIdentity.completions]
	fastcall f_write_record, rbx, rsi, rdi
	cmp ebp, [rbx + FatIdentity.completions]
	je .done
	fastcall f_snapshot_done, rbx, rsi, eax
.done:
	ret
endp

section '.text$fat_resize' code readable executable comdat any align 16
proc fat_resize uses rbx rsi rdi rbp
	mov rbx, rcx
	mov rsi, rdx
	mov rdi, r8
	fastcall f_file, rcx, rdx
	test eax, eax
	jnz .done
	mov ebp, dword [rbx + FatIdentity.completions]
	fastcall f_resize_record, rbx, rsi, rdi
	cmp ebp, [rbx + FatIdentity.completions]
	je .done
	fastcall f_snapshot_done, rbx, rsi, eax
.done:
	ret
endp

section '.text$fat_set_info' code readable executable comdat any align 16
proc fat_set_info uses rbx rsi rdi rbp
	mov rbx, rcx
	mov rsi, rdx
	mov rdi, r8
	fastcall f_validate, rcx, rdx
	test eax, eax
	jnz .done
	mov ebp, dword [rbx + FatIdentity.completions]
	fastcall f_set_record, rbx, rsi, rdi
	cmp ebp, [rbx + FatIdentity.completions]
	je .done
	fastcall f_snapshot_done, rbx, rsi, eax
.done:
	ret
endp

section '.text$fat_rename' code readable executable comdat any align 16
proc fat_rename uses rbx rsi rdi rbp r13 r14
	mov rbx, rcx
	mov rbp, rdx
	mov r13, r8
	fastcall f_validate, rcx, rdx
	test eax, eax
	jnz .done
	mov r14d, dword [rbx + FatIdentity.completions]
	fastcall f_rename_record, rbx, rbp, r13
	cmp r14d, [rbx + FatIdentity.completions]
	je .done
	fastcall f_snapshot_done, rbx, rbp, eax
	test eax, eax
	jnz .done
	mov rsi, r13
	lea rdi, [rbp + FatEntry.name]
	mov ecx, dword [rbp + FatEntry.name_length]
	inc ecx
	rep movsw
.done:
	ret
endp

section '.text$fat_remove' code readable executable comdat any align 16
proc fat_remove uses rbx rsi rdi
	mov rbx, rcx
	mov rsi, rdx
	fastcall f_validate, rcx, rdx
	test eax, eax
	jnz .done
	mov edi, dword [rbx + FatIdentity.completions]
	fastcall f_remove_record, rbx, rsi
	cmp edi, [rbx + FatIdentity.completions]
	je .done
	fastcall f_snapshot_done, rbx, 0, eax
.done:
	ret
endp
