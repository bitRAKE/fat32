; Read-only UEFI adapter and file reader. See README.md for setup and lifetime.
; Firmware calls require a live boot-services session; no Win32 dependencies.
include '../../common/policy.g'
include 'reader.inc'

; RCX=context*, RDX=volume LBA, R8=sector output -> EAX=F_*.
public guide_efi_read
; RCX=zeroed identity*, RDX=validated context*, R8=ops output -> EAX=F_*.
public guide_mount_readonly
; RCX=identity*, RDX=UTF-16 root component, R8=transfer* -> EAX=F_*.
public guide_read_root_range

extrn fat_mount
extrn fat_lookup
extrn fat_read

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
; R8=caller-owned SectorOps* output, not attached to another live mount.
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
	fastcall fat_mount, rcx, r8, 0 ; RDX gets ops before R8 becomes null OEM
	ret
endp
 ; RCX=mounted identity*, RDX=UTF-16 root component, R8=FatTransfer*.
; EAX=F_*; done=0 if lookup fails, otherwise fat_read's completed byte count.
; Entry is private to this call; output data may contain a prefix on read error.
proc guide_read_root_range uses rbx rsi
	locals
		entry FatEntry
	endl
body:
	mov rbx, rcx
	mov rsi, r8
	mov r9, rdx                  ; preserve name before EDX becomes parent
	mov dword [rsi + FatTransfer.done], 0
	fastcall fat_lookup, rcx, [rcx + FatIdentity.root_cluster], r9, addr entry
	test eax, eax
	jnz .done
	fastcall fat_read, rbx, addr entry, rsi
.done:
	ret
endp
