; Read-only UEFI adapter and file reader. See README.md for setup and lifetime.
; Firmware calls require a live boot-services session; no Win32 dependencies.
include '../../common/policy.g'
include 'reader.inc'

; RCX=context*, RDX=volume LBA, R8=sector output -> EAX=F_*.
public guide_efi_read
; RCX=zeroed identity*, RDX=context*, R8=ops*, R9=FatWorkspace* -> EAX=F_*.
public guide_mount_readonly
; RCX=root handle*, RDX=UTF-16 component, R8=transfer* -> EAX=F_*.
public guide_read_root_range

extrn fat_mount
extrn fat_open
extrn fat_read_at
extrn fat_close

; RCX=initialized GuideEfiRead*, RDX=volume LBA, R8=sector output.
; EAX=F_*; output changes only after a successful complete firmware read.
; Caller serializes the context/bounce buffer and keeps boot services usable.
proc guide_efi_read uses rbx rsi rdi
	mov rbx, rcx
	mov rdi, r8
	mov eax, F_IO
	cmp dword [rbx + GuideEfiRead.online], 1
	jne .done
	mov eax, F_RANGE
	cmp rdx, [rbx + GuideEfiRead.sectors]
	jae .done
	add rdx, [rbx + GuideEfiRead.base_lba]
	jc .done
	mov r10, rdx                  ; survives earlier RCX/EDX setup
	fastcall [rbx + GuideEfiRead.read_blocks], \
		[rbx + GuideEfiRead.protocol], [rbx + GuideEfiRead.media_id], \
		r10, [rbx + GuideEfiRead.block_bytes], [rbx + GuideEfiRead.bounce]
	mov qword [rbx + GuideEfiRead.last_status], rax
	test rax, rax                 ; EFI_STATUS is 64 bits on x64
	jnz .failed
	mov rsi, qword [rbx + GuideEfiRead.bounce]
	mov ecx, dword [rbx + GuideEfiRead.block_bytes]
	rep movsb
	xor eax, eax
.done:
	ret
.failed:
	mov dword [rbx + GuideEfiRead.online], 0
	mov eax, F_IO
	jmp .done
endp
 ; RCX=zeroed, unused FatIdentity*; RDX=validated GuideEfiRead*;
; R8=caller-owned SectorOps* output; R9=FatWorkspace* with three sectors.
; All three objects are distinct and remain live for the entire mount.
; EAX=mount status; context validation/allocation is the loader's job.
proc guide_mount_readonly
	mov qword [r8 + SectorOps.context], rdx
	lea rax, [guide_efi_read]
	mov qword [r8 + SectorOps.read], rax
	mov qword [r8 + SectorOps.write], 0
	mov qword [r8 + SectorOps.begin], 0
	mov qword [r8 + SectorOps.end], 0
	mov qword [r8 + SectorOps.flush], 0
	mov rax, qword [rdx + GuideEfiRead.sectors]
	mov qword [r8 + SectorOps.sectors], rax
	mov eax, dword [rdx + GuideEfiRead.block_bytes]
	mov dword [r8 + SectorOps.sector_bytes], eax
	mov dword [r8 + SectorOps.reserved], 0
	fastcall fat_mount, rcx, r8, 0, r9 ; caller passes FatWorkspace* as fourth arg
	ret
endp
; RCX=live root handle*, RDX=UTF-16 component, R8=FatTransfer*.
; This convenience helper opens/closes one local handle. Repeated streaming
; callers retain their own handle and call fat_read_at directly.
proc guide_read_root_range uses rsi r12
	locals
		opened FatHandle
	endl
body:
	mov rsi, r8
	mov qword [opened.volume], 0
	mov dword [rsi + FatTransfer.done], 0
	fastcall fat_open, rcx, rdx, FH_READ, addr opened
	test eax, eax
	jnz .done
	fastcall fat_read_at, addr opened, rsi
	mov r12d, eax
	fastcall fat_close, addr opened
	test r12d, r12d
	cmovnz eax, r12d
.done:
	ret
endp
