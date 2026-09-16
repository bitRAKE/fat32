; Directory cursors and names. Raw 8.3 bytes are always retained. An optional
; OEM table belongs to the caller; the deterministic default is CP437.

; RCX=mounted identity*, EDX=first directory cluster, R8=FatCursor* out.
; EAX=status. Validate chain before initializing output; unchanged on error.
; EDX stays live through the first call setup, while EDI keeps it across CALL.
proc fat_dir_open uses rbx rsi rdi
	locals
		chain dq ?
	endl
body:
	mov rbx, rcx
	mov rsi, r8
	mov edi, edx
	mov eax, F_CORRUPT
	cmp dword [rbx + FatIdentity.magic], F_MAGIC
	jne .done
	cmp edi, 2
	jb .done
	fastcall fat_chain, rcx, edx, addr chain
	test eax, eax
	jnz .done
	mov edx, edi
	mov rdi, rsi
	xor eax, eax
	mov ecx, sizeof.FatCursor
	rep stosb
	mov rax, qword [rbx + FatIdentity.generation]
	mov qword [rsi + FatCursor.generation], rax
	mov dword [rsi + FatCursor.cluster], edx
	mov dword [rsi + FatCursor.parent], edx
	xor eax, eax
.done:
	ret
endp

; Get the cursor's current sector, advancing at a boundary. Does not consume
; a slot. The caller increments offset/index after copying the slot.
; RCX=identity*, RDX=FatCursor* in-out -> EAX=F_OK/F_END/error.
; Cursor must be from fat_dir_open. F_OK loads identity.directory and advances
; cluster/sector at boundaries, but does not consume a slot. Error may modify
; cursor; reopen instead of retrying it. Generation mismatch gives F_STALE.
proc f_cursor_sector uses rbx rsi
	locals
		next dd ?
	endl
body:
	mov rbx, rcx
	mov rsi, rdx
	mov rax, qword [rbx + FatIdentity.generation]
	cmp rax, [rsi + FatCursor.generation]
	jne .stale
	cmp dword [rsi + FatCursor.ended], 0
	jne .end
	mov eax, dword [rsi + FatCursor.offset]
	cmp eax, [rbx + FatIdentity.sector_bytes]
	jb .load
	mov dword [rsi + FatCursor.offset], 0
	inc dword [rsi + FatCursor.sector_in]
	mov eax, dword [rsi + FatCursor.sector_in]
	cmp eax, [rbx + FatIdentity.cluster_sectors]
	jb .load
	mov dword [rsi + FatCursor.sector_in], 0
	fastcall fat_get, rbx, [rsi + FatCursor.cluster], addr next
	test eax, eax
	jnz .done
	mov eax, dword [next]
	cmp eax, F_EOC
	jae .end
	cmp eax, 2
	jb .bad
	mov ecx, dword [rbx + FatIdentity.cluster_count]
	inc ecx
	cmp eax, ecx
	ja .bad
	mov dword [rsi + FatCursor.cluster], eax
	inc dword [rsi + FatCursor.steps]
	mov eax, dword [rsi + FatCursor.steps]
	cmp eax, [rbx + FatIdentity.cluster_count]
	jae .bad
.load:
	fastcall f_cluster_sector, rbx, [rsi + FatCursor.cluster]
	mov edx, dword [rsi + FatCursor.sector_in]
	add rdx, rax
	fastcall f_directory, rcx, rdx ; f_cluster_sector preserves identity in RCX
.done:
	ret
.end:
	mov dword [rsi + FatCursor.ended], 1
	mov eax, F_END
	jmp .done
.bad:
	mov eax, F_CORRUPT
	jmp .done
.stale:
	mov eax, F_STALE
	jmp .done
endp

; checksum(raw SFN) -> AL. Private leaf.
; RCX=const uint8_t[11] SFN -> EAX=zero-extended 8-bit LFN checksum.
; Private leaf; advances RCX and clobbers EDX/flags. No F_* status return.
proc f_checksum
	xor eax, eax
	mov edx, 11
.loop:
	ror al, 1
	add al, [rcx]
	inc rcx
	dec edx
	jnz .loop
	ret
endp

; Decode the short name to entry.name, respecting NT lowercase flags.
; RCX=identity*, RDX=FatEntry* with raw[0..12] initialized -> void.
; Write name/name_length from SFN/OEM/case flags; preserve raw bytes. R8/R9 are
; leaf-only map/index temporaries. RDI is saved for STOSW; EAX is unspecified.
proc f_short_name uses rdi
	lea rdi, [rdx + FatEntry.name]
	mov r8, qword [rcx + FatIdentity.oem]
	test r8, r8
	jnz .map
	lea r8, [f_cp437]
.map:
	xor r9d, r9d
.base:
	movzx eax, byte [rdx + FatEntry.raw + r9]
	cmp al, 20h
	je .extension
	test r9d, r9d
	jnz .basecase
	cmp al, 5
	jne .basecase
	mov eax, 0E5h
.basecase:
	test byte [rdx + FatEntry.raw+12], 8
	jz .baseemit
	cmp eax, 'A'
	jb .baseemit
	cmp eax, 'Z'
	ja .baseemit
	add eax, 32
.baseemit:
	mov ax, word [r8+rax*2]
	stosw
	inc r9d
	cmp r9d, 8
	jb .base
.extension:
	cmp byte [rdx + FatEntry.raw+8], 20h
	je .done
	mov ax, '.'
	stosw
	mov r9d, 8
.ext:
	movzx eax, byte [rdx + FatEntry.raw+r9]
	cmp al, 20h
	je .done
	test byte [rdx + FatEntry.raw+12], 16
	jz .extemit
	cmp eax, 'A'
	jb .extemit
	cmp eax, 'Z'
	ja .extemit
	add eax, 32
.extemit:
	mov ax, word [r8+rax*2]
	stosw
	inc r9d
	cmp r9d, 11
	jb .ext
.done:
	mov word [rdi], 0
	lea rax, [rdx + FatEntry.name]
	sub rdi, rax
	shr edi, 1
	mov dword [rdx + FatEntry.name_length], edi
	ret
endp

; dir_next(identity, cursor, entry). F_END is distinct from I/O/corruption.
; RCX=identity*, RDX=FatCursor* in-out, R8=FatEntry* out -> EAX=status.
; F_OK yields an entry; F_END means exhausted. Output/cursor can be partially
; changed on error. Caller must not use output unless F_OK; see fat32.inc.
proc fat_dir_next uses rbx rsi rdi r12 r13 r14
	mov rbx, rcx
	mov rsi, rdx
	mov r12, r8
.scan:
	fastcall f_cursor_sector, rbx, rsi
	test eax, eax
	jnz .done
	mov edi, dword [rsi + FatCursor.offset]
	add rdi, rbx
	add rdi, FatIdentity.directory
	add dword [rsi + FatCursor.offset], 32
	inc qword [rsi + FatCursor.index]
	movzx eax, byte [rdi]
	test eax, eax
	jz .end
	cmp eax, 0E5h
	je .reset
	movzx edx, byte [rdi+11]
	and edx, 3Fh
	cmp edx, 0Fh
	je .long
	test edx, 8
	jnz .reset
	; Snapshot the complete short entry and its exact location.
	mov rax, qword [rbx + FatIdentity.generation]
	mov qword [r12 + FatEntry.generation], rax
	mov rax, qword [rbx + FatIdentity.dir_lba]
	mov qword [r12 + FatEntry.sector], rax
	mov rax, qword [rsi + FatCursor.index]
	dec rax
	mov qword [r12 + FatEntry.index], rax
	mov eax, dword [rsi + FatCursor.parent]
	mov dword [r12 + FatEntry.parent], eax
	mov eax, dword [rsi + FatCursor.offset]
	sub eax, 32
	mov dword [r12 + FatEntry.offset], eax
	movzx eax, word [rdi+20]
	shl eax, 16
	mov ax, word [rdi+26]
	and eax, F_MASK
	mov dword [r12 + FatEntry.cluster], eax
	; Nonempty files and ordinary directories need a data-cluster ID. Zero
	; is valid for empty files and for a root parent in the dot-dot entry.
	test eax, eax
	jz .zero_cluster
	cmp eax, 2
	jb .corrupt
	mov ecx, dword [rbx + FatIdentity.cluster_count]
	inc ecx
	cmp eax, ecx
	ja .corrupt
	jmp .cluster_ok
.zero_cluster:
	cmp dword [rdi+28], 0
	jne .corrupt
	test byte [rdi+11], 10h
	jz .cluster_ok
	cmp word [rdi], 2E2Eh
	jne .corrupt
.cluster_ok:
	mov eax, dword [rdi+28]
	mov dword [r12 + FatEntry.size], eax
	iterate n, 0,8,16,24
		mov rax, qword [rdi+n]
		mov qword [r12 + FatEntry.raw+n], rax
	end iterate
	mov dword [r12 + FatEntry.lfn_count], 0
	cmp dword [rsi + FatCursor.lfn_next], 0
	jne .short
	cmp dword [rsi + FatCursor.lfn_count], 0
	je .short
	fastcall f_checksum, rdi
	cmp eax, [rsi + FatCursor.checksum]
	jne .short
	; A terminator may be absent when length is a multiple of 13.
	mov ecx, dword [rsi + FatCursor.lfn_count]
	imul ecx, 13
	xor r13d, r13d
	xor r14d, r14d
.namecheck:
	movzx eax, word [rsi + FatCursor.name+r13*2]
	test r14d, r14d
	jnz .padding
	test eax, eax
	jz .terminate
	cmp eax, 0FFFFh
	je .short
	cmp r13d, 255
	jae .short
	mov word [r12 + FatEntry.name+r13*2], ax
	jmp .nextchar
.terminate:
	mov r14d, r13d
	inc r14d                 ; length + 1 distinguishes an empty string
	jmp .nextchar
.padding:
	cmp eax, 0FFFFh
	jne .short
.nextchar:
	inc r13d
	cmp r13d, ecx
	jb .namecheck
	test r14d, r14d
	jz .length
	lea r13d, [r14d-1]
.length:
	test r13d, r13d
	jz .short
	cmp r13d, 255
	ja .short
	mov word [r12 + FatEntry.name+r13*2], 0
	mov dword [r12 + FatEntry.name_length], r13d
	fastcall f_name_check, addr r12 + FatEntry.name
	test eax, eax
	jnz .short
	mov eax, dword [rsi + FatCursor.lfn_count]
	mov dword [r12 + FatEntry.lfn_count], eax
	jmp .entry
.short:
	fastcall f_short_name, rbx, r12
.entry:
	mov dword [rsi + FatCursor.lfn_count], 0
	mov dword [rsi + FatCursor.lfn_next], 0
	xor eax, eax
.done:
	ret
.long:
	cmp byte [rdi+12], 0
	jne .reset
	cmp word [rdi+26], 0
	jne .reset
	test eax, 80h
	jnz .reset
	test eax, 40h
	jz .continuation
	and eax, 3Fh
	cmp eax, 1
	jb .reset
	cmp eax, 20
	ja .reset
	mov dword [rsi + FatCursor.lfn_count], eax
	mov dword [rsi + FatCursor.lfn_next], eax
	movzx edx, byte [rdi+13]
	mov dword [rsi + FatCursor.checksum], edx
.continuation:
	and eax, 3Fh
	cmp eax, [rsi + FatCursor.lfn_next]
	jne .reset
	test eax, eax
	jz .reset
	movzx edx, byte [rdi+13]
	cmp edx, [rsi + FatCursor.checksum]
	jne .reset
	dec eax
	imul eax, 26
	lea r13, [rsi + FatCursor.name+rax]
	lea r14, [f_lfn_offsets]
	xor ecx, ecx
.fragment:
	movzx edx, byte [r14+rcx]
	mov ax, word [rdi+rdx]
	mov word [r13+rcx*2], ax
	inc ecx
	cmp ecx, 13
	jb .fragment
	dec dword [rsi + FatCursor.lfn_next]
	jmp .scan
.reset:
	mov dword [rsi + FatCursor.lfn_count], 0
	mov dword [rsi + FatCursor.lfn_next], 0
	jmp .scan
.end:
	mov dword [rsi + FatCursor.ended], 1
	mov eax, F_END
	jmp .done
.corrupt:
	mov eax, F_CORRUPT
	jmp .done
endp

; ASCII-insensitive, other UTF-16 code units exact. Does not depend on a host
; locale or silently apply an inappropriate Unicode/OEM case table.
; RCX/RDX=const NUL-terminated UTF-16 names -> EAX=1 equal, 0 different.
; Private leaf; pointers advance, R8/flags clobbered. No F_* status return.
proc f_name_equal
.loop:
	movzx eax, word [rcx]
	movzx r8d, word [rdx]
	cmp eax, 'A'
	jb .a
	cmp eax, 'Z'
	ja .a
	add eax, 32
.a:
	cmp r8d, 'A'
	jb .b
	cmp r8d, 'Z'
	ja .b
	add r8d, 32
.b:
	cmp eax, r8d
	jne .no
	test eax, eax
	jz .yes
	add rcx, 2
	add rdx, 2
	jmp .loop
.yes:
	mov eax, 1
	ret
.no:
	xor eax, eax
	ret
endp

; RCX=identity*, EDX=parent cluster, R8=const UTF-16 component, R9=entry* out.
; EAX=status/F_NOTFOUND. Search LFN then SFN alias. Output is only valid on F_OK;
; failed searches can leave an intermediate entry. RSI/RDI retain name/output.
proc fat_lookup uses rbx rsi rdi
	locals
		cursor FatCursor
		alias FatEntry
	endl
body:
	mov rbx, rcx
	mov rsi, r8
	mov rdi, r9
	fastcall fat_dir_open, rcx, edx, addr cursor
	test eax, eax
	jnz .done
.loop:
	fastcall fat_dir_next, rbx, addr cursor, rdi
	cmp eax, F_END
	je .missing
	test eax, eax
	jnz .done
	fastcall f_name_equal, rsi, addr rdi + FatEntry.name
	test eax, eax
	jnz .found
	cmp dword [rdi + FatEntry.lfn_count], 0
	je .loop
	iterate n, 0,8,16,24
		mov rax, qword [rdi + FatEntry.raw+n]
		mov qword [alias.raw+n], rax
	end iterate
	fastcall f_short_name, rbx, addr alias
	fastcall f_name_equal, rsi, addr alias.name
	test eax, eax
	jz .loop
.found:
	xor eax, eax
.done:
	ret
.missing:
	mov eax, F_NOTFOUND
	jmp .done
endp

; Locate an arbitrary slot in a prevalidated directory chain.
; slot(identity, parent, index, out {sector qword,offset dword}).
; RCX=identity*, EDX=parent first cluster, R8=uint64 logical slot index,
; R9=12-byte {uint64 sector; uint32 offset;}* out -> EAX=status.
; Caller prevalidates directory chain. Output may be partial on error; on F_OK
; offset is 32-byte aligned and sector identifies that on-disk slot.
proc f_slot uses rbx rsi rdi r12
	locals
		next dd ?
	endl
body:
	mov rbx, rcx
	mov esi, edx
	mov rdi, r9
	mov eax, dword [rbx + FatIdentity.cluster_count]
	mov ecx, dword [rbx + FatIdentity.cluster_bytes]
	imul rax, rcx
	shr rax, 5
	cmp r8, rax
	jae .bad
	mov rax, r8
	shl rax, 5
	jc .bad
	xor edx, edx
	mov ecx, dword [rbx + FatIdentity.cluster_bytes]
	div rcx
	mov r12, rax
	mov eax, edx
	xor edx, edx
	div dword [rbx + FatIdentity.sector_bytes]
	mov dword [rdi+8], edx
	mov qword [rdi], rax
	cmp r12d, [rbx + FatIdentity.cluster_count]
	jae .bad
.walk:
	test r12, r12
	jz .sector
	fastcall fat_get, rbx, esi, addr next
	test eax, eax
	jnz .done
	mov esi, dword [next]
	cmp esi, 2
	jb .bad
	mov eax, dword [rbx + FatIdentity.cluster_count]
	inc eax
	cmp esi, eax
	ja .bad
	dec r12
	jmp .walk
.sector:
	fastcall f_cluster_sector, rbx, esi
	add [rdi], rax
	xor eax, eax
.done:
	ret
.bad:
	mov eax, F_CORRUPT
	jmp .done
endp

section '.rdata' data readable align 8
f_lfn_offsets db 1,3,5,7,9,14,16,18,20,22,24,28,30
f_cp437:
	repeat 128, n:0
		dw n
	end repeat
	dw 00C7h,00FCh,00E9h,00E2h,00E4h,00E0h,00E5h,00E7h,00EAh,00EBh,00E8h,00EFh,00EEh,00ECh,00C4h,00C5h
	dw 00C9h,00E6h,00C6h,00F4h,00F6h,00F2h,00FBh,00F9h,00FFh,00D6h,00DCh,00A2h,00A3h,00A5h,020A7h,0192h
	dw 00E1h,00EDh,00F3h,00FAh,00F1h,00D1h,00AAh,00BAh,00BFh,02310h,00ACh,00BDh,00BCh,00A1h,00ABh,00BBh
	dw 02591h,02592h,02593h,02502h,02524h,02561h,02562h,02556h,02555h,02563h,02551h,02557h,0255Dh,0255Ch,0255Bh,02510h
	dw 02514h,02534h,0252Ch,0251Ch,02500h,0253Ch,0255Eh,0255Fh,0255Ah,02554h,02569h,02566h,02560h,02550h,0256Ch,02567h
	dw 02568h,02564h,02565h,02559h,02558h,02552h,02553h,0256Bh,0256Ah,02518h,0250Ch,02588h,02584h,0258Ch,02590h,02580h
	dw 003B1h,000DFh,00393h,003C0h,003A3h,003C3h,000B5h,003C4h,003A6h,00398h,003A9h,003B4h,0221Eh,003C6h,003B5h,02229h
	dw 02261h,000B1h,02265h,02264h,02320h,02321h,000F7h,02248h,000B0h,02219h,000B7h,0221Ah,0207Fh,000B2h,025A0h,000A0h
section '.text' code readable executable align 16
