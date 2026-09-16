; Link this object before fat32.obj/fat32.lib to exclude filesystem mutation.
; This is an optional consumer policy, never part of the default fat32.lib.
include '../../common/policy.g'
include '../../fat32.inc'

; Physical volume initialization.
public fat_format
public fat_format_verified
; Advanced mutation hooks.
public fat_put
public fat_put_checked
; Shared-handle mutations.
public fat_new
public fat_write_at
public fat_write_next
public fat_handle_resize
public fat_handle_set_info
public fat_handle_rename
public fat_unlink
; Snapshot mutations.
public fat_write
public fat_resize
public fat_set_info
public fat_create
public fat_remove
public fat_rename
; Ordered staging-provider construction and physical commits.
public fat_order_init
public fat_order_commit
public fat_order_commit_verified

section '.text$fat_new' code readable executable comdat any align 16
proc fat_new
    mov eax, F_READONLY
    ret
endp
section '.text$fat_write_at' code readable executable comdat any align 16
proc fat_write_at
    mov dword [rdx + FatTransfer.done], 0
    mov eax, F_READONLY
    ret
endp
section '.text$fat_write_next' code readable executable comdat any align 16
proc fat_write_next
    mov dword [rdx + FatBuffer.done], 0
    mov eax, F_READONLY
    ret
endp
section '.text$fat_handle_resize' code readable executable comdat any align 16
proc fat_handle_resize
    mov eax, F_READONLY
    ret
endp
section '.text$fat_handle_set_info' code readable executable comdat any align 16
proc fat_handle_set_info
    mov eax, F_READONLY
    ret
endp
section '.text$fat_handle_rename' code readable executable comdat any align 16
proc fat_handle_rename
    mov eax, F_READONLY
    ret
endp
section '.text$fat_unlink' code readable executable comdat any align 16
proc fat_unlink
    mov eax, F_READONLY
    ret
endp
section '.text$fat_write' code readable executable comdat any align 16
proc fat_write
    mov dword [r8 + FatTransfer.done], 0
    mov eax, F_READONLY
    ret
endp
section '.text$fat_resize' code readable executable comdat any align 16
proc fat_resize
    mov eax, F_READONLY
    ret
endp
section '.text$fat_set_info' code readable executable comdat any align 16
proc fat_set_info
    mov eax, F_READONLY
    ret
endp
section '.text$fat_create' code readable executable comdat any align 16
proc fat_create
    mov eax, F_READONLY
    ret
endp
section '.text$fat_remove' code readable executable comdat any align 16
proc fat_remove
    mov eax, F_READONLY
    ret
endp
section '.text$fat_rename' code readable executable comdat any align 16
proc fat_rename
    mov eax, F_READONLY
    ret
endp
section '.text$fat_order_init' code readable executable comdat any align 16
proc fat_order_init
    mov eax, F_READONLY
    ret
endp
section '.text$fat_order_commit' code readable executable comdat any align 16
proc fat_order_commit
    ; Required output is valid, just as for the default public ABI.
    mov qword [r8 + FatCommitReport.status], F_READONLY ; status + FE_NONE
    mov qword [r8 + FatCommitReport.phase], 0          ; phase + operation
    mov qword [r8 + FatCommitReport.lba], 0
    mov qword [r8 + FatCommitReport.writes], 0
    mov qword [r8 + FatCommitReport.flushes], 0
    mov eax, F_READONLY
    ret
endp
section '.text$fat_order_commit_verified' code readable executable comdat any align 16
proc fat_order_commit_verified
    mov qword [r8 + FatCommitReport.status], F_READONLY
    mov qword [r8 + FatCommitReport.phase], 0
    mov qword [r8 + FatCommitReport.lba], 0
    mov qword [r8 + FatCommitReport.writes], 0
    mov qword [r8 + FatCommitReport.flushes], 0
    mov eax, F_READONLY
    ret
endp

section '.text$fat_put' code readable executable comdat any align 16
proc fat_put
    mov eax, F_READONLY
    ret
endp
section '.text$fat_put_checked' code readable executable comdat any align 16
proc fat_put_checked
    mov eax, F_READONLY
    ret
endp

section '.text$fat_format' code readable executable comdat any align 16
proc fat_format
    mov qword [r9 + FatFormatReport.status], F_READONLY
    mov qword [r9 + FatFormatReport.phase], 0
    mov qword [r9 + FatFormatReport.lba], -1
    mov qword [r9 + FatFormatReport.writes], 0
    mov qword [r9 + FatFormatReport.flushes], 0
    mov qword [r9 + FatFormatReport.completed_writes], 0
    mov qword [r9 + FatFormatReport.completed_flushes], 0
    mov qword [r9 + FatFormatReport.reads], 0
    mov eax, F_READONLY
    ret
endp

section '.text$fat_format_verified' code readable executable comdat any align 16
proc fat_format_verified
    mov qword [r9 + FatFormatReport.status], F_READONLY
    mov qword [r9 + FatFormatReport.phase], 0
    mov qword [r9 + FatFormatReport.lba], -1
    mov qword [r9 + FatFormatReport.writes], 0
    mov qword [r9 + FatFormatReport.flushes], 0
    mov qword [r9 + FatFormatReport.completed_writes], 0
    mov qword [r9 + FatFormatReport.completed_flushes], 0
    mov qword [r9 + FatFormatReport.reads], 0
    mov eax, F_READONLY
    ret
endp
