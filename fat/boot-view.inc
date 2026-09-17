; One temporary bootstrap mapping supplies the explicitly selected BPB to the
; ordinary geometry parser. After mount, all reads use their authentic LBAs.
section '.text$f_boot_read' code readable executable comdat align 16
proc f_boot_read
    mov eax, F_RANGE
    test rdx, rdx
    jnz .done
    mov rax, [rcx]
    mov rdx, [rcx+8]
    fastcall [rax + SectorOps.read], [rax + SectorOps.context], rdx, r8
.done:
    ret
endp

; RCX=zeroed/closed FatView*, RDX=source SectorOps*, R8=FatBootSource*,
; R9=separate three-sector workspace. Failed candidates leave output unchanged.
section '.text$fat_view_boot' code readable executable comdat any align 16
proc fat_view_boot uses rbx rsi rdi r12 rbp r14, mapping:8, boot_lba:8
    locals
        candidate FatIdentity
        bootstrap SectorOps
    endl
    assert boot_lba+8-(parmbase@proc) <= 32
body:
    mov rbx, rcx
    mov r12, rdx
    mov rbp, r8
    mov r14, r9
    mov eax, F_BUSY
    cmp dword [rcx + FatIdentity.magic], 0
    jne .done
    mov eax, F_LIMIT
    cmp qword [rcx + FatIdentity.generation], -2
    jae .done
    mov eax, F_ARGUMENT
    cmp dword [r8 + FatBootSource.reserved], 0
    jne .done
    cmp dword [r9 + FatWorkspace.reserved], 0
    jne .done
    mov rdx, [r9 + FatWorkspace.data]
    test rdx, rdx
    jz .done
    mov ecx, [r9 + FatWorkspace.bytes]
    add rdx, rcx
    jc .done
    mov eax, F_RANGE
    mov edx, [r8 + FatBootSource.sector]
    test edx, edx
    jz .done
    cmp edx, 0FFFFh
    ja .done
    cmp rdx, [r12 + SectorOps.sectors]
    jae .done
    mov [boot_lba], rdx
    mov [mapping], r12
    mov eax, F_FORMAT
    cmp qword [r12 + SectorOps.read], 0
    je .done
    ; Temporary identity is never published on error. Its generation starts
    ; from the retired output, so successful reopening cannot revive handles.
    lea rdi, [candidate]
    xor eax, eax
    mov ecx, sizeof.FatIdentity/8
    rep stosq
    mov rax, [rbx + FatIdentity.generation]
    mov [candidate.generation], rax
    mov rsi, r12
    lea rdi, [bootstrap]
    mov ecx, sizeof.SectorOps/8
    rep movsq
    lea rax, [mapping]
    mov [bootstrap.context], rax
    lea rax, [f_boot_read]
    mov [bootstrap.read], rax
    xor eax, eax
    mov [bootstrap.write], rax
    mov [bootstrap.begin], rax
    mov [bootstrap.end], rax
    mov [bootstrap.flush], rax
    mov [bootstrap.reserved], eax
    fastcall fat_mount, addr candidate, addr bootstrap, [rbp + FatBootSource.oem], r14
    test eax, eax
    jnz .done
    ; A backup candidate must belong to its own reserved region. Plausible
    ; geometry does not prove which copy is authoritative or permit writes.
    mov eax, F_FORMAT
    mov ecx, [rbp + FatBootSource.sector]
    cmp ecx, [candidate.fat_start]
    jae .done
    lea rsi, [candidate]
    mov rdi, rbx
    mov ecx, sizeof.FatIdentity/8
    rep movsq
    mov rsi, r12
    lea rdi, [rbx + FatView.provider]
    mov [rbx + FatIdentity.ops], rdi
    mov ecx, sizeof.SectorOps/8
    rep movsq
    xor eax, eax
    mov [rbx + FatView.provider.write], rax
    mov [rbx + FatView.provider.begin], rax
    mov [rbx + FatView.provider.end], rax
    mov [rbx + FatView.provider.flush], rax
    mov [rbx + FatView.provider.reserved], eax
.done:
    ret
endp
