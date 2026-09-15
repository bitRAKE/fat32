; Minimal read-only client. Prints the root names of TESTING (X:).
; All filesystem calls go through fat32.lib; the application has no BPB,
; FAT entry, cluster traversal, or directory-record interpretation.
FAT32_WIN32 := 1
define win32.select.types system_console,globalization
define win32.select.downlevel kernel32
include 'common/policy.g'
include 'buffer.h'

public mainCRTStartup
public _load_config_used
extrn win_open
extrn win_close
extrn sb_init
extrn sb_discard
extrn fat_mount
extrn fat_dir_open
extrn fat_dir_next

proc mainCRTStartup uses rbx rsi rdi
	GetStdHandle -11
	mov qword [output], rax
	fastcall win_open, &volume, &device, 0
	test eax, eax
	jnz .failed
	fastcall sb_init, &buffer, &volume
	test eax, eax
	jnz .failed
	fastcall fat_mount, &identity, &buffer, 0
	test eax, eax
	jnz .failed
	fastcall fat_dir_open, &identity, dword [identity.root_cluster], &cursor
	test eax, eax
	jnz .failed
.next:
	fastcall fat_dir_next, &identity, &cursor, &entry
	cmp eax, F_END
	je .success
	test eax, eax
	jnz .failed
	WideCharToMultiByte 65001, 0, &entry.name, dword [entry.name_length], &text, 2046, 0, 0
	test eax, eax
	jz .failed
	lea rsi, [text]
	mov word [rsi+rax], 0A0Dh
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
	fastcall sb_discard, &buffer
	fastcall win_close, &volume
	ExitProcess edi
endp

section '.rdata' data readable align 8
device dw '\', '\', '.', '\', 'X', ':', 0
message db 'FAT32 operation failed. Run usbcheck X: for diagnostic detail.',13,10
message_bytes = $-message

; The subsystem-10 normal-loader policy used by hexed.
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
cursor FatCursor
entry FatEntry
output dq ?
written dd ?
text db 2048 dup ?

virtual as 'response'
	db '/NOLOGO',10,'/NODEFAULTLIB',10,'/DYNAMICBASE',10
	db '/SUBSYSTEM:CONSOLE,10.0',10,'/OUT:fatdemo.exe',10,'kernel32.lib',10
end virtual
