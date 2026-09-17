; Open a temporary read-only interpretation of a mounted, quiescent provider.
; No I/O. All storage is disjoint; caller holds the source lease until close.
; RCX=zeroed/closed FatView*, RDX=source identity*, R8D=FAT copy, R9=workspace*.
section '.text$fat_view_open' code readable executable comdat any align 16
proc fat_view_open uses rbx rsi rdi
    ; R8D/R9 retain copy/workspace through REP; the final call consumes neither.
    mov rbx, rcx
    mov rsi, rdx
    mov eax, F_BUSY
    cmp dword [rbx + FatIdentity.magic], 0
    jne .done
    cmp dword [rsi + FatIdentity.transaction], 0
    jne .done
    mov eax, F_STALE
    cmp dword [rsi + FatIdentity.magic], F_MAGIC
    jne .done
    mov eax, F_RANGE
    cmp r8d, [rsi + FatIdentity.fat_count]
    jae .done
    mov eax, F_LIMIT
    mov r10, [rbx + FatIdentity.generation]
    cmp r10, -2                 ; reserve a generation for close, never wrap
    jae .done
    mov eax, F_ARGUMENT
    cmp dword [r9 + FatWorkspace.reserved], 0
    jne .done
    mov rdi, [r9 + FatWorkspace.data]
    test rdi, rdi
    jz .done
    mov edx, [r9 + FatWorkspace.bytes]
    add rdx, rdi
    jc .done
    mov eax, [rsi + FatIdentity.sector_bytes]
    lea edx, [rax+rax*2]
    mov eax, F_MEMORY
    cmp [r9 + FatWorkspace.bytes], edx
    jb .done
    ; Copy geometry only as an implementation convenience; all mutable state,
    ; caches and provider capabilities below are independent of the source.
    mov rdi, rbx
    mov ecx, sizeof.FatIdentity/8
    rep movsq
    mov [rbx + FatIdentity.generation], r10
    mov dword [rbx + FatIdentity.transaction], 0
    mov dword [rbx + FatIdentity.completions], 0
    mov [rbx + FatIdentity.active_fat], r8d
    mov dword [rbx + FatIdentity.mirrored], 0
    mov rsi, [rbx + FatIdentity.ops]
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
    mov rdi, [r9 + FatWorkspace.data]
    mov eax, [rbx + FatIdentity.sector_bytes]
    mov [rbx + FatIdentity.fat], rdi
    add rdi, rax
    mov [rbx + FatIdentity.directory], rdi
    add rdi, rax
    mov [rbx + FatIdentity.scratch], rdi
    fastcall fat_invalidate, rbx
.done:
    ret
endp

; Retire only the view. Existing handles/reports become stale, no provider I/O.
; The caller drains operations first, and keeps referenced view storage alive.
section '.text$fat_view_close' code readable executable comdat any align 16
proc fat_view_close uses rbx
    mov rbx, rcx
    mov eax, F_STALE
    cmp dword [rbx + FatIdentity.magic], F_MAGIC
    jne .done
    mov eax, F_BUSY
    cmp dword [rbx + FatIdentity.transaction], 0
    jne .done
    ; Explicit cache invalidation may have consumed the reserved generation.
    ; Closing must still retire the view without enabling a later reopen.
    cmp qword [rbx + FatIdentity.generation], -1
    jne .invalidate
    dec qword [rbx + FatIdentity.generation]
.invalidate:
    fastcall fat_invalidate, rcx
    mov dword [rbx + FatIdentity.magic], 0
.done:
    ret
endp
