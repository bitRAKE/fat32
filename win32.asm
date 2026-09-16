; Raw volume adapter. The caller selects the volume and write mode; this file
; knows geometry and Win32 lifetime, and has no FAT32 interpretation at all.
FAT32_WIN32 := 1
define win32.select.types storage_filesystem,system_io,system_memory
define win32.select.downlevel kernel32
include 'common/policy.g'
include 'buffer.inc'

; RCX=adapter* out, RDX=UTF-16 raw path, R8D=write flag; EAX=status.
public win_open
; RCX=adapter*; EAX=status. Dismount/close/free; never commit.
public win_close
; RCX=adapter*, RDX=uint64 LBA, R8=writable sector; EAX=status.
public win_read
; RCX=locked adapter*, RDX=uint64 LBA, R8=readable sector; EAX=status.
public win_write
; RCX=opened writable adapter*; EAX=status. Explicit durability call.
public win_flush

; win_open(volume, L"\\.\X:", write_mode). Caller must zero volume first.
; The handle's volume origin is sector zero; no partition offset is added.
; RCX=WinVolume* out, RDX=const UTF-16 raw volume path, R8D=write flag.
; EAX=F_OK/F_IO; writable open holds exclusive volume lock. Requires zero/freed
; adapter (never a live one). Failed open cleans up; volume.error holds Win32 code.
; RDX is consumed by CreateFileW before its second argument overwrites EDX.
proc win_open uses rbx rdi
	locals
		geometry db 32 dup ?
		length dq ?
		returned dd ?
	endl
body:
	mov rbx, rcx
	mov edi, r8d
	mov qword [rbx + WinVolume.handle], -1
	mov dword [rbx + WinVolume.locked], 0
	mov dword [rbx + WinVolume.error], 0
	mov qword [rbx + WinVolume.bounce], 0
	mov eax, 80000000h
	test edi, edi
	jz .access
	or eax, 40000000h
.access:
	CreateFileW rdx, eax, 3, 0, 3, 0A0000000h, 0
	cmp rax, -1
	je .error
	mov qword [rbx + WinVolume.handle], rax
	; IOCTL_DISK_GET_DRIVE_GEOMETRY, independent of filesystem BPB.
	DeviceIoControl [rbx + WinVolume.handle], 70000h, 0, 0, &geometry, 24, &returned, 0
	test eax, eax
	jz .error
	mov eax, dword [geometry+20]
	cmp eax, 512
	jb .format
	cmp eax, F_MAX_SECTOR
	ja .format
	lea edx, [eax-1]
	test eax, edx
	jnz .format
	mov dword [rbx + WinVolume.ops.sector_bytes], eax
	; IOCTL_DISK_GET_LENGTH_INFO describes the opened volume extent.
	DeviceIoControl [rbx + WinVolume.handle], 7405Ch, 0, 0, &length, 8, &returned, 0
	test eax, eax
	jz .error
	mov rax, qword [length]
	test rax, rax
	jle .format
	xor edx, edx
	mov ecx, dword [rbx + WinVolume.ops.sector_bytes]
	div rcx
	test edx, edx
	jnz .format
	mov qword [rbx + WinVolume.ops.sectors], rax
	VirtualAlloc 0, F_MAX_SECTOR, 3000h, 4
	test rax, rax
	jz .error
	mov qword [rbx + WinVolume.bounce], rax
	mov qword [rbx + WinVolume.ops.context], rbx
	lea rax, [win_read]
	mov qword [rbx + WinVolume.ops.read], rax
	lea rax, [win_flush]
	mov qword [rbx + WinVolume.ops.flush], rax
	mov qword [rbx + WinVolume.ops.write], 0
	mov qword [rbx + WinVolume.ops.begin], 0
	mov qword [rbx + WinVolume.ops.end], 0
	test edi, edi
	jz .ok
	; FSCTL_LOCK_VOLUME flushes host caches and excludes mounted file users.
	DeviceIoControl [rbx + WinVolume.handle], 90018h, 0, 0, 0, 0, &returned, 0
	test eax, eax
	jz .error
	mov dword [rbx + WinVolume.locked], 1
	; Keep the filesystem locked through commit. FAT32 rejects
	; FlushFileBuffers after dismount with ERROR_NOT_READY on this host.
	; Close dismounts while the lock is still held, before releasing it.
	lea rax, [win_write]
	mov qword [rbx + WinVolume.ops.write], rax
.ok:
	xor eax, eax
.done:
	ret
.format:
	mov dword [rbx + WinVolume.error], 13
	jmp .close
.error:
	GetLastError
	mov dword [rbx + WinVolume.error], eax
.close:
	fastcall win_close, rbx
	mov eax, F_IO
	jmp .done
endp

; RCX=WinVolume* previously opened/failed-open/closed -> EAX=F_OK/F_IO.
; Dismount while locked, close handle, release bounce allocation; never commit.
; Continue cleanup on failure, retaining first dismount/close error in .error.
proc win_close uses rbx rsi
	locals
		returned dd ?
	endl
body:
	mov rbx, rcx
	xor esi, esi
	cmp qword [rbx + WinVolume.handle], -1
	je .memory
	cmp dword [rbx + WinVolume.locked], 0
	je .handle
	; Invalidate the filesystem's cached view before any native access can
	; resume. This must happen after the explicit commit/flush, not on open.
	DeviceIoControl [rbx + WinVolume.handle], 90020h, 0, 0, 0, 0, &returned, 0
	test eax, eax
	jnz .handle
	GetLastError
	mov dword [rbx + WinVolume.error], eax
	mov esi, F_IO
.handle:
	CloseHandle [rbx + WinVolume.handle]
	test eax, eax
	jnz .closed
	test esi, esi
	jnz .closed
	GetLastError
	mov dword [rbx + WinVolume.error], eax
	mov esi, F_IO
.closed:
	mov qword [rbx + WinVolume.handle], -1
.memory:
	cmp qword [rbx + WinVolume.bounce], 0
	je .done
	VirtualFree [rbx + WinVolume.bounce], 0, 8000h
	mov qword [rbx + WinVolume.bounce], 0
.done:
	mov dword [rbx + WinVolume.locked], 0
	mov eax, esi
	ret
endp

; Shared seek. Single caller serialization is part of the adapter contract.
; RCX=opened WinVolume*, RDX=relative uint64 sector LBA -> EAX=status.
; Private helper bounds LBA and positions the shared handle. Caller serializes
; seek plus subsequent I/O; native failure updates volume.error.
proc win_seek uses rbx
	mov rbx, rcx
	mov eax, F_RANGE
	cmp rdx, [rbx + WinVolume.ops.sectors]
	jae .done
	mov eax, dword [rbx + WinVolume.ops.sector_bytes]
	imul rdx, rax
	SetFilePointerEx [rcx + WinVolume.handle], rdx, 0, 0
	test eax, eax
	jz .error
	xor eax, eax
.done:
	ret
.error:
	GetLastError
	mov dword [rbx + WinVolume.error], eax
	mov eax, F_IO
	jmp .done
endp

; RCX=opened WinVolume*, RDX=uint64 LBA, R8=writable one-sector buffer.
; EAX=status. Destination unchanged unless a complete sector was read; bounce
; buffer removes alignment requirements. Error/short read updates .error.
proc win_read uses rbx rsi rdi
	locals
		count dd ?
	endl
body:
	mov rbx, rcx
	mov rdi, r8
	fastcall win_seek, rcx, rdx
	test eax, eax
	jnz .done
	ReadFile [rbx + WinVolume.handle], [rbx + WinVolume.bounce], [rbx + WinVolume.ops.sector_bytes], &count, 0
	test eax, eax
	jz .error
	mov ecx, dword [rbx + WinVolume.ops.sector_bytes]
	cmp ecx, [count]
	jne .short
	mov rsi, qword [rbx + WinVolume.bounce]
	rep movsb
	xor eax, eax
.done:
	ret
.error:
	GetLastError
.record:
	mov dword [rbx + WinVolume.error], eax
	mov eax, F_IO
	jmp .done
.short:
	mov eax, 38
	jmp .record
endp

; RCX=opened/locked WinVolume*, RDX=uint64 LBA, R8=readable one sector.
; EAX=status. Unlocked adapter returns F_READONLY; native error/short write may
; already affect media and sets .error. Input consumed synchronously via bounce.
proc win_write uses rbx rsi rdi
	locals
		count dd ?
	endl
body:
	mov rbx, rcx
	mov rsi, r8
	mov eax, F_READONLY
	cmp dword [rbx + WinVolume.locked], 1
	jne .done
	fastcall win_seek, rcx, rdx
	test eax, eax
	jnz .done
	mov rdi, qword [rbx + WinVolume.bounce]
	mov ecx, dword [rbx + WinVolume.ops.sector_bytes]
	rep movsb
	WriteFile [rbx + WinVolume.handle], [rbx + WinVolume.bounce], [rbx + WinVolume.ops.sector_bytes], &count, 0
	test eax, eax
	jz .error
	mov eax, dword [rbx + WinVolume.ops.sector_bytes]
	cmp eax, [count]
	jne .short
	xor eax, eax
.done:
	ret
.error:
	GetLastError
.record:
	mov dword [rbx + WinVolume.error], eax
	mov eax, F_IO
	jmp .done
.short:
	mov eax, 29
	jmp .record
endp

; RCX=opened writable WinVolume* -> EAX=status. Explicit FlushFileBuffers;
; does not dismount or release lock. Native failure updates .error.
proc win_flush uses rbx
	mov rbx, rcx
	FlushFileBuffers [rcx + WinVolume.handle]
	test eax, eax
	jz .error
	xor eax, eax
.done:
	ret
.error:
	GetLastError
	mov dword [rbx + WinVolume.error], eax
	mov eax, F_IO
	jmp .done
endp
