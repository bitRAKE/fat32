; Optional logical-directory diagnostics. No recursion, mutation, or full-chain
; preflight. Stop at the first unused entry; a separate chain check covers tail
; allocation. The caller holds its ordinary volume lease throughout.
section '.text$fat_check_directory' code readable executable comdat any align 16
proc fat_check_directory uses rbx rsi rdi r12 r13 rbp, checkpoint:4, power:4, distance:4, steps:4, kind:4, label_seen:4
    locals
        cursor FatCursor
    endl
    assert label_seen+4-(parmbase@proc) <= 32
body:
    mov rsi, r8
    mov r12d, edx
    fastcall f_handle, rcx
    test eax, eax
    jnz .invalid
    mov r13, r11
    mov eax, F_ARGUMENT
    test dword [rcx + FatHandle.access], FH_READ
    jz .invalid
    movzx edx, byte [r13 + FatRecord.raw+11]
    and edx, 18h
    cmp edx, 10h
    jne .invalid
    mov rbx, [r10 + FatVolume.identity]
    fastcall f_check_init, rbx, rsi, FC_DIRECTORY
    test eax, eax
    jnz .done
    mov [rsi + FatCheck.budget], r12d
    mov edx, [r13 + FatRecord.cluster]
    mov [rsi + FatCheck.first], edx
    mov [checkpoint], edx
    mov dword [power], 1
    mov dword [distance], 0
    mov dword [steps], 0
    mov dword [label_seen], 0
    lea rbp, [cursor]
    fastcall fat_dir_open, rbx, edx, rbp
    test eax, eax
    jnz .link
.slot:
    mov eax, [rbp + FatCursor.cluster]
    mov [rsi + FatCheck.cluster], eax
    mov qword [rsi + FatCheck.sector], -1
    mov eax, [rsi + FatCheck.examined]
    cmp eax, r12d
    jae .limit
    fastcall f_cursor_sector, rbx, rbp
    cmp eax, F_END
    je .end
    test eax, eax
    jnz .read_error
    mov rax, [rbx + FatIdentity.dir_lba]
    mov [rsi + FatCheck.sector], rax
    mov eax, [rbp + FatCursor.cluster]
    mov [rsi + FatCheck.cluster], eax
    mov edx, [rbp + FatCursor.steps]
    cmp edx, [steps]
    je .loaded
    mov [steps], edx
    inc dword [distance]
    cmp eax, [checkpoint]
    je .cycle
    mov edx, [distance]
    cmp edx, [power]
    jne .loaded
    mov [checkpoint], eax
    shl dword [power], 1
    mov dword [distance], 0
.loaded:
    mov edi, [rbp + FatCursor.offset]
    add rdi, [rbx + FatIdentity.directory]
    add dword [rbp + FatCursor.offset], 32
    inc qword [rbp + FatCursor.index]
    inc dword [rsi + FatCheck.examined]
    cmp qword [r13 + FatObject.parent], 0
    je .ordinary
    cmp dword [rsi + FatCheck.examined], 2
    jbe .dot
.ordinary:
    movzx eax, byte [rdi]
    mov [rsi + FatCheck.observed], eax
    test eax, eax
    jz .marker
    cmp eax, 0E5h
    je .deleted
    cmp eax, '.'
    je .dot_bad
    movzx eax, byte [rdi+11]
    and eax, 3Fh                 ; reserved high attribute bits are ignored
    cmp eax, 0Fh
    je .long
    and eax, 18h
    mov [kind], eax
    cmp eax, 18h
    je .entry_bad
    movzx eax, word [rdi+20]
    shl eax, 16
    mov ax, [rdi+26]
    and eax, F_MASK
    mov [rsi + FatCheck.observed], eax
    cmp dword [kind], 8
    je .label
    cmp byte [rdi], ' '
    je .entry_bad
    test eax, eax
    jnz .cluster
    cmp dword [kind], 10h
    je .entry_bad
    cmp dword [rdi+28], 0
    jne .entry_bad
    jmp .name
.cluster:
    sub eax, 2
    cmp eax, [rbx + FatIdentity.cluster_count]
    jae .entry_bad
    cmp dword [kind], 10h
    jne .name
    cmp dword [rdi+28], 0
    jne .entry_bad
.name:
    cmp dword [rbp + FatCursor.lfn_count], 0
    je .accepted
    cmp dword [rbp + FatCursor.lfn_next], 0
    jne .lfn_bad
    fastcall f_checksum, rdi
    cmp eax, [rbp + FatCursor.checksum]
    jne .lfn_bad
    mov ecx, [rbp + FatCursor.lfn_count]
    imul ecx, 13
    xor edx, edx
    xor r8d, r8d               ; zero or terminator offset + 1
.character:
    movzx eax, word [rbp + FatCursor.name+rdx*2]
    test r8d, r8d
    jnz .padding
    test eax, eax
    jz .terminator
    cmp eax, 0FFFFh
    je .lfn_bad
    cmp edx, 255
    jae .lfn_bad
    jmp .next_character
.terminator:
    lea r8d, [rdx+1]
    jmp .next_character
.padding:
    cmp eax, 0FFFFh
    jne .lfn_bad
.next_character:
    inc edx
    cmp edx, ecx
    jb .character
    test r8d, r8d
    jz .length
    lea edx, [r8-1]
.length:
    test edx, edx
    jz .lfn_bad
    cmp edx, 255
    ja .lfn_bad
    mov word [rbp + FatCursor.name+rdx*2], 0
    fastcall f_name_check, addr rbp + FatCursor.name
    test eax, eax
    jnz .lfn_bad
.accepted:
    inc dword [rsi + FatCheck.count]
    mov dword [rbp + FatCursor.lfn_count], 0
    mov dword [rbp + FatCursor.lfn_next], 0
    jmp .slot
.label:
    test eax, eax
    jnz .entry_bad
    cmp dword [rdi+28], 0
    jne .entry_bad
    cmp qword [r13 + FatObject.parent], 0
    jne .entry_bad
    cmp dword [label_seen], 0
    jne .entry_bad
    cmp dword [rbp + FatCursor.lfn_count], 0
    jne .lfn_bad
    mov dword [label_seen], 1
    jmp .accepted
.dot:
    mov ecx, [rsi + FatCheck.examined] ; 1 for dot, 2 for dotdot
    xor edx, edx
.dots:
    cmp byte [rdi+rdx], '.'
    jne .dot_bad
    inc edx
    cmp edx, ecx
    jb .dots
.dot_padding:
    cmp byte [rdi+rdx], ' '
    jne .dot_bad
    inc edx
    cmp edx, 11
    jb .dot_padding
    movzx eax, byte [rdi+11]
    and eax, 18h
    cmp eax, 10h
    jne .dot_bad
    cmp dword [rdi+28], 0
    jne .dot_bad
    mov edx, [rsi + FatCheck.first]
    cmp ecx, 1
    je .dot_cluster
    mov rax, [r13 + FatObject.parent]
    mov edx, [rax + FatRecord.cluster]
    cmp edx, [rbx + FatIdentity.root_cluster]
    jne .dot_cluster
    xor edx, edx
.dot_cluster:
    mov [rsi + FatCheck.expected], edx
    movzx eax, word [rdi+20]
    shl eax, 16
    mov ax, [rdi+26]
    and eax, F_MASK
    mov [rsi + FatCheck.observed], eax
    cmp eax, edx
    jne .dot_bad
    jmp .accepted
.long:
    movzx eax, byte [rdi+12]
    test eax, eax
    jnz .extension
    movzx eax, word [rdi+26]
    test eax, eax
    jnz .extension
    movzx eax, byte [rdi]
    test eax, 0A0h
    jnz .lfn_bad
    test eax, 40h
    jz .continuation
    cmp dword [rbp + FatCursor.lfn_count], 0
    jne .lfn_bad
    and eax, 3Fh
    cmp eax, 1
    jb .lfn_bad
    cmp eax, 20
    ja .lfn_bad
    mov [rbp + FatCursor.lfn_count], eax
    mov [rbp + FatCursor.lfn_next], eax
    movzx edx, byte [rdi+13]
    mov [rbp + FatCursor.checksum], edx
.continuation:
    and eax, 3Fh
    test eax, eax
    jz .lfn_bad
    cmp eax, [rbp + FatCursor.lfn_next]
    jne .lfn_bad
    movzx edx, byte [rdi+13]
    cmp edx, [rbp + FatCursor.checksum]
    jne .lfn_bad
    dec eax
    imul eax, 26
    lea r8, [rbp + FatCursor.name+rax]
    lea r9, [f_lfn_offsets]
    xor ecx, ecx
.fragment:
    movzx edx, byte [r9+rcx]
    mov ax, [rdi+rdx]
    mov [r8+rcx*2], ax
    inc ecx
    cmp ecx, 13
    jb .fragment
    dec dword [rbp + FatCursor.lfn_next]
    jmp .slot
.deleted:
    cmp dword [rbp + FatCursor.lfn_count], 0
    jne .lfn_bad
    jmp .slot
.marker:
    or dword [rsi + FatCheck.flags], FC_ENDMARKER
.end:
    cmp dword [rbp + FatCursor.lfn_count], 0
    jne .lfn_bad
    mov eax, [rbp + FatCursor.cluster]
    mov [rsi + FatCheck.last], eax
    xor eax, eax
.done:
    mov [rsi + FatCheck.status], eax
    ret
.invalid:
    mov ebx, eax
    fastcall f_check_clear, rsi
    mov dword [rsi + FatCheck.scope], FC_DIRECTORY
    xchg eax, ebx
    jmp .done
.read_error:
    cmp eax, F_CORRUPT
    je .link
    mov dword [rsi + FatCheck.issue], FC_IO
    jmp .done
.link:
    mov dword [rsi + FatCheck.issue], FC_LINK
    jmp .corrupt
.cycle:
    mov dword [rsi + FatCheck.issue], FC_CYCLE
    jmp .corrupt
.entry_bad:
    mov dword [rsi + FatCheck.issue], FC_DIRENT
    jmp .corrupt
.dot_bad:
    mov dword [rsi + FatCheck.issue], FC_DOT
    jmp .corrupt
.lfn_bad:
    mov dword [rsi + FatCheck.issue], FC_LFN
.corrupt:
    mov eax, F_CORRUPT
    jmp .done
.extension:
    mov [rsi + FatCheck.observed], eax
    mov dword [rsi + FatCheck.issue], FC_EXTENSION
    mov eax, F_ATTENTION
    jmp .done
.limit:
    mov dword [rsi + FatCheck.issue], FC_BUDGET
    mov eax, F_LIMIT
    jmp .done
endp
