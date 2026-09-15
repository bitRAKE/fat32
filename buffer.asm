; Win32-owned, format-neutral copy-on-write sector buffer.
; Versions before mark are immutable during an operation. Rollback therefore
; requires only freeing the new prefix; it cannot fail or allocate memory.
FAT32_WIN32 := 1
define win32.select.types system_memory
define win32.select.downlevel kernel32
include 'common/policy.g'
include 'buffer.h'

public sb_init
public sb_read
public sb_write
public sb_begin
public sb_end
public sb_discard
public sb_commit

proc sb_init uses rbx rsi rdi
	mov rbx, rcx
	mov rsi, rdx
	mov rdi, rbx
	mov ecx, sizeof.SectorBuffer
	xor eax, eax
	rep stosb
	cmp qword [rsi + SectorOps.read], 0
	je .bad
	cmp qword [rsi + SectorOps.sectors], 0
	je .bad
	mov eax, dword [rsi + SectorOps.sector_bytes]
	cmp eax, 512
	jb .bad
	cmp eax, F_MAX_SECTOR
	ja .bad
	lea edx, [eax-1]
	test eax, edx
	jnz .bad
	mov qword [rbx + SectorBuffer.backend], rsi
	mov qword [rbx + SectorBuffer.ops.context], rbx
	lea rax, [sb_read]
	mov qword [rbx + SectorBuffer.ops.read], rax
	lea rax, [sb_write]
	mov qword [rbx + SectorBuffer.ops.write], rax
	lea rax, [sb_begin]
	mov qword [rbx + SectorBuffer.ops.begin], rax
	lea rax, [sb_end]
	mov qword [rbx + SectorBuffer.ops.end], rax
	mov rax, qword [rsi + SectorOps.sectors]
	mov qword [rbx + SectorBuffer.ops.sectors], rax
	mov eax, dword [rsi + SectorOps.sector_bytes]
	mov dword [rbx + SectorBuffer.ops.sector_bytes], eax
	mov qword [rbx + SectorBuffer.limit], 131072
	xor eax, eax
	ret
.bad:
	mov eax, F_ARGUMENT
	ret
endp

proc sb_read uses rbx rsi rdi
	mov rbx, rcx
	cmp dword [rbx + SectorBuffer.poisoned], 0
	jne .io
	cmp rdx, [rbx + SectorBuffer.ops.sectors]
	jae .range
	mov rsi, qword [rbx + SectorBuffer.head]
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
	mov ecx, dword [rbx + SectorBuffer.ops.sector_bytes]
	rep movsb
	xor eax, eax
	ret
.backend:
	mov rsi, qword [rbx + SectorBuffer.backend]
	fastcall [rsi + SectorOps.read], [rsi + SectorOps.context], rdx, r8
	ret
.range:
	mov eax, F_RANGE
	ret
.io:
	mov eax, F_IO
	ret
endp

proc sb_write uses rbx rsi rdi r12
	mov rbx, rcx
	mov r12, rdx
	mov rsi, r8
	cmp dword [rbx + SectorBuffer.active], 1
	jne .busy
	cmp rdx, [rbx + SectorBuffer.ops.sectors]
	jae .range
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
	ret
.busy:
	mov eax, F_BUSY
	ret
.range:
	mov eax, F_RANGE
	ret
.memory:
	mov eax, F_MEMORY
	ret
endp

proc sb_begin
	cmp dword [rcx + SectorBuffer.poisoned], 0
	jne .io
	cmp dword [rcx + SectorBuffer.active], 0
	jne .busy
	mov rax, qword [rcx + SectorBuffer.head]
	mov qword [rcx + SectorBuffer.mark], rax
	mov dword [rcx + SectorBuffer.active], 1
	xor eax, eax
	ret
.busy:
	mov eax, F_BUSY
	ret
.io:
	mov eax, F_IO
	ret
endp

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

proc sb_discard uses rbx
	mov rbx, rcx
	mov qword [rbx + SectorBuffer.mark], 0
	fastcall sb_end, rbx, 0
	xor eax, eax
	ret
endp

; Device commit is explicitly separate from logical operation acceptance.
; A failed write/flush poisons the buffer: no uncertain retry is permitted.
; This is not an on-disk journal and cannot make a power failure atomic.
proc sb_commit uses rbx rsi rdi r12
	mov rbx, rcx
	cmp dword [rbx + SectorBuffer.active], 0
	jne .busy
	cmp dword [rbx + SectorBuffer.poisoned], 0
	jne .io
	mov r12, qword [rbx + SectorBuffer.backend]
	cmp qword [r12 + SectorOps.write], 0
	je .ro
	cmp qword [r12 + SectorOps.flush], 0
	je .ro
	mov rsi, qword [rbx + SectorBuffer.head]
.page:
	test rsi, rsi
	jz .flush
	; Superseded versions are never sent to the device.
	mov rdi, qword [rbx + SectorBuffer.head]
	mov rax, qword [rsi + SectorPage.lba]
.newer:
	cmp rdi, rsi
	je .write
	cmp rax, [rdi + SectorPage.lba]
	je .skip
	mov rdi, qword [rdi + SectorPage.next]
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
	ret
.failed:
	mov dword [rbx + SectorBuffer.poisoned], 1
	ret
.ro:
	mov eax, F_READONLY
	ret
.busy:
	mov eax, F_BUSY
	ret
.io:
	mov eax, F_IO
	ret
endp
