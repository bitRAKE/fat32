; Win32-owned, format-neutral copy-on-write sector buffer.
; Versions before mark are immutable during an operation. Rollback therefore
; requires only freeing the new prefix; it cannot fail or allocate memory.
FAT32_WIN32 := 1
define win32.select.types system_memory
define win32.select.downlevel kernel32
include '../../common/policy.g'
include 'buffer.inc'

; RCX=buffer* out, RDX=backend SectorOps*; EAX=status.
public sb_init
; RCX=buffer*, RDX=uint64 LBA, R8=writable sector; EAX=status.
public sb_read
; RCX=buffer*, RDX=uint64 LBA, R8=readable sector; EAX=status.
public sb_write
; RCX=buffer*; EAX=status. One isolated savepoint.
public sb_begin
; RCX=buffer*, EDX=accept (0 rollback); VOID/cannot fail.
public sb_end
; RCX=buffer*; EAX=F_OK. Free pending pages; no I/O.
public sb_discard
; RCX=buffer*; EAX=status. Write/flush, poison on backend failure.
public sb_commit

; RCX=SectorBuffer* out, RDX=const SectorOps* backend -> EAX=status.
; Fresh/discarded storage only; clears object, validates geometry/read callback.
; No calls: R8/RDX retain buffer/backend, only STOSB's RDI needs preservation.
proc sb_init uses rdi
	mov r8, rcx
	mov rdi, rcx
	mov ecx, sizeof.SectorBuffer
	xor eax, eax
	rep stosb
	cmp qword [rdx + SectorOps.read], 0
	je .bad
	cmp qword [rdx + SectorOps.sectors], 0
	je .bad
	mov eax, dword [rdx + SectorOps.sector_bytes]
	cmp eax, 512
	jb .bad
	cmp eax, F_MAX_SECTOR
	ja .bad
	lea ecx, [eax-1]
	test eax, ecx
	jnz .bad
	mov qword [r8 + SectorBuffer.backend], rdx
	mov qword [r8 + SectorBuffer.ops.context], r8
	lea rax, [sb_read]
	mov qword [r8 + SectorBuffer.ops.read], rax
	lea rax, [sb_write]
	mov qword [r8 + SectorBuffer.ops.write], rax
	lea rax, [sb_begin]
	mov qword [r8 + SectorBuffer.ops.begin], rax
	lea rax, [sb_end]
	mov qword [r8 + SectorBuffer.ops.end], rax
	mov rax, qword [rdx + SectorOps.sectors]
	mov qword [r8 + SectorBuffer.ops.sectors], rax
	mov eax, dword [rdx + SectorOps.sector_bytes]
	mov dword [r8 + SectorBuffer.ops.sector_bytes], eax
	mov qword [r8 + SectorBuffer.limit], 131072
	xor eax, eax
.done:
	ret
.bad:
	mov eax, F_ARGUMENT
	jmp .done
endp

; RCX=buffer*, RDX=uint64 LBA, R8=writable sector_bytes -> EAX=status.
; Newest pending version wins; otherwise call backend. Can read inside/outside
; savepoint; poisoned buffer rejects reads. R9 is dead after backend dispatch.
proc sb_read uses rsi rdi
	mov r9, rcx
	mov eax, F_IO
	cmp dword [r9 + SectorBuffer.poisoned], 0
	jne .done
	mov eax, F_RANGE
	cmp rdx, [r9 + SectorBuffer.ops.sectors]
	jae .done
	mov rsi, qword [r9 + SectorBuffer.head]
.search:
	test rsi, rsi
	jz .backend
	cmp rdx, [rsi + SectorPage.lba]
	je .copy
	mov rsi, qword [rsi + SectorPage.next]
	jmp .search
.copy:
	add rsi, SectorPage.data
	mov rdi, r8
	mov ecx, dword [r9 + SectorBuffer.ops.sector_bytes]
	rep movsb
	xor eax, eax
.done:
	ret
.backend:
	mov rsi, qword [r9 + SectorBuffer.backend]
	fastcall [rsi + SectorOps.read], [rsi + SectorOps.context], rdx, r8
	jmp .done
endp

; RCX=buffer*, RDX=uint64 LBA, R8=readable complete sector -> EAX=status.
; Active savepoint required. Copy before return; preserve older versions until
; acceptance/rollback. Failure leaves prior overlay intact; no device writes.
proc sb_write uses rbx rsi rdi r12
	mov rbx, rcx
	mov r12, rdx
	mov rsi, r8
	mov eax, F_BUSY
	cmp dword [rbx + SectorBuffer.active], 1
	jne .done
	mov eax, F_RANGE
	cmp rdx, [rbx + SectorBuffer.ops.sectors]
	jae .done
	mov rdi, qword [rbx + SectorBuffer.head]
.search:
	cmp rdi, [rbx + SectorBuffer.mark]
	je .allocate
	cmp r12, [rdi + SectorPage.lba]
	je .copy
	mov rdi, qword [rdi + SectorPage.next]
	jmp .search
.allocate:
	mov rax, qword [rbx + SectorBuffer.pages]
	cmp rax, [rbx + SectorBuffer.limit]
	jae .memory
	VirtualAlloc 0, sizeof.SectorPage, 3000h, 4
	test rax, rax
	jz .memory
	mov rdi, rax
	mov rax, qword [rbx + SectorBuffer.head]
	mov qword [rdi + SectorPage.next], rax
	mov qword [rdi + SectorPage.lba], r12
	mov qword [rbx + SectorBuffer.head], rdi
	inc qword [rbx + SectorBuffer.pages]
.copy:
	add rdi, SectorPage.data
	mov ecx, dword [rbx + SectorBuffer.ops.sector_bytes]
	rep movsb
	xor eax, eax
.done:
	ret
.memory:
	mov eax, F_MEMORY
	jmp .done
endp

; RCX=buffer* -> EAX=status. Open one savepoint; no nesting/allocation/I/O.
; F_BUSY if active, F_IO if poisoned; failed begin makes no changes.
proc sb_begin
	mov eax, F_IO
	cmp dword [rcx + SectorBuffer.poisoned], 0
	jne .done
	mov eax, F_BUSY
	cmp dword [rcx + SectorBuffer.active], 0
	jne .done
	mov rax, qword [rcx + SectorBuffer.head]
	mov qword [rcx + SectorBuffer.mark], rax
	mov dword [rcx + SectorBuffer.active], 1
	xor eax, eax
.done:
	ret
endp

; RCX=buffer*, EDX=accept (0 rollback, nonzero accept) -> VOID.
; Called once after successful begin; clear active/mark. Rollback frees only new
; versions and cannot fail. Internal discard also uses it with mark=0.
proc sb_end uses rbx rsi
	mov rbx, rcx
	test edx, edx
	jnz .end
.free:
	mov rcx, qword [rbx + SectorBuffer.head]
	cmp rcx, [rbx + SectorBuffer.mark]
	je .end
	mov rsi, qword [rcx + SectorPage.next]
	VirtualFree rcx, 0, 8000h
	mov qword [rbx + SectorBuffer.head], rsi
	dec qword [rbx + SectorBuffer.pages]
	jmp .free
.end:
	mov dword [rbx + SectorBuffer.active], 0
	mov qword [rbx + SectorBuffer.mark], 0
	ret
endp

; RCX=buffer* -> EAX=F_OK. Free all pages, cancel savepoint; no media I/O.
; Poison remains set. Caller must invalidate attached FAT identities afterward.
; No state needed after sb_end, so no nonvolatile save is necessary.
proc sb_discard
	mov qword [rcx + SectorBuffer.mark], 0
	fastcall sb_end, rcx, 0
	xor eax, eax
	ret
endp

; Device commit is explicitly separate from logical operation acceptance.
; A failed write/flush poisons the buffer: no uncertain retry is permitted.
; This is not an on-disk journal and cannot make a power failure atomic.
; RCX=buffer* -> EAX=status. Idle, unpoisoned buffer with writable/flushable
; backend required. Latest sectors then flush; success frees pages. Any backend
; failure poisons and retains them. Never retry a failed commit; see buffer.inc.
proc sb_commit uses rbx rsi r12
	mov rbx, rcx
	mov eax, F_BUSY
	cmp dword [rbx + SectorBuffer.active], 0
	jne .done
	mov eax, F_IO
	cmp dword [rbx + SectorBuffer.poisoned], 0
	jne .done
	mov r12, qword [rbx + SectorBuffer.backend]
	mov eax, F_READONLY
	cmp qword [r12 + SectorOps.write], 0
	je .done
	cmp qword [r12 + SectorOps.flush], 0
	je .done
	mov rsi, qword [rbx + SectorBuffer.head]
.page:
	test rsi, rsi
	jz .flush
	; Superseded versions are never sent to the device.
	mov r9, qword [rbx + SectorBuffer.head]
	mov rax, qword [rsi + SectorPage.lba]
.newer:
	cmp r9, rsi
	je .write
	cmp rax, [r9 + SectorPage.lba]
	je .skip
	mov r9, qword [r9 + SectorPage.next]
	jmp .newer
.write:
	fastcall [r12 + SectorOps.write], [r12 + SectorOps.context], [rsi + SectorPage.lba], &rsi + SectorPage.data
	test eax, eax
	jnz .failed
.skip:
	mov rsi, qword [rsi + SectorPage.next]
	jmp .page
.flush:
	fastcall [r12 + SectorOps.flush], [r12 + SectorOps.context]
	test eax, eax
	jnz .failed
	fastcall sb_discard, rbx
.done:
	ret
.failed:
	mov dword [rbx + SectorBuffer.poisoned], 1
	jmp .done
endp
