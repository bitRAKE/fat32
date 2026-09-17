; Direct-backend bounded FAT32 quick format. No mounted identity is consumed.
; Scratch, options, report and backend remain disjoint and stable throughout.

section '.text$fat_format_plan' code readable executable comdat any align 16
proc fat_format_plan uses rsi rdi rbp
    mov r9, rcx
    mov rsi, rdx
    mov rbp, r8
    mov rdi, r8
    mov ecx, sizeof.FatFormatPlan
    xor eax, eax
    rep stosb
    mov eax, F_ARGUMENT
    cmp dword [rsi + FatFormatOptions.reserved], 0
    jne .done
    cmp dword [rsi + FatFormatOptions.reserved_label], 0
    jne .done
    cmp byte [rsi + FatFormatOptions.reserved_label+4], 0
    jne .done
    ; Labels use printable ASCII and spaces, normalized to uppercase on disk.
    ; All-zero is the only accepted NUL-containing encoding (unlabeled).
    xor r10d, r10d
    xor r11d, r11d
    xor ecx, ecx
.label:
    movzx edx, byte [rsi + FatFormatOptions.label+rcx]
    test edx, edx
    jz .zero
    inc r10d
    cmp edx, 32
    jb .name
    cmp edx, 126
    ja .name
    ; FAT label restrictions. This deliberately accepts no OEM interpretation.
    lea rdi, [f_format_invalid_label]
.character:
    movzx eax, byte [rdi]
    test eax, eax
    jz .next
    inc rdi
    cmp edx, eax
    je .name
    jmp .character
.zero:
    inc r11d
.next:
    inc ecx
    cmp ecx, 11
    jb .label
    test r11d, r11d
    jz .geometry
    test r10d, r10d
    jnz .name
.geometry:
    mov eax, F_FORMAT
    mov r10d, [r9 + SectorOps.sector_bytes]
    cmp r10d, 512
    jb .done
    cmp r10d, 4096
    ja .done
    lea edx, [r10-1]
    test r10d, edx
    jnz .done
    mov r11d, [rsi + FatFormatOptions.cluster_bytes]
    cmp r11d, r10d
    jb .done
    cmp r11d, F_MAX_CLUSTER
    ja .done
    lea edx, [r11-1]
    test r11d, edx
    jnz .done
    mov rax, [r9 + SectorOps.sectors]
    mov ecx, 0FFFFFFFFh
    cmp rax, rcx
    ja .range
    cmp rax, 32
    jbe .format
    mov r9d, eax                 ; complete provider extent, BPB uses uint32
    mov eax, r11d
    xor edx, edx
    div r10d
    cmp eax, 128
    ja .format
    mov r8d, eax                 ; sectors/cluster
    ; Smallest FAT with F*(bps/4) >= floor((N-32-2F)/spc)+2.
    ; Equivalent strict inequality: F*(spc*bps/4+2) > N-32+spc.
    mov ecx, r11d
    shr ecx, 2
    add ecx, 2
    mov eax, r9d
    sub rax, 32
    add rax, r8
    xor edx, edx
    div rcx
    inc eax
    mov esi, eax                 ; sectors/FAT
    lea rdi, [rax*2+32]
    cmp rdi, r9
    jae .format
    mov eax, r9d
    sub eax, edi
    xor edx, edx
    div r8d
    cmp eax, 65541               ; FAT32 threshold plus compatibility margin
    jb .format
    cmp eax, 0FFFFFEEh
    ja .format
    mov [rbp + FatFormatPlan.cluster_count], eax
    mov [rbp + FatFormatPlan.sector_bytes], r10d
    mov [rbp + FatFormatPlan.workspace_bytes], r10d
    mov [rbp + FatFormatPlan.cluster_sectors], r8d
    mov [rbp + FatFormatPlan.cluster_bytes], r11d
    mov [rbp + FatFormatPlan.total_sectors], r9d
    mov [rbp + FatFormatPlan.fat_sectors], esi
    mov [rbp + FatFormatPlan.data_start], edi
    mov dword [rbp + FatFormatPlan.fat_start], 32
    mov dword [rbp + FatFormatPlan.root_cluster], 2
    mov dword [rbp + FatFormatPlan.fat_count], 2
    mov dword [rbp + FatFormatPlan.fsinfo], 1
    mov dword [rbp + FatFormatPlan.backup], 6
    lea rax, [rdi+r8+2]          ; reserved/FAT/root plus two invalidation writes
    mov [rbp + FatFormatPlan.writes], rax
    xor eax, eax
.done:
    ret
.name:
    mov eax, F_NAME
    jmp .done
.range:
    mov eax, F_RANGE
    jmp .done
.format:
    mov eax, F_FORMAT
    jmp .done
endp
section '.rdata$f_format_invalid_label' data readable comdat align 1
f_format_invalid_label db '"*+,./:;<=>?[\]|',0

; One private call context, allocated only on the formatter's stack.
struct f_format_state
    ?plan FatFormatPlan
    ?backend dq ?
    ?options dq ?
    ?scratch dq ?
    ?verify_buffer dq ?
    ?report dq ?
    ?verify dq ?
ends

; RCX=state*. Clear one scratch sector; no external calls.
section '.text$f_format_zero' code readable executable comdat align 16
proc f_format_zero uses rdi
    mov rdi, [rcx + f_format_state.scratch]
    mov ecx, [rcx + f_format_state.plan.sector_bytes]
    xor eax, eax
    rep stosb
    ret
endp

; RCX=options*, RDX=11-byte output. EAX=1 iff labeled, 0 otherwise.
section '.text$f_format_label' code readable executable comdat align 16
proc f_format_label
    xor r8d, r8d
    xor r9d, r9d
.byte:
    mov al, [rcx + FatFormatOptions.label+r8]
    test al, al
    jnz .space
    mov al, 32
.space:
    cmp al, 32
    je .store
    mov r9d, 1
    cmp al, 'a'
    jb .store
    cmp al, 'z'
    ja .store
    sub al, 32
.store:
    mov [rdx+r8], al
    inc r8d
    cmp r8d, 11
    jb .byte
    mov eax, r9d
    ret
endp

; RCX=state*. Build the BPB in scratch; no callbacks or writes.
section '.text$f_format_boot' code readable executable comdat align 16
proc f_format_boot uses rbx rsi rdi
    mov rbx, rcx
    fastcall f_format_zero, rcx
    mov rdi, [rbx + f_format_state.scratch]
    mov rsi, [rbx + f_format_state.options]
    mov word [rdi], 058EBh
    mov byte [rdi+2], 090h
    mov rax, 'FAT32   '
    mov [rdi+3], rax
    mov eax, [rbx + f_format_state.plan.sector_bytes]
    mov [rdi+11], ax
    mov eax, [rbx + f_format_state.plan.cluster_sectors]
    mov [rdi+13], al
    mov word [rdi+14], 32
    mov byte [rdi+16], 2
    mov byte [rdi+21], 0F8h
    mov word [rdi+24], 63
    mov word [rdi+26], 255
    mov eax, [rsi + FatFormatOptions.hidden_sectors]
    mov [rdi+28], eax
    mov eax, [rbx + f_format_state.plan.total_sectors]
    mov [rdi+32], eax
    mov eax, [rbx + f_format_state.plan.fat_sectors]
    mov [rdi+36], eax
    mov dword [rdi+44], 2
    mov word [rdi+48], 1
    mov word [rdi+50], 6
    mov byte [rdi+64], 80h
    mov byte [rdi+66], 29h
    mov eax, [rsi + FatFormatOptions.serial]
    mov [rdi+67], eax
    fastcall f_format_label, rsi, addr rdi+71
    test eax, eax
    jnz .label
    mov rax, 'NO NAME '
    mov [rdi+71], rax
.label:
    mov rax, 'FAT32   '
    mov [rdi+82], rax
    ; Defined inert legacy entry: no boot program is installed by formatting.
    mov word [rdi+90], 0FEEBh     ; jmp $ for accidental legacy execution
    mov word [rdi+510], 0AA55h
    ret
endp

; RCX=state*, RDX=LBA. Report attempts before dispatch; stop at first failure.
section '.text$f_format_emit' code readable executable comdat align 16
proc f_format_emit uses rbx rsi
    mov rbx, rcx
    mov rsi, [rcx + f_format_state.report]
    mov [rsi + FatFormatReport.lba], rdx
    mov dword [rsi + FatFormatReport.operation], FI_WRITE
    mov dword [rsi + FatFormatReport.effect], FE_UNCERTAIN
    inc qword [rsi + FatFormatReport.writes]
    mov rax, [rbx + f_format_state.backend]
    fastcall [rax + SectorOps.write], [rax + SectorOps.context], rdx, [rbx + f_format_state.scratch]
    test eax, eax
    jnz .done
    inc qword [rsi + FatFormatReport.completed_writes]
    cmp qword [rbx + f_format_state.verify], 0
    je .done
    fastcall [rbx + f_format_state.verify], rbx
.done:
    ret
endp

section '.text$f_format_flush' code readable executable comdat align 16
proc f_format_flush uses rbx
    mov rbx, [rcx + f_format_state.report]
    mov dword [rbx + FatFormatReport.operation], FI_FLUSH
    inc qword [rbx + FatFormatReport.flushes]
    mov rax, [rcx + f_format_state.backend]
    fastcall [rax + SectorOps.flush], [rax + SectorOps.context]
    test eax, eax
    jnz .done
    inc qword [rbx + FatFormatReport.completed_flushes]
.done:
    ret
endp

; Ordinary entry has no reference to the verification helper.
section '.text$fat_format' code readable executable comdat any align 16
proc fat_format
    fastcall f_format_run, rcx, rdx, r8, r9, 0
    ret
endp
section '.text$fat_format_verified' code readable executable comdat any align 16
proc fat_format_verified
    fastcall f_format_run, rcx, rdx, r8, r9, addr f_format_verify
    ret
endp

; backend*, options*, workspace*, report*, optional verifier address.
section '.text$f_format_run' code readable executable comdat align 16
proc f_format_run uses rbx rsi rdi rbp r13, backend_arg, options_arg, workspace_arg, report_arg, verifier_arg
    locals
        state f_format_state
    endl
body:
    lea rbx, [state]
    mov [rbx + f_format_state.backend], rcx
    mov [rbx + f_format_state.options], rdx
    mov [rbx + f_format_state.report], r9
    mov rax, [verifier_arg]
    mov [rbx + f_format_state.verify], rax
    mov rbp, r8
    mov rsi, r9
    mov rdi, r9
    mov ecx, sizeof.FatFormatReport
    xor eax, eax
    rep stosb
    mov qword [rsi + FatFormatReport.lba], -1
    fastcall fat_format_plan, [rbx + f_format_state.backend], [rbx + f_format_state.options], addr state.plan
    test eax, eax
    jnz .done
    mov r9, [rbx + f_format_state.backend]
    mov eax, F_ARGUMENT
    cmp dword [r9 + SectorOps.reserved], 0
    jne .done
    cmp qword [r9 + SectorOps.begin], 0
    jne .done
    cmp qword [r9 + SectorOps.end], 0
    jne .done
    mov eax, F_READONLY
    cmp qword [r9 + SectorOps.write], 0
    je .done
    cmp qword [r9 + SectorOps.flush], 0
    je .done
    mov ecx, [rbx + f_format_state.plan.workspace_bytes]
    cmp qword [rbx + f_format_state.verify], 0
    je .workspace
    mov eax, F_ARGUMENT
    cmp qword [r9 + SectorOps.read], 0
    je .done
    add ecx, ecx
.workspace:
    mov eax, F_ARGUMENT
    cmp dword [rbp + FatWorkspace.reserved], 0
    jne .done
    mov rdx, [rbp + FatWorkspace.data]
    test rdx, rdx
    jz .done
    mov r8d, [rbp + FatWorkspace.bytes]
    add r8, rdx
    jc .done
    mov eax, F_MEMORY
    cmp [rbp + FatWorkspace.bytes], ecx
    jb .done
    mov [rbx + f_format_state.scratch], rdx
    mov ecx, [rbx + f_format_state.plan.sector_bytes]
    add rdx, rcx
    mov [rbx + f_format_state.verify_buffer], rdx
    ; Retire both recognizable boot headers before changing allocation metadata.
    mov dword [rsi + FatFormatReport.phase], FF_INVALIDATE
    fastcall f_format_zero, rbx
    fastcall f_format_emit, rbx, 0
    test eax, eax
    jnz .done
    fastcall f_format_emit, rbx, 6
    test eax, eax
    jnz .done
    fastcall f_format_flush, rbx
    test eax, eax
    jnz .done
    mov dword [rsi + FatFormatReport.phase], FF_RESERVED
    mov ebp, 1
.reserved:
    cmp ebp, 6
    je .next_reserved
    fastcall f_format_zero, rbx
    mov rdi, [rbx + f_format_state.scratch]
    cmp ebp, 1
    je .info
    cmp ebp, 7
    je .info
    cmp ebp, 2
    je .signature
    cmp ebp, 8
    jne .reserved_write
.signature:
    mov word [rdi+510], 0AA55h
    jmp .reserved_write
.info:
    mov dword [rdi + FDiskInfo.lead_signature], FSI_LEAD_SIGNATURE
    mov dword [rdi + FDiskInfo.structure_signature], FSI_STRUCTURE_SIGNATURE
    mov eax, [rbx + f_format_state.plan.cluster_count]
    dec eax
    mov [rdi + FDiskInfo.free_count], eax
    mov dword [rdi + FDiskInfo.next_free], 3
    mov dword [rdi + FDiskInfo.trail_signature], FSI_TRAIL_SIGNATURE
.reserved_write:
    fastcall f_format_emit, rbx, rbp
    test eax, eax
    jnz .done
.next_reserved:
    inc ebp
    cmp ebp, 32
    jb .reserved
    mov dword [rsi + FatFormatReport.phase], FF_FATS
    mov ebp, 32
    xor r13d, r13d
.fat:
    fastcall f_format_zero, rbx
    test r13d, r13d
    jnz .fat_write
    mov rdi, [rbx + f_format_state.scratch]
    mov dword [rdi], 0FFFFFF8h
    mov dword [rdi+4], 0FFFFFFFh
    mov dword [rdi+8], 0FFFFFFFh
.fat_write:
    fastcall f_format_emit, rbx, rbp
    test eax, eax
    jnz .done
    inc r13d
    cmp r13d, [rbx + f_format_state.plan.fat_sectors]
    jb .next_fat
    xor r13d, r13d
.next_fat:
    inc ebp
    cmp ebp, [rbx + f_format_state.plan.data_start]
    jb .fat
    mov dword [rsi + FatFormatReport.phase], FF_ROOT
    mov r13d, [rbx + f_format_state.plan.cluster_sectors]
    fastcall f_format_zero, rbx
    mov rdi, [rbx + f_format_state.scratch]
    fastcall f_format_label, [rbx + f_format_state.options], rdi
    test eax, eax
    jz .empty_root
    mov byte [rdi+11], 8
    jmp .root
.empty_root:
    fastcall f_format_zero, rbx
.root:
    fastcall f_format_emit, rbx, rbp
    test eax, eax
    jnz .done
    inc ebp
    dec r13d
    jz .metadata_done
    fastcall f_format_zero, rbx
    jmp .root
.metadata_done:
    fastcall f_format_flush, rbx
    test eax, eax
    jnz .done
    fastcall f_format_boot, rbx
    mov dword [rsi + FatFormatReport.phase], FF_BACKUP
    fastcall f_format_emit, rbx, 6
    test eax, eax
    jnz .done
    fastcall f_format_flush, rbx
    test eax, eax
    jnz .done
    mov dword [rsi + FatFormatReport.phase], FF_PRIMARY
    fastcall f_format_emit, rbx, 0
    test eax, eax
    jnz .done
    fastcall f_format_flush, rbx
    test eax, eax
    jnz .done
    mov dword [rsi + FatFormatReport.effect], FE_COMMITTED
.done:
    mov [rsi + FatFormatReport.status], eax
    ret
endp

; Selected only by fat_format_verified: flush, backend reread, compare.
section '.text$f_format_verify' code readable executable comdat align 16
proc f_format_verify uses rbx rsi rdi rbp
    mov rbx, rcx
    mov rbp, [rcx + f_format_state.report]
    fastcall f_format_flush, rcx
    test eax, eax
    jnz .done
    mov dword [rbp + FatFormatReport.operation], FI_READ
    inc qword [rbp + FatFormatReport.reads]
    mov rax, [rbx + f_format_state.backend]
    fastcall [rax + SectorOps.read], [rax + SectorOps.context], [rbp + FatFormatReport.lba], [rbx + f_format_state.verify_buffer]
    test eax, eax
    jnz .done
    mov rsi, [rbx + f_format_state.scratch]
    mov rdi, [rbx + f_format_state.verify_buffer]
    mov ecx, [rbx + f_format_state.plan.sector_bytes]
    repe cmpsb
    je .done
    mov dword [rbp + FatFormatReport.operation], FI_VERIFY
    mov eax, F_VERIFY
.done:
    ret
endp
