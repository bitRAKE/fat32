; Optional namespace diagnosis. The caller holds the volume lease and stable
; provider view across both passes. The first pass validates every logical slot;
; the second reuses ordinary name decoding and its exact lookup comparison.
section '.text$fat_check_names' code readable executable comdat any align 16
proc fat_check_names uses rbx rsi rbp r13 r14 r15
    locals
        cursor FatCursor
        current FatEntry
        alias FatEntry
        short_name dw 13 dup ?
    endl
body:
    mov r13, rcx
    mov rbp, rdx
    mov rsi, r8
    fastcall f_check_clear, r8
    mov dword [rsi + FatCheck.scope], FC_NAMES
    mov eax, F_ARGUMENT
    cmp dword [rbp + FatNameCheck.reserved], 0
    jne .done
    mov edx, [rbp + FatNameCheck.capacity]
    test edx, edx
    jz .structure
    cmp qword [rbp + FatNameCheck.entries], 0
    je .done
    imul rdx, sizeof.FatEntry
    add rdx, [rbp + FatNameCheck.entries]
    jc .range
.structure:
    fastcall fat_check_directory, r13, [rbp + FatNameCheck.slot_budget], rsi
    test eax, eax
    jnz .done                 ; keep structural scope and its diagnostic
    fastcall f_handle, r13
    test eax, eax
    jnz .done
    mov rbx, [r10 + FatVolume.identity]
    mov dword [rsi + FatCheck.scope], FC_NAMES
    mov dword [rsi + FatCheck.examined], 0
    mov dword [rsi + FatCheck.count], 0
    mov dword [rsi + FatCheck.last], 0
    mov dword [rsi + FatCheck.observed], 0
    mov dword [rsi + FatCheck.expected], 0
    mov eax, [rbp + FatNameCheck.pair_budget]
    mov [rsi + FatCheck.budget], eax
    mov qword [rsi + FatCheck.sector], -1
    fastcall fat_dir_open, rbx, [rsi + FatCheck.first], addr cursor
    test eax, eax
    jnz .error
.next:
    mov qword [rsi + FatCheck.sector], -1
    fastcall fat_dir_next, rbx, addr cursor, addr current
    mov edx, [cursor.cluster]
    mov [rsi + FatCheck.cluster], edx
    cmp eax, F_END
    je .ok
    test eax, eax
    jnz .error
    mov rax, [current.sector]
    mov [rsi + FatCheck.sector], rax
    mov eax, [rsi + FatCheck.count]
    cmp eax, [rbp + FatNameCheck.capacity]
    jae .memory
    ; Decode the new entry's alias once. Private helper below decodes each old
    ; alias on demand; no normal mount/handle or mandatory workspace grows.
    cmp dword [current.lfn_count], 0
    je .pairs
    iterate n, 0,8,16,24
        mov rax, qword [current.raw+n]
        mov qword [alias.raw+n], rax
    end iterate
    fastcall f_short_name, rbx, addr alias
    lea r13, [alias.name]
    lea rdx, [short_name]
    mov ecx, 13
.copy_alias:
    mov ax, [r13]
    mov [rdx], ax
    add r13, 2
    add rdx, 2
    dec ecx
    jnz .copy_alias
.pairs:
    mov r14, [rbp + FatNameCheck.entries]
    xor r15d, r15d
.pair:
    cmp r15d, [rsi + FatCheck.count]
    jae .accept
    mov eax, [rsi + FatCheck.examined]
    cmp eax, [rbp + FatNameCheck.pair_budget]
    jae .limit
    inc dword [rsi + FatCheck.examined]
    fastcall f_check_name_pair, rbx, addr current, r14, addr short_name
    test eax, eax
    jnz .duplicate
    inc r15d
    add r14, sizeof.FatEntry
    jmp .pair
.accept:
    ; R14 is the next workspace entry. Entries are temporary observations;
    ; no caller may treat this workspace as a set of mutation permissions.
    lea r13, [current]
    mov rdx, r14
    mov ecx, sizeof.FatEntry/8
.copy_entry:
    mov rax, [r13]
    mov [rdx], rax
    add r13, 8
    add rdx, 8
    dec ecx
    jnz .copy_entry
    inc dword [rsi + FatCheck.count]
    mov eax, [cursor.cluster]
    mov [rsi + FatCheck.last], eax
    jmp .next
.ok:
    mov eax, [cursor.cluster]
    mov [rsi + FatCheck.last], eax
    xor eax, eax
.done:
    mov [rsi + FatCheck.status], eax
    ret
.range:
    mov eax, F_RANGE
    jmp .done
.error:
    mov dword [rsi + FatCheck.issue], FC_IO
    cmp eax, F_CORRUPT
    jne .done
    mov dword [rsi + FatCheck.issue], FC_LINK
    jmp .done
.memory:
    mov dword [rsi + FatCheck.issue], FC_WORKSPACE
    mov edx, [rbp + FatNameCheck.capacity]
    mov [rsi + FatCheck.observed], edx
    inc eax
    mov [rsi + FatCheck.expected], eax
    mov eax, F_MEMORY
    jmp .done
.limit:
    mov dword [rsi + FatCheck.issue], FC_BUDGET
    mov eax, F_LIMIT
    jmp .done
.duplicate:
    mov dword [rsi + FatCheck.issue], FC_DUPLICATE
    mov eax, dword [current.index]
    mov [rsi + FatCheck.observed], eax
    mov eax, dword [r14 + FatEntry.index]
    mov [rsi + FatCheck.expected], eax
    mov eax, F_CORRUPT
    jmp .done
endp

; RCX=id*, RDX=new FatEntry*, R8=previous FatEntry*, R9=new decoded alias*.
; EAX=1 if any lookup name collides, else zero (not an F_* return). The alias
; argument is read only when the new entry has an LFN. Up to four comparisons;
; same-entry LFN/alias equality is permitted and is never compared here.
section '.text$f_check_name_pair' code readable executable comdat align 16
proc f_check_name_pair uses rbx rsi rdi rbp
    locals
        alias FatEntry
    endl
body:
    mov rbx, rcx
    mov rsi, rdx
    mov rdi, r8
    mov rbp, r9
    fastcall f_name_equal, addr rdx + FatEntry.name, addr r8 + FatEntry.name
    test eax, eax
    jnz .done
    cmp dword [rsi + FatEntry.lfn_count], 0
    je .old_alias
    fastcall f_name_equal, rbp, addr rdi + FatEntry.name
    test eax, eax
    jnz .done
.old_alias:
    cmp dword [rdi + FatEntry.lfn_count], 0
    je .done
    iterate n, 0,8,16,24
        mov rax, qword [rdi + FatEntry.raw+n]
        mov qword [alias.raw+n], rax
    end iterate
    fastcall f_short_name, rbx, addr alias
    fastcall f_name_equal, addr rsi + FatEntry.name, addr alias.name
    test eax, eax
    jnz .done
    cmp dword [rsi + FatEntry.lfn_count], 0
    je .done
    fastcall f_name_equal, rbp, addr alias.name
.done:
    ret
endp
