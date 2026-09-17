; Optional read-first diagnostic policy. No retry, mutation or hidden repair.
; The caller holds one volume lease across the read and any diagnosis.
section '.text$fat_read_adaptive' code readable executable comdat any align 16
proc fat_read_adaptive uses rbx rsi rdi rbp
	mov rbx, rcx
	mov rsi, rdx
	mov rdi, r8
	mov dword [rdx + FatTransfer.done], 0
	mov eax, F_ARGUMENT
	cmp dword [r8 + FatCheckedRead.reserved], 0
	jne .done
	; Respect already diagnosed damage only while its file/chain view is current.
	fastcall fat_check_fresh, rcx, [r8 + FatCheckedRead.report]
	cmp eax, F_CORRUPT
	je .done
	; Scope zero denotes no diagnosis for this attempt, not checked-good health.
	fastcall f_check_clear, [rdi + FatCheckedRead.report]
	fastcall fat_read_at, rbx, rsi
	cmp eax, F_CORRUPT
	je .diagnose
	cmp eax, F_IO
	jne .done
.diagnose:
	xchg ebp, eax
	fastcall fat_check_file, rbx, [rdi + FatCheckedRead.budget], [rdi + FatCheckedRead.report]
	; A successful diagnosis cannot turn a failed read into success. Diagnostics
	; do not touch the caller's output, completed prefix or independent position.
	xchg eax, ebp
.done:
	ret
endp
