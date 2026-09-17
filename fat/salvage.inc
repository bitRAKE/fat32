; Caller selects the FAT (possibly a FatView), first cluster and byte limit.
; Only a unique prefix with allocated/legal FAT entries is admitted. No writes,
; retries, authority choice, directory lookup or ownership claim. See docs/RECOVERY.md.
section '.text$fat_salvage_plan' code readable executable comdat any align 16
proc fat_salvage_plan uses rbx rdi r12 rbp r14 r15
    locals
        next dd ?
    endl
body:
    mov rbx, rcx
    mov r12, rdx
    mov rbp, r8
    lea rdi, [rbp + sizeof.FatCheck]
    xor eax, eax
    mov ecx, (sizeof.FatSalvage-sizeof.FatCheck)/8
    rep stosq
    fastcall f_check_init, rbx, rbp, FC_SALVAGE
    test eax, eax
    jnz .done
    mov eax, F_ARGUMENT
    cmp dword [r12 + FatSalvageRequest.reserved], 0
    jne .done
    mov r14, [r12 + FatSalvageRequest.extents]
    mov ecx, [r12 + FatSalvageRequest.capacity]
    test ecx, ecx
    jz .storage
    test r14, r14
    jz .done
    lea rdx, [rcx*8]
    add rdx, r14
    jc .done
.storage:
    mov [rbp + FatSalvage.extents], r14
    mov [rbp + FatSalvage.capacity], ecx
    mov eax, [r12 + FatSalvageRequest.bytes]
    mov [rbp + FatSalvage.bytes], eax
    mov eax, [r12 + FatSalvageRequest.fat_budget]
    mov [rbp + FatCheck.budget], eax
    mov eax, [r12 + FatSalvageRequest.pair_budget]
    mov [rbp + FatSalvage.pair_budget], eax
    mov r15d, [r12 + FatSalvageRequest.first]
    mov [rbp + FatCheck.first], r15d
    cmp dword [rbp + FatSalvage.bytes], 0
    je .ok
    test r15d, r15d
    jz .short
.loop:
    mov [rbp + FatCheck.cluster], r15d
    mov qword [rbp + FatCheck.sector], -1
    lea eax, [r15-2]
    cmp eax, [rbx + FatIdentity.cluster_count]
    jae .link
    xor edi, edi
.unique:
    cmp edi, [rbp + FatSalvage.extent_count]
    jae .room
    mov eax, [rbp + FatSalvage.pairs]
    cmp eax, [rbp + FatSalvage.pair_budget]
    jae .pairs
    inc dword [rbp + FatSalvage.pairs]
    mov eax, r15d
    sub eax, [r14 + rdi*8 + FatExtent.first]
    cmp eax, [r14 + rdi*8 + FatExtent.count]
    jb .cycle
    inc edi
    jmp .unique
.room:
    ; A full extent array can still extend the last contiguous run.
    test edi, edi
    jz .new
    lea r8d, [rdi-1]
    mov eax, [r14 + r8*8 + FatExtent.first]
    add eax, [r14 + r8*8 + FatExtent.count]
    cmp eax, r15d
    je .budget
.new:
    cmp edi, [rbp + FatSalvage.capacity]
    jae .memory
.budget:
    mov eax, [rbp + FatCheck.examined]
    cmp eax, [rbp + FatCheck.budget]
    jae .limit
    inc dword [rbp + FatCheck.examined]
    mov eax, r15d
    shl rax, 2
    xor edx, edx
    mov ecx, [rbx + FatIdentity.sector_bytes]
    div rcx
    mov edx, [rbx + FatIdentity.active_fat]
    imul edx, [rbx + FatIdentity.fat_sectors]
    add edx, [rbx + FatIdentity.fat_start]
    add rax, rdx
    mov [rbp + FatCheck.sector], rax
    fastcall fat_get, rbx, r15d, addr next
    test eax, eax
    jnz .io
    mov eax, [next]
    mov [rbp + FatCheck.observed], eax
    cmp eax, F_EOC
    jae .admit
    sub eax, 2
    cmp eax, [rbx + FatIdentity.cluster_count]
    jae .link
.admit:
    test edi, edi
    jz .append
    lea r8d, [rdi-1]
    mov eax, [r14 + r8*8 + FatExtent.first]
    add eax, [r14 + r8*8 + FatExtent.count]
    cmp eax, r15d
    jne .append
    inc dword [r14 + r8*8 + FatExtent.count]
    jmp .included
.append:
    mov [r14 + rdi*8 + FatExtent.first], r15d
    mov dword [r14 + rdi*8 + FatExtent.count], 1
    inc dword [rbp + FatSalvage.extent_count]
.included:
    inc dword [rbp + FatCheck.count]
    mov [rbp + FatCheck.last], r15d
    mov eax, [rbp + FatCheck.count]
    mov ecx, [rbx + FatIdentity.cluster_bytes]
    imul rax, rcx
    mov ecx, [rbp + FatSalvage.bytes]
    cmp rax, rcx
    cmova rax, rcx
    mov [rbp + FatSalvage.available], eax
    cmp eax, ecx
    je .complete
    mov r15d, [next]
    cmp r15d, F_EOC
    jb .loop
.short:
    mov dword [rbp + FatCheck.issue], FC_SHORT
    mov eax, F_CORRUPT
    jmp .done
.complete:
    cmp dword [next], F_EOC
    jae .ok
    or dword [rbp + FatCheck.flags], FC_EXTRA
.ok:
    xor eax, eax
.done:
    mov [rbp + FatCheck.status], eax
    ret
.link:
    mov dword [rbp + FatCheck.issue], FC_LINK
    mov eax, F_CORRUPT
    jmp .done
.cycle:
    mov dword [rbp + FatCheck.issue], FC_CYCLE
    mov eax, F_CORRUPT
    jmp .done
.memory:
    mov dword [rbp + FatCheck.issue], FC_WORKSPACE
    mov [rbp + FatCheck.observed], edi
    inc edi
    mov [rbp + FatCheck.expected], edi
    mov eax, F_MEMORY
    jmp .done
.pairs:
    mov dword [rbp + FatCheck.issue], FC_PAIR_BUDGET
    mov eax, F_LIMIT
    jmp .done
.limit:
    mov dword [rbp + FatCheck.issue], FC_BUDGET
    mov eax, F_LIMIT
    jmp .done
.io:
    mov dword [rbp + FatCheck.issue], FC_IO
    jmp .done
endp

; Read only the mapped candidate prefix; no FAT access or padding missing data.
; F_END with a completed prefix distinguishes a request crossing available.
; Plans/extents are immutable under the same stable lease from plan through use.
section '.text$fat_salvage_read' code readable executable comdat any align 16
proc fat_salvage_read uses rbx rsi rdi rbp r13 r14 r15
    locals
        position dq ?
        left dd ?
        take dd ?
        in_sector dd ?
        result dd ?
    endl
body:
    mov rbp, rcx
    mov r13, rdx
    mov dword [r13 + FatTransfer.done], 0
    mov eax, F_STALE
    cmp dword [rbp + FatCheck.scope], FC_SALVAGE
    jne .done
    mov rbx, [rbp + FatCheck.identity]
    cmp dword [rbx + FatIdentity.magic], F_MAGIC
    jne .done
    mov rcx, [rbx + FatIdentity.generation]
    cmp rcx, [rbp + FatCheck.generation]
    jne .done
    mov eax, F_BUSY
    cmp dword [rbx + FatIdentity.transaction], 0
    jne .done
    xor eax, eax
    mov ecx, [r13 + FatTransfer.length]
    test ecx, ecx
    jz .done
    mov eax, F_ARGUMENT
    mov rdx, [r13 + FatTransfer.data]
    test rdx, rdx
    jz .done
    add rdx, rcx
    jc .done
    mov eax, F_RANGE
    mov rdx, [r13 + FatTransfer.offset]
    add rdx, rcx
    jc .done
    mov edx, [rbp + FatSalvage.available]
    sub rdx, [r13 + FatTransfer.offset]
    jbe .end
    mov dword [result], F_OK
    cmp rdx, rcx
    jae .length
    mov ecx, edx
    mov dword [result], F_END
.length:
    mov [left], ecx
    mov rax, [r13 + FatTransfer.offset]
    mov [position], rax
    mov r14, [rbp + FatSalvage.extents]
    mov r15d, [rbp + FatSalvage.extent_count]
.extent:
    test r15d, r15d
    jz .invalid
    mov eax, [r14 + FatExtent.count]
    mov ecx, [rbx + FatIdentity.cluster_bytes]
    imul rax, rcx
    cmp [position], rax
    jb .sector
    sub [position], rax
    add r14, sizeof.FatExtent
    dec r15d
    jmp .extent
.sector:
    mov rax, [position]
    mov ecx, [rbx + FatIdentity.sector_bytes]
    xor edx, edx
    div rcx
    mov [in_sector], edx
    sub ecx, edx
    cmp ecx, [left]
    cmova ecx, [left]
    mov [take], ecx
    mov edx, [r14 + FatExtent.first]
    sub edx, 2
    imul edx, [rbx + FatIdentity.cluster_sectors]
    add edx, [rbx + FatIdentity.data_start]
    add rdx, rax
    fastcall f_read_sector, rbx, rdx, [rbx + FatIdentity.scratch]
    test eax, eax
    jnz .done
    mov esi, [in_sector]
    add rsi, [rbx + FatIdentity.scratch]
    mov edi, [r13 + FatTransfer.done]
    add rdi, [r13 + FatTransfer.data]
    mov ecx, [take]
    rep movsb
    mov eax, [take]
    add [position], rax
    add [r13 + FatTransfer.done], eax
    sub [left], eax
    jnz .extent
    mov eax, [result]
.done:
    ret
.end:
    mov eax, F_END
    jmp .done
.invalid:
    mov eax, F_ARGUMENT
    jmp .done
endp
