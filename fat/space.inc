; Optional exact free count of the currently selected coherent FAT view.
; No FSInfo trust, stored exact-count cache, mutation or implicit verification.
; RCX=identity*, RDX=uint32_t* out. Output is unchanged on any failure.
section '.text$fat_count_free' code readable executable comdat any align 16
proc fat_count_free uses rbx rsi rdi rbp r13
	locals
		value dd ?
	endl
body:
	mov eax, F_FORMAT
	cmp dword [rcx + FatIdentity.magic], F_MAGIC
	jne .done
	mov rbx, rcx
	mov esi, 2
	mov r13, rdx
	mov edi, [rcx + FatIdentity.cluster_count]
	add edi, 2
	xor ebp, ebp
.scan:
	fastcall fat_get, rbx, esi, addr value
	test eax, eax
	jnz .done
	inc esi
	cmp dword [value], 1
	adc ebp, 0
	cmp esi, edi
	jb .scan
	mov [r13], ebp
	xor eax, eax
.done:
	ret
endp
