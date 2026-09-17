; Optional allocation ownership diagnosis. No base path references this module.
; A serialized, stable selected-FAT view is required for the complete operation.
struct FOwnerWalk
    ?owner dq ?
    ?first dd ?
    ?required dd ?             ; minimum file clusters, -1 for directory
ends

section '.text$fat_check_ownership' code readable executable comdat any align 16
proc fat_check_ownership uses rbx rsi rdi r12 rbp r15
    locals
        walk FOwnerWalk
        task_index dd ?
        cluster dd ?
        sector_in dd ?
        offset dd ?
        index dd ?
        kind dd ?
        label_seen dd ?
        orphan_first dd ?
        orphan_sector dq ?
    endl
body:
    mov rbx, rcx
    mov rsi, rdx
    mov r9, r8
    mov rdi, r8
    mov ecx, sizeof.FatOwnershipReport/8
    xor eax, eax
    rep stosq
    mov rdi, r9
    mov qword [rdi + FatOwnershipReport.owner_sector], -1
    mov qword [rdi + FatOwnershipReport.other_sector], -1
    fastcall f_check_init, rbx, rdi, FC_OWNERSHIP
    test eax, eax
    jnz .done
    mov eax, [rsi + FatOwnershipCheck.fat_budget]
    mov [rdi + FatCheck.budget], eax
    cmp qword [rsi + FatOwnershipCheck.reserved], 0
    jne .argument
    mov eax, [rsi + FatOwnershipCheck.owner_capacity]
    mov edx, [rbx + FatIdentity.cluster_count]
    cmp eax, edx
    jb .memory
    mov eax, [rsi + FatOwnershipCheck.directory_capacity]
    mov edx, 1
    test eax, eax
    jz .memory
    cmp qword [rsi + FatOwnershipCheck.owners], 0
    je .argument
    cmp qword [rsi + FatOwnershipCheck.directories], 0
    je .argument
    mov eax, [rsi + FatOwnershipCheck.owner_capacity]
    shl rax, 3
    add rax, [rsi + FatOwnershipCheck.owners]
    jc .range
    mov eax, [rsi + FatOwnershipCheck.directory_capacity]
    shl rax, 4
    add rax, [rsi + FatOwnershipCheck.directories]
    jc .range
    cmp dword [rsi + FatOwnershipCheck.fat_budget], 0
    je .fat_limit             ; zero budget does not even clear workspace
    ; f_check_init preserves R9, which still holds the report during clearing.
    mov rdi, [rsi + FatOwnershipCheck.owners]
    mov ecx, [rbx + FatIdentity.cluster_count]
    xor eax, eax
    rep stosq
    mov rdi, r9
    mov r12, [rsi + FatOwnershipCheck.directories]
    mov eax, [rbx + FatIdentity.root_cluster]
    mov [r12 + FatDirectoryTask.first], eax
    mov dword [r12 + FatDirectoryTask.parent], 0
    mov qword [r12 + FatDirectoryTask.owner], -1
    mov qword [walk.owner], -1
    mov [walk.first], eax
    mov dword [walk.required], -1
    mov dword [rdi + FatOwnershipReport.directories], 1
    fastcall f_check_owner_chain, rbx, rsi, rdi, addr walk
    test eax, eax
    jnz .done
    mov dword [task_index], 0
.directory:
    mov eax, [task_index]
    shl rax, 4
    add rax, [rsi + FatOwnershipCheck.directories]
    mov r12, rax
    mov eax, [r12 + FatDirectoryTask.first]
    mov [cluster], eax
    mov dword [sector_in], 0
    mov dword [offset], 0
    mov dword [index], 0
    mov dword [label_seen], 0
.slot:
    fastcall f_check_owner_tag, rdi, qword [r12 + FatDirectoryTask.owner]
    mov eax, [r12 + FatDirectoryTask.first]
    mov [rdi + FatCheck.first], eax
    mov eax, [cluster]
    mov [rdi + FatCheck.cluster], eax
    mov qword [rdi + FatCheck.sector], -1
    mov eax, [rdi + FatOwnershipReport.slots]
    cmp eax, [rsi + FatOwnershipCheck.slot_budget]
    jae .slot_limit
    mov eax, [offset]
    cmp eax, [rbx + FatIdentity.sector_bytes]
    jb .load
    mov dword [offset], 0
    inc dword [sector_in]
    mov eax, [sector_in]
    cmp eax, [rbx + FatIdentity.cluster_sectors]
    jb .load
    mov dword [sector_in], 0
    fastcall f_check_owner_get, rbx, rsi, rdi, [cluster]
    test eax, eax
    jnz .done
    cmp edx, F_EOC
    jae .directory_end
    lea eax, [rdx-2]
    cmp eax, [rbx + FatIdentity.cluster_count]
    jae .link
    mov [cluster], edx
.load:
    fastcall f_cluster_sector, rbx, [cluster]
    mov edx, [sector_in]
    add rdx, rax
    mov [rdi + FatCheck.sector], rdx
    mov eax, [cluster]
    mov [rdi + FatCheck.cluster], eax
    fastcall f_directory, rcx, rdx
    test eax, eax
    jnz .io
    mov ebp, [offset]
    mov [rdi + FatOwnershipReport.owner_offset], ebp
    mov rax, [rbx + FatIdentity.dir_lba]
    mov [rdi + FatOwnershipReport.owner_sector], rax
    add rbp, [rbx + FatIdentity.directory]
    add dword [offset], 32
    inc dword [index]
    inc dword [rdi + FatOwnershipReport.slots]
    cmp qword [r12 + FatDirectoryTask.owner], -1
    je .ordinary
    cmp dword [index], 2
    jbe .dot
.ordinary:
    movzx eax, byte [rbp]
    test eax, eax
    jz .directory_end
    cmp eax, 0E5h
    je .slot
    cmp eax, '.'
    je .dot_bad
    cmp eax, ' '
    je .entry_bad
    movzx eax, byte [rbp+11]
    and eax, 3Fh
    cmp eax, 0Fh
    je .lfn
    and eax, 18h
    mov [kind], eax
    cmp eax, 18h
    je .entry_bad
    movzx eax, word [rbp+20]
    shl eax, 16
    mov ax, [rbp+26]
    and eax, F_MASK
    mov [walk.first], eax
    mov [rdi + FatCheck.observed], eax
    cmp dword [kind], 8
    je .label
    cmp dword [kind], 10h
    jne .file
    test eax, eax
    jz .entry_bad
    cmp dword [rbp+28], 0
    jne .entry_bad
    mov eax, [rdi + FatOwnershipReport.directories]
    mov edx, eax
    inc edx
    cmp eax, [rsi + FatOwnershipCheck.directory_capacity]
    jae .memory
    mov dword [walk.required], -1
    jmp .walk
.file:
    mov eax, [rbp+28]
    mov ecx, [rbx + FatIdentity.cluster_bytes]
    lea rax, [rax+rcx-1]
    xor edx, edx
    div rcx
    mov [walk.required], eax
.walk:
    mov eax, [rdi + FatOwnershipReport.owner_offset]
    shl rax, 32
    mov edx, dword [rdi + FatOwnershipReport.owner_sector]
    or rax, rdx
    mov [walk.owner], rax
    fastcall f_check_owner_chain, rbx, rsi, rdi, addr walk
    test eax, eax
    jnz .done
    cmp dword [kind], 10h
    je .enqueue
    inc dword [rdi + FatOwnershipReport.files]
    jmp .slot
.enqueue:
    mov eax, [rdi + FatOwnershipReport.directories]
    shl rax, 4
    add rax, [rsi + FatOwnershipCheck.directories]
    mov edx, [walk.first]
    mov [rax + FatDirectoryTask.first], edx
    mov edx, [r12 + FatDirectoryTask.first]
    mov [rax + FatDirectoryTask.parent], edx
    mov rdx, [walk.owner]
    mov qword [rax + FatDirectoryTask.owner], rdx
    inc dword [rdi + FatOwnershipReport.directories]
    jmp .slot
.lfn:
    movzx eax, byte [rbp+12]
    test eax, eax
    jnz .extension
    movzx eax, word [rbp+26]
    test eax, eax
    jnz .extension
    jmp .slot                 ; names do not carry allocation ownership
.label:
    test eax, eax
    jnz .entry_bad
    cmp dword [rbp+28], 0
    jne .entry_bad
    cmp qword [r12 + FatDirectoryTask.owner], -1
    jne .entry_bad
    cmp dword [label_seen], 0
    jne .entry_bad
    mov dword [label_seen], 1
    jmp .slot
.dot:
    mov ecx, [index]
    xor edx, edx
.dot_name:
    cmp byte [rbp+rdx], '.'
    jne .dot_bad
    inc edx
    cmp edx, ecx
    jb .dot_name
.dot_pad:
    cmp byte [rbp+rdx], ' '
    jne .dot_bad
    inc edx
    cmp edx, 11
    jb .dot_pad
    movzx eax, byte [rbp+11]
    and eax, 18h
    cmp eax, 10h
    jne .dot_bad
    cmp dword [rbp+28], 0
    jne .dot_bad
    mov edx, [r12 + FatDirectoryTask.first]
    cmp ecx, 1
    je .dot_cluster
    mov edx, [r12 + FatDirectoryTask.parent]
    cmp edx, [rbx + FatIdentity.root_cluster]
    jne .dot_cluster
    xor edx, edx
.dot_cluster:
    mov [rdi + FatCheck.expected], edx
    movzx eax, word [rbp+20]
    shl eax, 16
    mov ax, [rbp+26]
    and eax, F_MASK
    mov [rdi + FatCheck.observed], eax
    cmp eax, edx
    jne .dot_bad
    jmp .slot
.directory_end:
    inc dword [task_index]
    mov eax, [task_index]
    cmp eax, [rdi + FatOwnershipReport.directories]
    jb .directory
    fastcall f_check_owner_tag, rdi, -1
    mov eax, [rbx + FatIdentity.root_cluster]
    mov [rdi + FatCheck.first], eax
    mov dword [rdi + FatCheck.last], 0
    mov dword [orphan_first], 0
    mov r15d, 2
.inventory:
    lea eax, [r15-2]
    mov r12, [rsi + FatOwnershipCheck.owners]
    cmp qword [r12+rax*8], 0
    jne .next_cluster
    fastcall f_check_owner_get, rbx, rsi, rdi, r15d
    test eax, eax
    jnz .done
    test edx, edx
    jz .free
    cmp edx, 0FFFFFF7h
    je .bad_cluster
    cmp edx, F_EOC
    jae .orphan
    lea eax, [rdx-2]
    cmp eax, [rbx + FatIdentity.cluster_count]
    jae .link
.orphan:
    cmp dword [orphan_first], 0
    jne .more_orphans
    mov [orphan_first], r15d
    mov rax, [rdi + FatCheck.sector]
    mov [orphan_sector], rax
.more_orphans:
    inc dword [rdi + FatOwnershipReport.orphan_clusters]
    jmp .next_cluster
.free:
    inc dword [rdi + FatOwnershipReport.free_clusters]
    jmp .next_cluster
.bad_cluster:
    inc dword [rdi + FatOwnershipReport.bad_clusters]
.next_cluster:
    inc r15d
    mov eax, [rbx + FatIdentity.cluster_count]
    inc eax
    cmp r15d, eax
    jbe .inventory
    mov [rdi + FatCheck.last], eax
    mov eax, [rbx + FatIdentity.root_cluster]
    mov [rdi + FatCheck.first], eax
    mov dword [rdi + FatCheck.expected], 0
    mov eax, [rdi + FatOwnershipReport.orphan_clusters]
    mov [rdi + FatCheck.observed], eax
    test eax, eax
    jnz .orphans
    mov dword [rdi + FatCheck.cluster], 0
    mov qword [rdi + FatCheck.sector], -1
    xor eax, eax
.done:
    mov [rdi + FatCheck.status], eax
    ret
.argument:
    mov eax, F_ARGUMENT
    jmp .done
.range:
    mov eax, F_RANGE
    jmp .done
.memory:
    mov [rdi + FatCheck.observed], eax
    mov [rdi + FatCheck.expected], edx
    mov dword [rdi + FatCheck.issue], FC_WORKSPACE
    mov eax, F_MEMORY
    jmp .done
.fat_limit:
    mov dword [rdi + FatCheck.issue], FC_BUDGET
    mov eax, F_LIMIT
    jmp .done
.slot_limit:
    mov [rdi + FatCheck.observed], eax
    mov eax, [rsi + FatOwnershipCheck.slot_budget]
    mov [rdi + FatCheck.expected], eax
    mov dword [rdi + FatCheck.issue], FC_SLOT_BUDGET
    mov eax, F_LIMIT
    jmp .done
.io:
    mov dword [rdi + FatCheck.issue], FC_IO
    jmp .done
.link:
    mov dword [rdi + FatCheck.issue], FC_LINK
    jmp .corrupt
.entry_bad:
    mov dword [rdi + FatCheck.issue], FC_DIRENT
    jmp .corrupt
.dot_bad:
    mov dword [rdi + FatCheck.issue], FC_DOT
    jmp .corrupt
.extension:
    mov [rdi + FatCheck.observed], eax
    mov dword [rdi + FatCheck.issue], FC_EXTENSION
    mov eax, F_ATTENTION
    jmp .done
.orphans:
    or dword [rdi + FatCheck.flags], FC_ORPHANS
    mov dword [rdi + FatCheck.issue], FC_ORPHAN
    mov eax, [orphan_first]
    mov [rdi + FatCheck.cluster], eax
    mov rax, [orphan_sector]
    mov [rdi + FatCheck.sector], rax
.corrupt:
    mov eax, F_CORRUPT
    jmp .done
endp

; RCX=report*, RDX=owner tag. Private leaf; no status return.
section '.text$f_check_owner_tag' code readable executable comdat align 16
proc f_check_owner_tag
    mov qword [rcx + FatOwnershipReport.other_sector], -1
    mov dword [rcx + FatOwnershipReport.other_offset], 0
    cmp rdx, -1
    je .root
    mov eax, edx
    mov [rcx + FatOwnershipReport.owner_sector], rax
    shr rdx, 32
    mov [rcx + FatOwnershipReport.owner_offset], edx
    ret
.root:
    mov qword [rcx + FatOwnershipReport.owner_sector], -1
    mov dword [rcx + FatOwnershipReport.owner_offset], 0
    ret
endp

; RCX=id*, RDX=request*, R8=report*, R9D=data cluster. EAX=status, EDX=value
; only on success. Count attempted FAT accesses, even cached hits or I/O errors.
; Only the report pointer is live across fat_get; other inputs die at dispatch.
section '.text$f_check_owner_get' code readable executable comdat align 16
proc f_check_owner_get uses rdi
    locals
        value dd ?
    endl
body:
    mov r10, rcx
    mov rdi, r8
    mov [rdi + FatCheck.cluster], r9d
    mov qword [rdi + FatCheck.sector], -1
    mov dword [rdi + FatCheck.observed], 0
    mov dword [rdi + FatCheck.expected], 0
    mov eax, [rdi + FatCheck.examined]
    cmp eax, [rdx + FatOwnershipCheck.fat_budget]
    jae .limit
    mov eax, r9d
    shl rax, 2
    xor edx, edx
    mov ecx, [r10 + FatIdentity.sector_bytes]
    div rcx
    mov edx, [r10 + FatIdentity.active_fat]
    imul edx, [r10 + FatIdentity.fat_sectors]
    add edx, [r10 + FatIdentity.fat_start]
    add rax, rdx
    mov [rdi + FatCheck.sector], rax
    inc dword [rdi + FatCheck.examined]
    fastcall fat_get, r10, r9d, addr value
    test eax, eax
    jnz .io
    mov edx, [value]
    mov [rdi + FatCheck.observed], edx
.done:
    ret
.limit:
    mov dword [rdi + FatCheck.issue], FC_BUDGET
    mov eax, F_LIMIT
    jmp .done
.io:
    mov dword [rdi + FatCheck.issue], FC_IO
    jmp .done
endp

; RCX=id*, RDX=request*, R8=report*, R9=FOwnerWalk*. Claim one complete chain.
; Same-owner revisit is a cycle; different-owner revisit is a cross-link.
section '.text$f_check_owner_chain' code readable executable comdat align 16
proc f_check_owner_chain uses rbx rsi rdi r12 rbp r14 r15
    mov rbx, rcx
    mov rsi, rdx
    mov rdi, r8
    mov r12, r9
    mov ebp, [r9 + FOwnerWalk.first]
    mov r15, [r9 + FOwnerWalk.owner]
    xor r14d, r14d
    fastcall f_check_owner_tag, r8, r15
    mov [rdi + FatCheck.first], ebp
    mov [rdi + FatCheck.cluster], ebp
    mov qword [rdi + FatCheck.sector], -1
    mov dword [rdi + FatCheck.last], 0
    test ebp, ebp
    jz .end
.cluster:
    mov [rdi + FatCheck.cluster], ebp
    mov qword [rdi + FatCheck.sector], -1
    lea eax, [rbp-2]
    cmp eax, [rbx + FatIdentity.cluster_count]
    jae .link
    mov r8, [rsi + FatOwnershipCheck.owners]
    mov rdx, [r8+rax*8]
    test rdx, rdx
    jnz .claimed
    fastcall f_check_owner_get, rbx, rsi, rdi, ebp
    test eax, eax
    jnz .done
    cmp edx, F_EOC
    jae .claim
    lea eax, [rdx-2]
    cmp eax, [rbx + FatIdentity.cluster_count]
    jae .link
.claim:
    lea eax, [rbp-2]
    mov r8, [rsi + FatOwnershipCheck.owners]
    mov [r8+rax*8], r15
    inc r14d
    inc dword [rdi + FatCheck.count]
    mov [rdi + FatCheck.last], ebp
    cmp edx, F_EOC
    jae .end
    mov ebp, edx
    jmp .cluster
.end:
    mov eax, [r12 + FOwnerWalk.required]
    cmp eax, -1
    je .ok
    mov [rdi + FatCheck.expected], eax
    mov [rdi + FatCheck.observed], r14d
    cmp r14d, eax
    jb .short
    je .ok
    or dword [rdi + FatCheck.flags], FC_EXTRA
.ok:
    xor eax, eax
.done:
    ret
.claimed:
    mov dword [rdi + FatCheck.issue], FC_CYCLE
    cmp rdx, r15
    je .corrupt
    mov dword [rdi + FatCheck.issue], FC_CROSSLINK
    cmp rdx, -1
    je .corrupt
    mov eax, edx
    mov [rdi + FatOwnershipReport.other_sector], rax
    shr rdx, 32
    mov [rdi + FatOwnershipReport.other_offset], edx
    jmp .corrupt
.link:
    mov dword [rdi + FatCheck.issue], FC_LINK
    jmp .corrupt
.short:
    mov dword [rdi + FatCheck.issue], FC_SHORT
.corrupt:
    mov eax, F_CORRUPT
    jmp .done
endp
