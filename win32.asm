; Raw volume adapter. The caller selects the volume and write mode; this file
; knows geometry and Win32 lifetime, and has no FAT32 interpretation at all.
FAT32_WIN32 := 1
define win32.select.types storage_filesystem,system_io,system_memory
define win32.select.downlevel kernel32
include 'common/policy.g'
include 'buffer.h'

public win_open
public win_close
public win_read
public win_write
public win_flush

; win_open(volume, L"\\.\X:", write_mode). Caller must zero volume first.
; The handle's volume origin is sector zero; no partition offset is added.
proc win_open uses rbx rsi rdi
	locals
		geometry db 32 dup ?
		length dq ?
		returned dd ?
	endl
body:
	mov rbx, rcx
	mov rsi, rdx
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
	CreateFileW rsi, eax, 3, 0, 3, 0A0000000h, 0
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
	DeviceIoControl [rbx + WinVolume.handle], 90020h, 0, 0, 0, 0, &returned, 0
	test eax, eax
	jz .error
	lea rax, [win_write]
	mov qword [rbx + WinVolume.ops.write], rax
.ok:
	xor eax, eax
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
	ret
endp

proc win_close uses rbx
	mov rbx, rcx
	cmp qword [rbx + WinVolume.handle], -1
	je .memory
	CloseHandle [rbx + WinVolume.handle]
	mov qword [rbx + WinVolume.handle], -1
.memory:
	cmp qword [rbx + WinVolume.bounce], 0
	je .done
	VirtualFree [rbx + WinVolume.bounce], 0, 8000h
	mov qword [rbx + WinVolume.bounce], 0
.done:
	mov dword [rbx + WinVolume.locked], 0
	xor eax, eax
	ret
endp

; Shared seek. Single caller serialization is part of the adapter contract.
proc win_seek uses rbx
	mov rbx, rcx
	cmp rdx, [rbx + WinVolume.ops.sectors]
	jae .range
	mov eax, dword [rbx + WinVolume.ops.sector_bytes]
	imul rdx, rax
	SetFilePointerEx [rbx + WinVolume.handle], rdx, 0, 0
	test eax, eax
	jz .error
	xor eax, eax
	ret
.error:
	GetLastError
	mov dword [rbx + WinVolume.error], eax
	mov eax, F_IO
	ret
.range:
	mov eax, F_RANGE
	ret
endp

proc win_read uses rbx rsi rdi
	locals
		count dd ?
	endl
body:
	mov rbx, rcx
	mov rdi, r8
	fastcall win_seek, rbx, rdx
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
	mov dword [rbx + WinVolume.error], eax
	mov eax, F_IO
	ret
.short:
	mov dword [rbx + WinVolume.error], 38
	mov eax, F_IO
	ret
endp

proc win_write uses rbx rsi rdi
	locals
		count dd ?
	endl
body:
	mov rbx, rcx
	mov rsi, r8
	cmp dword [rbx + WinVolume.locked], 1
	jne .ro
	fastcall win_seek, rbx, rdx
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
	mov dword [rbx + WinVolume.error], eax
	mov eax, F_IO
	ret
.short:
	mov dword [rbx + WinVolume.error], 29
	mov eax, F_IO
	ret
.ro:
	mov eax, F_READONLY
	ret
endp

proc win_flush uses rbx
	mov rbx, rcx
	FlushFileBuffers [rbx + WinVolume.handle]
	test eax, eax
	jz .error
	xor eax, eax
	ret
.error:
	GetLastError
	mov dword [rbx + WinVolume.error], eax
	mov eax, F_IO
	ret
endp
