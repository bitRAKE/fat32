; Caller holds one shared-volume lease through action, diagnosis and policy
; publication. Caller-built callbacks are trusted, as for fat_call_locked.
section '.text$f_policy_current' code readable executable comdat align 16
proc f_policy_current
    mov eax, F_STALE
    mov r10, [rcx + FatPolicy.volume]
    test r10, r10
    jz .done
    mov r11, [r10 + FatVolume.identity]
    cmp r11, [rcx + FatPolicy.identity]
    jne .done
    test r11, r11
    jz .done
    cmp dword [r11 + FatIdentity.magic], F_MAGIC
    jne .done
    cmp [r11 + FatIdentity.shared], r10
    jne .done
    mov r9, [r11 + FatIdentity.generation]
    cmp r9, [rcx + FatPolicy.generation]
    jne .done
    cmp r9, [r10 + FatVolume.generation]
    jne .done
    mov r9, [r10 + FatVolume.epoch]
    cmp r9, [rcx + FatPolicy.epoch]
    jne .done
    mov eax, F_BUSY
    cmp dword [r11 + FatIdentity.transaction], 0
    jne .done
    xor eax, eax
.done:
    ret
endp

; Zeroed/closed output, current shared volume, FatCheck[capacity], capacity.
; Report storage is not cleared and no object is pinned until diagnosed bad.
section '.text$fat_policy_init' code readable executable comdat any align 16
proc fat_policy_init uses rbx rdi
    ; This leaf needs no saved copies of the volume, report array or capacity.
    mov rbx, rcx
    mov eax, F_BUSY
    cmp qword [rbx + FatPolicy.volume], 0
    jne .done
    cmp dword [rbx + FatPolicy.active], 0
    jne .done
    mov eax, F_ARGUMENT
    test r9d, r9d
    jz .storage
    test r8, r8
    jz .done
    mov ecx, r9d
    imul rcx, sizeof.FatCheck
    add rcx, r8
    jc .done
.storage:
    mov eax, F_STALE
    mov r10, [rdx + FatVolume.identity]
    test r10, r10
    jz .done
    cmp dword [r10 + FatIdentity.magic], F_MAGIC
    jne .done
    cmp [r10 + FatIdentity.shared], rdx
    jne .done
    mov r11, [r10 + FatIdentity.generation]
    cmp r11, [rdx + FatVolume.generation]
    jne .done
    cmp qword [rdx + FatVolume.epoch], 0
    je .done
    mov eax, F_BUSY
    cmp dword [r10 + FatIdentity.transaction], 0
    jne .done
    mov rdi, rbx
    mov ecx, sizeof.FatPolicy/8
    xor eax, eax
    rep stosq
    mov [rbx + FatPolicy.volume], rdx
    mov [rbx + FatPolicy.identity], r10
    mov [rbx + FatPolicy.generation], r11
    mov rdx, [rdx + FatVolume.epoch]
    mov [rbx + FatPolicy.epoch], rdx
    mov [rbx + FatPolicy.reports], r8
    mov [rbx + FatPolicy.capacity], r9d
    mov qword [rbx + FatPolicy.cause.sector], -1
.done:
    ret
endp

; Build a temporary borrowed handle to validate/release an actual report pin.
; RCX=report*, RDX=FatHandle* out. No reference count change here.
section '.text$f_policy_handle' code readable executable comdat align 16
proc f_policy_handle
    mov rax, [rcx + FatCheck.volume]
    mov [rdx + FatHandle.volume], rax
    mov rax, [rcx + FatCheck.object]
    mov [rdx + FatHandle.object], rax
    mov rax, [rcx + FatCheck.epoch]
    mov [rdx + FatHandle.epoch], rax
    mov rax, [rcx + FatCheck.incarnation]
    mov [rdx + FatHandle.incarnation], rax
    mov qword [rdx + FatHandle.position], 0
    mov dword [rdx + FatHandle.access], FH_READ
    mov dword [rdx + FatHandle.reserved], 0
    ret
endp

; RCX=current policy, RDX=immediate report; called under the active lease.
; Only current structural/attention/verification evidence restricts capabilities.
section '.text$f_policy_note' code readable executable comdat align 16
proc f_policy_note uses rbx rsi rdi rbp
    locals
        handle FatHandle
    endl
body:
    mov rbx, rcx
    mov rbp, rdx
    mov eax, F_ARGUMENT
    mov ecx, [rbp + FatCheck.scope]
    dec ecx
    cmp ecx, FC_SALVAGE
    jae .done
    mov eax, F_STALE
    mov rdx, [rbx + FatPolicy.identity]
    cmp [rbp + FatCheck.identity], rdx
    jne .done
    mov rdx, [rbx + FatPolicy.generation]
    cmp [rbp + FatCheck.generation], rdx
    jne .done
    cmp dword [rbp + FatCheck.scope], FC_FILE
    jne .status
    mov rdx, [rbx + FatPolicy.volume]
    cmp [rbp + FatCheck.volume], rdx
    jne .done
    fastcall f_policy_handle, rbp, addr handle
    fastcall fat_check_fresh, addr handle, rbp
    cmp eax, F_STALE
    je .done
    cmp eax, [rbp + FatCheck.status]
    jne .stale
.status:
    mov eax, [rbp + FatCheck.status]
    cmp eax, F_CORRUPT
    je .adverse
    cmp eax, F_ATTENTION
    je .adverse
    cmp eax, F_VERIFY
    jne .ok
.adverse:
    test dword [rbx + FatPolicy.flags], FP_NO_WRITE
    jnz .retained
    mov rsi, rbp
    lea rdi, [rbx + FatPolicy.cause]
    mov ecx, sizeof.FatCheck/8
    rep movsq
    mov qword [rbx + FatPolicy.cause_target], 0
.retained:
    or dword [rbx + FatPolicy.flags], FP_NO_WRITE
    cmp dword [rbp + FatCheck.status], F_ATTENTION
    je .ok
    cmp dword [rbp + FatCheck.scope], FC_FILE
    jne .global
    cmp dword [rbp + FatCheck.status], F_CORRUPT
    jne .global
    mov rsi, [rbx + FatPolicy.reports]
    xor r8d, r8d
.existing:
    cmp r8d, [rbx + FatPolicy.count]
    jae .new
    mov rax, [rsi + FatCheck.object]
    cmp rax, [rbp + FatCheck.object]
    je .ok                      ; pinned object cannot retire while policy lives
    add rsi, sizeof.FatCheck
    inc r8d
    jmp .existing
.new:
    cmp r8d, [rbx + FatPolicy.capacity]
    jae .memory
    mov rdx, [rbp + FatCheck.object]
    cmp dword [rdx + FatObject.references], -1
    je .limit
    inc dword [rdx + FatObject.references]
    mov rdi, rsi
    mov rsi, rbp
    mov ecx, sizeof.FatCheck/8
    rep movsq
    inc dword [rbx + FatPolicy.count]
.ok:
    xor eax, eax
.done:
    ret
.global:
    or dword [rbx + FatPolicy.flags], FP_NO_READ
    jmp .ok
.memory:
    mov eax, F_MEMORY
    jmp .closed
.limit:
    mov eax, F_LIMIT
.closed:
    or dword [rbx + FatPolicy.flags], FP_NO_READ
    jmp .done
.stale:
    mov eax, F_STALE
    jmp .done
endp

section '.text$fat_policy_note' code readable executable comdat any align 16
proc fat_policy_note
    mov eax, F_BUSY
    cmp dword [rcx + FatPolicy.active], 0
    jne .done
    fastcall f_policy_current, rcx
    test eax, eax
    jnz .done
    fastcall f_policy_note, rcx, rdx
.done:
    ret
endp

; Preserve first action failure separately from any later diagnostic finding.
section '.text$f_policy_failure' code readable executable comdat align 16
proc f_policy_failure
    test dword [rcx + FatPolicy.flags], FP_NO_WRITE
    jnz .done
    mov [rcx + FatPolicy.cause.status], edx
    mov [rcx + FatPolicy.cause_target], r8
    mov rax, [rcx + FatPolicy.identity]
    mov [rcx + FatPolicy.cause.identity], rax
    mov rax, [rcx + FatPolicy.generation]
    mov [rcx + FatPolicy.cause.generation], rax
.done:
    or dword [rcx + FatPolicy.flags], FP_NO_WRITE
    ret
endp

section '.text$fat_policy_call' code readable executable comdat any align 16
proc fat_policy_call uses rbx rsi rdi rbp r13 r14
    mov rbx, rcx
    mov rsi, rdx
    mov dword [rsi + FatPolicyCall.invoked], 0
    mov dword [rsi + FatPolicyCall.diagnosed], 0
    mov eax, F_BUSY
    cmp dword [rbx + FatPolicy.active], 0
    jne .done
    fastcall f_policy_current, rcx
    test eax, eax
    jnz .done
    mov eax, F_ARGUMENT
    cmp dword [rsi + FatPolicyCall.reserved], 0
    jne .done
    cmp qword [rsi + FatPolicyCall.action.target], 0
    je .done
    mov edi, [rsi + FatPolicyCall.kind]
    lea ecx, [rdi-1]
    cmp ecx, 2
    ja .done
    mov rbp, [rsi + FatPolicyCall.diagnosis]
    mov r13, [rsi + FatPolicyCall.report]
    test rbp, rbp
    jz .no_diagnosis
    test r13, r13
    jz .done
    cmp edi, FP_INSPECT
    je .done
    cmp qword [rbp + FatCall.target], 0
    je .done
    jmp .subject
.no_diagnosis:
    test r13, r13
    jnz .done
.subject:
    mov rcx, [rsi + FatPolicyCall.subject]
    test rcx, rcx
    jz .admit
    fastcall f_handle, rcx
    test eax, eax
    jnz .done
    mov eax, F_ARGUMENT
    cmp r10, [rbx + FatPolicy.volume]
    jne .done
.admit:
    cmp edi, FP_INSPECT
    je .invoke
    cmp edi, FP_WRITE
    jne .read
    mov eax, F_READONLY
    test dword [rbx + FatPolicy.flags], FP_NO_WRITE
    jnz .done
    jmp .invoke
.read:
    mov eax, F_CORRUPT
    test dword [rbx + FatPolicy.flags], FP_NO_READ
    jnz .done
    cmp dword [rbx + FatPolicy.count], 0
    je .invoke
    mov eax, F_ARGUMENT
    cmp qword [rsi + FatPolicyCall.subject], 0
    je .done
    mov r13, [rbx + FatPolicy.reports]
    mov r14d, [rbx + FatPolicy.count]
.report:
    fastcall fat_check_fresh, [rsi + FatPolicyCall.subject], r13
    cmp eax, F_CORRUPT
    je .done
    add r13, sizeof.FatCheck
    dec r14d
    jnz .report
.invoke:
    mov dword [rbx + FatPolicy.active], 1
    mov dword [rsi + FatPolicyCall.invoked], 1
    fastcall [rsi + FatPolicyCall.action.target], [rsi + FatPolicyCall.action.arg1], [rsi + FatPolicyCall.action.arg2], [rsi + FatPolicyCall.action.arg3], [rsi + FatPolicyCall.action.arg4]
    mov r14d, eax
    cmp edi, FP_INSPECT
    je .return
    cmp eax, F_CORRUPT
    je .corrupt
    cmp eax, F_VERIFY
    je .verify
    cmp eax, F_ATTENTION
    je .attention
    cmp eax, F_IO
    jne .return
    jmp .diagnose
.verify:
    or dword [rbx + FatPolicy.flags], FP_NO_READ
.attention:
    fastcall f_policy_failure, rbx, r14d, [rsi + FatPolicyCall.action.target]
    jmp .return
.corrupt:
    fastcall f_policy_failure, rbx, r14d, [rsi + FatPolicyCall.action.target]
.diagnose:
    test rbp, rbp
    jz .unscoped
    fastcall f_policy_current, rbx
    test eax, eax
    jnz .unscoped              ; retirement/uncertainty is not a retry route
    mov dword [rsi + FatPolicyCall.diagnosed], 1
    ; An unsuccessful callback must not reuse an earlier observation.
    fastcall f_check_clear, [rsi + FatPolicyCall.report]
    fastcall [rbp + FatCall.target], [rbp + FatCall.arg1], [rbp + FatCall.arg2], [rbp + FatCall.arg3], [rbp + FatCall.arg4]
    fastcall f_policy_current, rbx
    test eax, eax
    jnz .unscoped
    mov r13, [rsi + FatPolicyCall.report]
    fastcall f_policy_note, rbx, r13
    test eax, eax
    jnz .unscoped
    cmp r14d, F_CORRUPT
    jne .return
    ; A matching live file diagnosis may scope the ordinary-read refusal.
    cmp edi, FP_READ
    jne .unscoped
    cmp dword [r13 + FatCheck.scope], FC_FILE
    jne .unscoped
    cmp dword [r13 + FatCheck.status], F_CORRUPT
    jne .unscoped
    mov rcx, [rsi + FatPolicyCall.subject]
    test rcx, rcx
    jz .unscoped
    fastcall fat_check_fresh, rcx, r13
    cmp eax, F_CORRUPT
    je .return
.unscoped:
    cmp r14d, F_CORRUPT
    jne .return
    or dword [rbx + FatPolicy.flags], FP_NO_READ
.return:
    mov dword [rbx + FatPolicy.active], 0
    xchg eax, r14d
.done:
    ret
endp

; Retire the wrapper and release its report pins, even after mount retirement.
; Evidence bytes remain for inspection. Reinitialization is an explicit new
; policy session after reconciliation, not a hidden clearing of known damage.
section '.text$fat_policy_close' code readable executable comdat any align 16
proc fat_policy_close uses rbx rsi rdi
    locals
        handle FatHandle
    endl
body:
    mov rbx, rcx
    mov eax, F_BUSY
    cmp dword [rbx + FatPolicy.active], 0
    jne .done
    mov eax, F_STALE
    cmp qword [rbx + FatPolicy.volume], 0
    je .done
    mov rsi, [rbx + FatPolicy.reports]
    mov edi, [rbx + FatPolicy.count]
.release:
    test edi, edi
    jz .closed
    fastcall f_policy_handle, rsi, addr handle
    fastcall fat_close, addr handle
    add rsi, sizeof.FatCheck
    dec edi
    jmp .release
.closed:
    mov qword [rbx + FatPolicy.volume], 0
    mov qword [rbx + FatPolicy.identity], 0
    xor eax, eax
.done:
    ret
endp
