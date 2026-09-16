; Minimal read-only client. Set the sample device below before running.
; All filesystem calls go through fat32.lib; the application has no BPB,
; FAT entry, cluster traversal, or directory-record interpretation.
FAT32_WIN32 := 1
define win32.select.types system_console,globalization
define win32.select.downlevel kernel32
include '../../common/policy.g'
include '../../tests/win32/buffer.inc'

public mainCRTStartup
public _load_config_used
extrn win_open
extrn win_close
extrn sb_init
extrn sb_discard
extrn fat_mount
extrn fat_volume_init
extrn fat_volume_close
extrn fat_root
extrn fat_close
extrn fat_iter_open
extrn fat_iter_next
extrn fat_iter_close

proc mainCRTStartup uses rdi
	GetStdHandle -11
	mov qword [output], rax
	fastcall win_open, &volume, &device, 0
	test eax, eax
	jnz .failed
	fastcall sb_init, &buffer, &volume
	test eax, eax
	jnz .failed
	fastcall fat_mount, &identity, &buffer, 0, &workspace
	test eax, eax
	jnz .failed
	fastcall fat_volume_init, &shared, &identity, &objects, 1
	test eax, eax
	jnz .failed
	fastcall fat_root, &shared, FH_READ, &root
	test eax, eax
	jnz .failed
	fastcall fat_iter_open, &root, &cursor
	test eax, eax
	jnz .failed
.next:
	fastcall fat_iter_next, &cursor, &entry
	cmp eax, F_END
	je .success
	test eax, eax
	jnz .failed
	WideCharToMultiByte 65001, 0, &entry.name, dword [entry.name_length], &text, 2046, 0, 0
	test eax, eax
	jz .failed
	lea rcx, [text]
	mov word [rcx+rax], 0A0Dh
	add eax, 2
	WriteFile qword [output], &text, eax, &written, 0
	test eax, eax
	jz .failed
	jmp .next
.success:
	xor edi, edi
	jmp .close
.failed:
	mov edi, 1
	WriteFile qword [output], &message, message_bytes, &written, 0
.close:
	fastcall fat_iter_close, &cursor
	fastcall fat_close, &root
	fastcall fat_volume_close, &shared
	fastcall sb_discard, &buffer
	fastcall win_close, &volume
	ExitProcess edi
endp

section '.rdata' data readable align 8
workspace FatWorkspace workspace_data, 3*F_MAX_SECTOR, 0
device dw '\', '\', '.', '\', 'X', ':', 0
message db 'FAT32 operation failed. Run usbcheck X: for diagnostic detail.',13,10
message_bytes = $-message

; Subsystem-10 normal-loader policy.
align 8
_load_config_used:
	dd 148,0
	dw 0,0
	dd 0,0,0
	dq 0,0,0,0,0,0
	dd 0
	dw 0,0
	dq 0,0,0,0,0,0,0,0
	dd 800h
	assert $ - _load_config_used = 148

section '.bss' readable writeable align 64
volume WinVolume
buffer SectorBuffer
identity FatIdentity
workspace_data db 3*F_MAX_SECTOR dup ?
shared FatVolume
objects FatObject
root FatHandle
cursor FatIterator
entry FatEntry
output dq ?
written dd ?
text db 2048 dup ?

virtual as 'response'
	db '/NOLOGO',10,'/NODEFAULTLIB',10,'/DYNAMICBASE',10
	db '/SUBSYSTEM:CONSOLE,10.0',10,'kernel32.lib',10
end virtual
