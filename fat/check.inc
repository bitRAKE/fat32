; Separately linked structural diagnostics; no mutation and no hidden policy.
; Caller holds the same operation lease as basic I/O (or fat_call_locked).

; RCX=report*. Clear all fields, default unknown evidence LBA. Preserves RDX/R8/R9.
section '.text$f_check_clear' code readable executable comdat align 16
proc f_check_clear uses rdi
	mov rdi, rcx
	mov r10, rcx
	mov ecx, sizeof.FatCheck
	xor eax, eax
	rep stosb
	mov qword [r10 + FatCheck.sector], -1
	ret
endp

; RCX=identity*, RDX=report*, R8D=scope. EAX=status; report initialized even
; on error. Preserves RCX/RDX/R8/R9; no I/O or cache changes here.
section '.text$f_check_init' code readable executable comdat align 16
proc f_check_init uses rbx
	mov rbx, rcx
	fastcall f_check_clear, rdx
	mov rcx, rbx
	mov qword [rdx + FatCheck.identity], rcx
	mov rax, [rcx + FatIdentity.generation]
	mov [rdx + FatCheck.generation], rax
	mov [rdx + FatCheck.scope], r8d
	mov eax, F_FORMAT
	cmp dword [rcx + FatIdentity.magic], F_MAGIC
	jne .done
	mov eax, F_BUSY
	cmp dword [rcx + FatIdentity.transaction], 0
	jne .done
	xor eax, eax
.done:
	mov [rdx + FatCheck.status], eax
	ret
endp

; RCX=id*, EDX=cluster, R8D=link budget, R9=report*. Brent's bounded check is
; separate from fat_chain (the tail/count primitive used by actual mutations).
section '.text$fat_check_chain' code readable executable comdat any align 16
proc fat_check_chain uses rbx rsi rdi r12 r13 rbp r15
	locals
		next dd ?
	endl
body:
	mov rbx, rcx
	mov rsi, r9
	mov edi, edx
	mov r15d, r8d
	fastcall f_check_init, rcx, rsi, FC_CHAIN
	test eax, eax
	jnz .done
	mov [rsi + FatCheck.first], edi
	mov [rsi + FatCheck.budget], r15d
	test edi, edi
	jz .ok
	mov r12d, edi
	mov r13d, 1
	xor ebp, ebp
.loop:
	mov [rsi + FatCheck.cluster], edi
	mov qword [rsi + FatCheck.sector], -1
	lea eax, [edi-2]
	cmp eax, [rbx + FatIdentity.cluster_count]
	jae .link
	mov eax, [rsi + FatCheck.examined]
	cmp eax, r15d
	jae .limit
	cmp eax, [rbx + FatIdentity.cluster_count]
	jae .cycle
	; Evidence points to the active FAT sector, also on a provider failure.
	mov eax, edi
	shl rax, 2
	xor edx, edx
	mov ecx, [rbx + FatIdentity.sector_bytes]
	div rcx
	mov edx, [rbx + FatIdentity.active_fat]
	imul edx, [rbx + FatIdentity.fat_sectors]
	add edx, [rbx + FatIdentity.fat_start]
	add rax, rdx
	mov [rsi + FatCheck.sector], rax
	fastcall fat_get, rbx, edi, addr next
	test eax, eax
	jnz .io
	inc dword [rsi + FatCheck.examined]
	inc dword [rsi + FatCheck.count]
	mov eax, [next]
	mov [rsi + FatCheck.observed], eax
	cmp eax, F_EOC
	jae .end
	lea ecx, [eax-2]
	cmp ecx, [rbx + FatIdentity.cluster_count]
	jae .link
	inc ebp
	cmp eax, r12d
	je .cycle
	cmp ebp, r13d
	jne .advance
	mov r12d, eax
	shl r13d, 1
	xor ebp, ebp
.advance:
	mov edi, eax
	jmp .loop
.end:
	mov [rsi + FatCheck.last], edi
.ok:
	xor eax, eax
.done:
	mov [rsi + FatCheck.status], eax
	ret
.link:
	mov dword [rsi + FatCheck.issue], FC_LINK
	mov eax, F_CORRUPT
	jmp .done
.cycle:
	mov dword [rsi + FatCheck.issue], FC_CYCLE
	mov eax, F_CORRUPT
	jmp .done
.limit:
	mov dword [rsi + FatCheck.issue], FC_BUDGET
	mov eax, F_LIMIT
	jmp .done
.io:
	mov dword [rsi + FatCheck.issue], FC_IO
	jmp .done
endp

; RCX=handle*, EDX=budget, R8=report*. File chain and sufficient length only.
; Bind evidence to canonical chain_version; content/rename changes do not stale
; it. Parent membership, directory records and global ownership are not checked.
section '.text$fat_check_file' code readable executable comdat any align 16
proc fat_check_file uses rbx rsi rdi rbp r13
	mov rbx, rcx
	mov edi, edx
	mov rsi, r8
	fastcall f_handle, rcx
	test eax, eax
	jnz .invalid
	mov rbp, r11
	mov r13, r10
	mov eax, F_ARGUMENT
	test byte [rbp + FatRecord.raw+11], 18h
	jnz .invalid
	fastcall fat_check_chain, [r13 + FatVolume.identity], [rbp + FatRecord.cluster], edi, rsi
	mov dword [rsi + FatCheck.scope], FC_FILE
	mov [rsi + FatCheck.volume], r13
	mov rcx, [r13 + FatVolume.epoch]
	mov [rsi + FatCheck.epoch], rcx
	mov [rsi + FatCheck.object], rbp
	mov rcx, [rbp + FatObject.incarnation]
	mov [rsi + FatCheck.incarnation], rcx
	mov rcx, [rbp + FatObject.chain_version]
	mov [rsi + FatCheck.version], rcx
	test eax, eax
	jnz .done
	mov rbx, [r13 + FatVolume.identity]
	mov eax, [rbp + FatRecord.size]
	mov ecx, [rbx + FatIdentity.cluster_bytes]
	lea rax, [rax+rcx-1]
	xor edx, edx
	div rcx
	mov [rsi + FatCheck.expected], eax
	cmp [rsi + FatCheck.count], eax
	jb .short
	je .ok
	or dword [rsi + FatCheck.flags], FC_EXTRA
.ok:
	xor eax, eax
.done:
	mov [rsi + FatCheck.status], eax
	ret
.short:
	mov dword [rsi + FatCheck.issue], FC_SHORT
	mov eax, F_CORRUPT
	jmp .done
.invalid:
	mov ebx, eax
	fastcall f_check_clear, rsi
	mov dword [rsi + FatCheck.scope], FC_FILE
	mov eax, ebx
	jmp .done
endp

; RCX=handle*, RDX=report*. No I/O. F_OK means only current scoped evidence.
; Other current outcomes are returned unchanged, including F_LIMIT and F_IO.
section '.text$fat_check_fresh' code readable executable comdat any align 16
proc fat_check_fresh
	fastcall f_handle, rcx
	test eax, eax
	jnz .done
	mov eax, F_STALE
	cmp dword [rdx + FatCheck.scope], FC_FILE
	jne .done
	cmp [rdx + FatCheck.volume], r10
	jne .done
	cmp [rdx + FatCheck.object], r11
	jne .done
	mov r8, [r10 + FatVolume.epoch]
	cmp [rdx + FatCheck.epoch], r8
	jne .done
	mov r8, [r11 + FatObject.incarnation]
	cmp [rdx + FatCheck.incarnation], r8
	jne .done
	mov r8, [r11 + FatObject.chain_version]
	cmp [rdx + FatCheck.version], r8
	jne .done
	mov r8, [r10 + FatVolume.identity]
	cmp [rdx + FatCheck.identity], r8
	jne .done
	mov r8, [r8 + FatIdentity.generation]
	cmp [rdx + FatCheck.generation], r8
	jne .done
	mov eax, [rdx + FatCheck.status]
.done:
	ret
endp

; RCX=handle*, RDX=transfer*, R8=request*. Checked alternate interface. Never
; silently retry a failed diagnostic; caller explicitly requests any new check.
section '.text$fat_read_checked' code readable executable comdat any align 16
proc fat_read_checked uses rbx rsi rdi
	mov rbx, rcx
	mov rsi, rdx
	mov rdi, r8
	mov dword [rsi + FatTransfer.done], 0
	mov eax, F_ARGUMENT
	cmp dword [rdi + FatCheckedRead.reserved], 0
	jne .done
	fastcall fat_check_fresh, rcx, [rdi + FatCheckedRead.report]
	cmp eax, F_STALE
	jne .checked
	fastcall fat_check_file, rbx, [rdi + FatCheckedRead.budget], [rdi + FatCheckedRead.report]
.checked:
	test eax, eax
	jnz .done
	fastcall fat_read_at, rbx, rsi
.done:
	ret
endp

; RCX=id*, RDX=report*. FAT[0] media/reserved bits and FAT[1] status. Reads
; primary BPB and selected FAT afresh through provider, not bypassing its cache.
section '.text$fat_check_reserved' code readable executable comdat any align 16
proc fat_check_reserved uses rbx rsi rdi
	locals
		value dd ?
	endl
body:
	mov rbx, rcx
	mov rsi, rdx
	fastcall f_check_init, rcx, rdx, FC_RESERVED
	test eax, eax
	jnz .done
	mov qword [rsi + FatCheck.sector], 0
	fastcall f_read_sector, rbx, 0, [rbx + FatIdentity.scratch]
	test eax, eax
	jnz .io
	mov r9, [rbx + FatIdentity.scratch]
	movzx edi, byte [r9+21]
	or edi, 0FFFFF00h
	mov [rsi + FatCheck.expected], edi
	mov eax, [rbx + FatIdentity.active_fat]
	imul eax, [rbx + FatIdentity.fat_sectors]
	add eax, [rbx + FatIdentity.fat_start]
	mov [rsi + FatCheck.sector], rax
	mov qword [rbx + FatIdentity.fat_lba], -1
	fastcall fat_get, rbx, 0, addr value
	test eax, eax
	jnz .io
	inc dword [rsi + FatCheck.examined]
	mov eax, [value]
	mov [rsi + FatCheck.observed], eax
	cmp eax, edi
	jne .bad
	mov dword [rsi + FatCheck.cluster], 1
	mov dword [rsi + FatCheck.expected], 03FFFFFFh
	fastcall fat_get, rbx, 1, addr value
	test eax, eax
	jnz .io
	inc dword [rsi + FatCheck.examined]
	mov eax, [value]
	mov [rsi + FatCheck.observed], eax
	and eax, 03FFFFFFh
	cmp eax, 03FFFFFFh
	jne .bad
	test dword [value], 08000000h
	jnz .clean
	or dword [rsi + FatCheck.flags], FC_DIRTY
.clean:
	test dword [value], 04000000h
	jnz .status
	or dword [rsi + FatCheck.flags], FC_HARD_ERROR
.status:
	xor eax, eax
	cmp dword [rsi + FatCheck.flags], 0
	je .done
	mov dword [rsi + FatCheck.issue], FC_STATUS
	mov eax, F_ATTENTION
.done:
	mov [rsi + FatCheck.status], eax
	ret
.bad:
	mov dword [rsi + FatCheck.issue], FC_RESERVED_VALUE
	mov eax, F_CORRUPT
	jmp .done
.io:
	mov dword [rsi + FatCheck.issue], FC_IO
	jmp .done
endp

; RCX=id*, RDX=report*. Compare BPB bytes 11..51 and volume serial, plus boot
; signatures. Ignore boot code, labels and reserved padding. Absence is explicit.
section '.text$fat_check_backup' code readable executable comdat any align 16
proc fat_check_backup uses rbx rsi rdi rbp
	mov rbx, rcx
	mov rbp, rdx
	fastcall f_check_init, rcx, rdx, FC_BACKUP
	test eax, eax
	jnz .done
	mov qword [rbx + FatIdentity.fat_lba], -1
	mov qword [rbp + FatCheck.sector], 0
	fastcall f_read_sector, rbx, 0, [rbx + FatIdentity.fat]
	test eax, eax
	jnz .io
	mov rsi, [rbx + FatIdentity.fat]
	cmp word [rsi+510], 0AA55h
	jne .signature
	movzx edx, word [rsi+50]
	test edx, edx
	jz .absent
	mov [rbp + FatCheck.observed], edx
	cmp edx, [rbx + FatIdentity.fat_start]
	jae .location
	mov [rbp + FatCheck.sector], rdx
	fastcall f_read_sector, rbx, rdx, [rbx + FatIdentity.scratch]
	test eax, eax
	jnz .io
	mov rdi, [rbx + FatIdentity.scratch]
	cmp word [rdi+510], 0AA55h
	jne .signature
	mov ecx, 11
.compare:
	movzx eax, byte [rsi+rcx]
	movzx edx, byte [rdi+rcx]
	cmp eax, edx
	jne .mismatch
	inc ecx
	cmp ecx, 52
	jb .compare
	movzx eax, byte [rsi+66]
	movzx edx, byte [rdi+66]
	cmp eax, edx
	jne .mismatch
	cmp eax, 29h
	jne .ok
	mov eax, [rsi+67]
	mov edx, [rdi+67]
	cmp eax, edx
	jne .mismatch
.ok:
	xor eax, eax
.done:
	mov [rbp + FatCheck.status], eax
	ret
.absent:
	or dword [rbp + FatCheck.flags], FC_ABSENT
	jmp .ok
.mismatch:
	mov [rbp + FatCheck.expected], eax
	mov [rbp + FatCheck.observed], edx
	mov dword [rbp + FatCheck.issue], FC_BACKUP_MISMATCH
	jmp .bad
.signature:
	mov dword [rbp + FatCheck.issue], FC_BACKUP_SIGNATURE
	jmp .bad
.location:
	mov dword [rbp + FatCheck.issue], FC_BACKUP_LOCATION
.bad:
	mov eax, F_CORRUPT
	jmp .done
.io:
	mov dword [rbp + FatCheck.issue], FC_IO
	jmp .done
endp

; RCX=id*, RDX=range*, R8=report*. Compare only requested low 28-bit entries;
; preserved high nibbles and unaddressable FAT padding are not consistency data.
; At most one active-sector read + (fat_count-1) other reads per range sector.
section '.text$fat_check_mirrors' code readable executable comdat any align 16
proc fat_check_mirrors uses rbx rsi rdi rbp r13 r14 r15
	locals
		part dq ?
		offset dd ?
		entries dd ?
		value dd ?
	endl
body:
	mov rbx, rcx
	mov rsi, r8
	mov edi, [rdx + FatFatRange.first]
	mov ebp, [rdx + FatFatRange.count]
	fastcall f_check_init, rcx, rsi, FC_MIRRORS
	test eax, eax
	jnz .done
	mov [rsi + FatCheck.first], edi
	mov eax, edi
	add rax, rbp
	mov ecx, [rbx + FatIdentity.cluster_count]
	add ecx, 2
	cmp rax, rcx
	ja .range
	cmp dword [rbx + FatIdentity.mirrored], 0
	je .inactive
	cmp dword [rbx + FatIdentity.fat_count], 2
	jb .ok
	; Keep the public comparison count exact. Larger requests are split by
	; the caller into explicit bounded ranges, without changing base state.
	mov eax, [rbx + FatIdentity.fat_count]
	dec eax
	imul rax, rbp
	shr rax, 32
	jnz .range
	mov qword [rbx + FatIdentity.fat_lba], -1
.page:
	test ebp, ebp
	jz .ok
	mov eax, edi
	shl rax, 2
	xor edx, edx
	mov ecx, [rbx + FatIdentity.sector_bytes]
	div rcx
	mov [part], rax
	mov [offset], edx
	sub ecx, edx
	shr ecx, 2
	cmp ecx, ebp
	cmova ecx, ebp
	mov [entries], ecx
	mov eax, [rbx + FatIdentity.fat_start]
	add rax, [part]
	mov [rsi + FatCheck.sector], rax
	mov [rsi + FatCheck.cluster], edi
	fastcall fat_get, rbx, edi, addr value
	test eax, eax
	jnz .io
	mov r13d, 1
.copy:
	mov eax, r13d
	imul eax, [rbx + FatIdentity.fat_sectors]
	add eax, [rbx + FatIdentity.fat_start]
	add rax, [part]
	mov [rsi + FatCheck.sector], rax
	fastcall f_read_sector, rbx, rax, [rbx + FatIdentity.scratch]
	test eax, eax
	jnz .io
	mov r14, [rbx + FatIdentity.fat]
	mov r15, [rbx + FatIdentity.scratch]
	mov ecx, [offset]
	add r14, rcx
	add r15, rcx
	xor ecx, ecx
.compare:
	mov eax, [r14+rcx*4]
	mov edx, [r15+rcx*4]
	and eax, F_MASK
	and edx, F_MASK
	inc dword [rsi + FatCheck.examined]
	cmp eax, edx
	jne .mismatch
	inc ecx
	cmp ecx, [entries]
	jb .compare
	inc r13d
	cmp r13d, [rbx + FatIdentity.fat_count]
	jb .copy
	mov eax, [entries]
	add edi, eax
	sub ebp, eax
	jmp .page
.ok:
	xor eax, eax
.done:
	mov [rsi + FatCheck.status], eax
	ret
.mismatch:
	add ecx, edi
	mov [rsi + FatCheck.cluster], ecx
	mov [rsi + FatCheck.expected], eax
	mov [rsi + FatCheck.observed], edx
	mov dword [rsi + FatCheck.issue], FC_MIRROR_MISMATCH
	mov eax, F_CORRUPT
	jmp .done
.range:
	mov eax, F_RANGE
	jmp .done
.inactive:
	or dword [rsi + FatCheck.flags], FC_INACTIVE
	jmp .ok
.io:
	mov dword [rsi + FatCheck.issue], FC_IO
	jmp .done
endp
