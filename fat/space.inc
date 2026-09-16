; Optional exact free count of the currently selected coherent FAT view.
; No FSInfo trust, stored exact-count cache, mutation or implicit verification.
; RCX=identity*, RDX=uint32_t* out. Output is unchanged on any failure.
section '.text$fat_count_free' code readable executable comdat any align 16
proc fat_count_free uses rbx rsi rdi rbp r13
	locals
		value dd ?
	endl
body:
	mov rbx, rcx
	mov r13, rdx
	mov eax, F_FORMAT
	cmp dword [rbx + FatIdentity.magic], F_MAGIC
	jne .done
	mov edi, [rbx + FatIdentity.cluster_count]
	add edi, 2
	mov esi, 2
	xor ebp, ebp
.scan:
	fastcall fat_get, rbx, esi, addr value
	test eax, eax
	jnz .done
	cmp dword [value], 1
	adc ebp, 0
	inc esi
	cmp esi, edi
	jb .scan
	mov [r13], ebp
	xor eax, eax
.done:
	ret
endp
