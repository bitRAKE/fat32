; One canonical object per open directory slot. Operation calls are serialized
; by the caller; fat_call_locked is an optional outer gate, not a spinlock.

; RCX=handle*. EAX=status, R10=volume*, R11=object* on success.
; Preserves RCX/RDX/R8/R9. No I/O, no dereference of an unchecked pool pointer.
section '.text$f_handle' code readable executable comdat align 16
proc f_handle
	mov r10, qword [rcx + FatHandle.volume]
	test r10, r10
	jz .stale
	mov rax, qword [rcx + FatHandle.epoch]
	test rax, rax
	jz .stale
	cmp rax, [r10 + FatVolume.epoch]
	jne .stale
	mov rax, qword [rcx + FatHandle.object]
	sub rax, [r10 + FatVolume.objects]
	jc .stale
	test al, 127
	jnz .stale
	shr rax, 7
	mov r11d, dword [r10 + FatVolume.capacity]
	cmp rax, r11
	jae .stale
	mov r11, qword [r10 + FatVolume.identity]
	test r11, r11
	jz .stale
	cmp qword [r11 + FatIdentity.shared], r10
	jne .stale
	cmp dword [r11 + FatIdentity.magic], F_MAGIC
	jne .stale
	mov rax, qword [r11 + FatIdentity.generation]
	cmp rax, [r10 + FatVolume.generation]
	jne .stale
	mov r11, qword [rcx + FatHandle.object]
	cmp dword [r11 + FatObject.live], 1
	jne .stale
	cmp dword [r11 + FatObject.references], 0
	je .stale
	mov rax, qword [rcx + FatHandle.incarnation]
	cmp rax, [r11 + FatObject.incarnation]
	jne .stale
	xor eax, eax
	ret
.stale:
	mov eax, F_STALE
	ret
endp

; EAX=status, R10=volume*, R11=object*. Preserves RDX/R8/R9, clobbers RCX.
; Require writable access and nonwrapping versions before mutation; readonly
; attribute rules belong to each operation.
section '.text$f_mutable' code readable executable comdat align 16
proc f_mutable
	fastcall f_handle, rcx
	test eax, eax
	jnz .done
	mov eax, F_READONLY
	test dword [rcx + FatHandle.access], FH_WRITE
	jz .done
	fastcall f_version_room, r11
.done:
	ret
endp

; RCX=object*, preserves RCX/RDX/R8/R9/R10/R11. No version counter wraps.
section '.text$f_version_room' code readable executable comdat align 16
proc f_version_room
	mov eax, F_RANGE
	cmp qword [rcx + FatObject.file_version], -1
	je .done
	cmp qword [rcx + FatObject.chain_version], -1
	je .done
	cmp qword [rcx + FatObject.directory_version], -1
	je .done
	xor eax, eax
.done:
	ret
endp

; RCX=volume*, RDX=parent object*, R8=record* -> R10=matching object or 0.
; Canonical metadata survives cache eviction; no refreshed copies are needed.
section '.text$f_find_object' code readable executable comdat align 16
proc f_find_object
	mov r10, qword [rcx + FatVolume.objects]
	mov r9d, dword [rcx + FatVolume.capacity]
.loop:
	cmp dword [r10 + FatObject.live], 1
	jne .next
	cmp qword [r10 + FatObject.parent], rdx
	jne .next
	mov rax, qword [r8 + FatRecord.sector]
	cmp rax, [r10 + FatRecord.sector]
	jne .next
	mov eax, dword [r8 + FatRecord.offset]
	cmp eax, [r10 + FatRecord.offset]
	je .done
.next:
	add r10, sizeof.FatObject
	dec r9d
	jnz .loop
	xor r10d, r10d
.done:
	ret
endp

; Reserve capacity without changing any state. RCX=volume*, RDX=parent*.
; EAX=status, R10=free object on success. Preserves RCX/RDX/R8.
section '.text$f_object_room' code readable executable comdat align 16
proc f_object_room
	mov eax, F_RANGE
	cmp qword [rcx + FatVolume.next_id], -1
	je .done
	cmp dword [rdx + FatObject.references], -1
	je .done
	mov r10, qword [rcx + FatVolume.objects]
	mov r9d, dword [rcx + FatVolume.capacity]
.loop:
	cmp dword [r10 + FatObject.live], 0
	je .ok
	add r10, sizeof.FatObject
	dec r9d
	jnz .loop
	mov eax, F_MEMORY
	jmp .done
.ok:
	xor eax, eax
.done:
	ret
endp

; RCX=volume*, RDX=parent*, R8=record*, R9=reserved object*. Cannot fail after
; f_object_room under the same operation lease. Copy only the 80-byte prefix.
section '.text$f_adopt_object' code readable executable comdat align 16
proc f_adopt_object uses rsi rdi
	mov r10, rcx
	mov rsi, r8
	mov rdi, r9
	mov ecx, sizeof.FatRecord
	rep movsb
	inc qword [r10 + FatVolume.next_id]
	mov rax, qword [r10 + FatVolume.next_id]
	mov qword [r9 + FatObject.incarnation], rax
	mov qword [r9 + FatObject.file_version], 1
	mov qword [r9 + FatObject.chain_version], 1
	mov qword [r9 + FatObject.directory_version], 1
	mov dword [r9 + FatObject.references], 0
	mov dword [r9 + FatObject.live], 1
	mov qword [r9 + FatObject.parent], rdx
	inc dword [rdx + FatObject.references]
	ret
endp

; RCX=volume*, RDX=live object*, R8=zero/closed output*, R9D=access 1..3.
; No I/O; output assigned only after capacity checks. EAX=status.
section '.text$f_bind_handle' code readable executable comdat align 16
proc f_bind_handle
	mov eax, F_BUSY
	cmp qword [r8 + FatHandle.volume], 0
	jne .done
	mov eax, F_ARGUMENT
	test r9d, r9d
	jz .done
	cmp r9d, 3
	ja .done
	mov eax, F_RANGE
	cmp dword [rdx + FatObject.references], -1
	je .done
	mov qword [r8 + FatHandle.volume], rcx
	mov qword [r8 + FatHandle.object], rdx
	mov rax, qword [rcx + FatVolume.epoch]
	mov qword [r8 + FatHandle.epoch], rax
	mov rax, qword [rdx + FatObject.incarnation]
	mov qword [r8 + FatHandle.incarnation], rax
	mov qword [r8 + FatHandle.position], 0
	mov dword [r8 + FatHandle.access], r9d
	mov dword [r8 + FatHandle.reserved], 0
	inc dword [rdx + FatObject.references]
	xor eax, eax
.done:
	ret
endp

; RCX=volume* zero on first use, RDX=mounted identity*, R8=pool*, R9D=count.
; Reinitialization retires handles via epoch. No active operation may exist.
section '.text$fat_volume_init' code readable executable comdat any align 16
proc fat_volume_init uses rdi
	; No calls: retain inputs in volatile registers while RCX/RDI clear the pool.
	mov r10, rcx
	mov eax, F_BUSY
	cmp dword [r10 + FatVolume.gate], 0
	jne .done
	cmp qword [r10 + FatVolume.identity], 0
	je .identity
	cmp qword [r10 + FatVolume.identity], rdx
	jne .done
.identity:
	mov eax, F_FORMAT
	cmp dword [rdx + FatIdentity.magic], F_MAGIC
	jne .done
	mov eax, F_BUSY
	cmp dword [rdx + FatIdentity.transaction], 0
	jne .done
	cmp qword [rdx + FatIdentity.shared], 0
	je .owner
	cmp qword [rdx + FatIdentity.shared], r10
	jne .done
.owner:
	mov eax, F_ARGUMENT
	test r9d, r9d
	jz .done
	mov eax, F_RANGE
	cmp qword [r10 + FatVolume.epoch], -1
	je .done
	mov ecx, r9d
	shl rcx, 7
	mov r11, r8
	add r11, rcx
	jc .done
	mov rdi, r8
	xor eax, eax
	rep stosb
	mov qword [r10 + FatVolume.identity], rdx
	mov qword [rdx + FatIdentity.shared], r10
	mov qword [r10 + FatVolume.objects], r8
	mov dword [r10 + FatVolume.capacity], r9d
	mov rax, qword [rdx + FatIdentity.generation]
	mov qword [r10 + FatVolume.generation], rax
	mov qword [r8 + FatRecord.generation], rax
	inc qword [r10 + FatVolume.epoch]
	mov qword [r10 + FatVolume.next_id], 1
	mov qword [r8 + FatRecord.sector], -1
	mov eax, dword [rdx + FatIdentity.root_cluster]
	mov dword [r8 + FatRecord.cluster], eax
	mov dword [r8 + FatRecord.parent], eax
	mov byte [r8 + FatRecord.raw+11], 10h
	mov qword [r8 + FatObject.incarnation], 1
	mov qword [r8 + FatObject.file_version], 1
	mov qword [r8 + FatObject.chain_version], 1
	mov qword [r8 + FatObject.directory_version], 1
	mov dword [r8 + FatObject.live], 1
	mov dword [r8 + FatObject.references], 1 ; root pin
	xor eax, eax
.done:
	ret
endp

; RCX=volume*, EDX=access, R8=closed output handle*. Check mount lifetime first.
section '.text$fat_root' code readable executable comdat any align 16
proc fat_root
	mov r9d, edx
	mov r10, qword [rcx + FatVolume.identity]
	mov eax, F_STALE
	cmp qword [rcx + FatVolume.epoch], 0
	je .done
	test r10, r10
	jz .done
	cmp qword [r10 + FatIdentity.shared], rcx
	jne .done
	cmp dword [r10 + FatIdentity.magic], F_MAGIC
	jne .done
	mov rdx, qword [r10 + FatIdentity.generation]
	cmp rdx, [rcx + FatVolume.generation]
	jne .done
	fastcall f_bind_handle, rcx, [rcx + FatVolume.objects], r8, r9d
.done:
	ret
endp

; RCX=volume*. Detach only when no handles, iterators or parent pins remain.
; The identity can then accept a different manager without a media remount.
section '.text$fat_volume_close' code readable executable comdat any align 16
proc fat_volume_close
	mov eax, F_STALE
	cmp qword [rcx + FatVolume.epoch], 0
	je .done
	mov r10, qword [rcx + FatVolume.identity]
	test r10, r10
	jz .stale
	cmp qword [r10 + FatIdentity.shared], rcx
	jne .stale
	mov rdx, qword [rcx + FatVolume.generation]
	cmp rdx, [r10 + FatIdentity.generation]
	jne .stale
	mov eax, F_BUSY
	cmp dword [rcx + FatVolume.gate], 0
	jne .done
	mov r11, qword [rcx + FatVolume.objects]
	cmp dword [r11 + FatObject.references], 1
	jne .done
	mov qword [r10 + FatIdentity.shared], 0
	mov qword [rcx + FatVolume.identity], 0
	xor eax, eax
.done:
	ret
.stale:
	mov qword [rcx + FatVolume.identity], 0
	jmp .done
endp

; RCX=parent handle*, RDX=UTF16 component, R8D=access, R9=closed output*.
; Dot components are deliberately excluded; callers retain parent handles.
section '.text$fat_open' code readable executable comdat any align 16
proc fat_open uses rbx rsi rdi rbp r13 r14 r15
	locals
		entry FatEntry
	endl
body:
	mov rbx, rdx
	mov esi, r8d
	mov r14, r9
	fastcall f_handle, rcx
	test eax, eax
	jnz .done
	mov rbp, r11
	mov r13, r10
	mov eax, F_ARGUMENT
	test dword [rcx + FatHandle.access], FH_READ
	jz .done
	test byte [rbp + FatRecord.raw+11], 10h
	jz .done
	test esi, esi
	jz .done
	cmp esi, 3
	ja .done
	mov eax, F_BUSY
	cmp qword [r14 + FatHandle.volume], 0
	jne .done
	cmp word [rbx], '.'
	jne .lookup
	cmp word [rbx+2], 0
	je .name
	cmp word [rbx+2], '.'
	jne .lookup
	cmp word [rbx+4], 0
	je .name
.lookup:
	fastcall fat_lookup, [r13 + FatVolume.identity], [rbp + FatRecord.cluster], rbx, addr entry
	test eax, eax
	jnz .done
	fastcall f_find_object, r13, rbp, addr entry
	test r10, r10
	jnz .bind
	fastcall f_object_room, r13, rbp
	test eax, eax
	jnz .done
	mov r15, r10
	fastcall f_adopt_object, r13, rbp, addr entry, r15
	mov r10, r15
.bind:
	fastcall f_bind_handle, r13, r10, r14, esi
.done:
	ret
.name:
	mov eax, F_NAME
	jmp .done
endp

; RCX=handle*. Close the reference and recursively release unreferenced parent
; pins. No media access. A stale handle can be cleared without touching a pool.
section '.text$fat_close' code readable executable comdat any align 16
proc fat_close
	fastcall f_handle, rcx
	mov qword [rcx + FatHandle.volume], 0
	test eax, eax
	jnz .done
.release:
	dec dword [r11 + FatObject.references]
	jnz .done
	mov dword [r11 + FatObject.live], 0
	mov r11, qword [r11 + FatObject.parent]
	test r11, r11
	jnz .release
.done:
	ret
endp

; RCX=handle*, RDX=FatRecord* out. No disk access or name buffer.
section '.text$fat_handle_info' code readable executable comdat any align 16
proc fat_handle_info uses rsi rdi
	fastcall f_handle, rcx
	test eax, eax
	jnz .done
	mov rdi, rdx
	mov rsi, r11
	mov ecx, sizeof.FatRecord
	rep movsb
.done:
	ret
endp

; Decode application metadata without making consumers parse directory bytes.
; RCX=handle*, RDX=FatStamp* out. File or directory, output only on success.
section '.text$fat_handle_get_stamp' code readable executable comdat any align 16
proc fat_handle_get_stamp
	fastcall f_handle, rcx
	test eax, eax
	jnz .done
	mov eax, dword [r11 + FatRecord.raw+14]
	mov dword [rdx + FatStamp.create_time], eax
	mov ax, word [r11 + FatRecord.raw+18]
	mov [rdx + FatStamp.access_date], ax
	mov eax, dword [r11 + FatRecord.raw+22]
	mov dword [rdx + FatStamp.write_time], eax
	mov al, [r11 + FatRecord.raw+13]
	mov [rdx + FatStamp.create_tenth], al
	mov al, [r11 + FatRecord.raw+11]
	mov [rdx + FatStamp.attributes], al
	xor eax, eax
.done:
	ret
endp

; RCX=handle*, RDX=uint64 position. Beyond EOF is allowed; writes bound growth.
section '.text$fat_seek' code readable executable comdat any align 16
proc fat_seek
	fastcall f_handle, rcx
	test eax, eax
	jnz .done
	mov qword [rcx + FatHandle.position], rdx
.done:
	ret
endp

; RCX=handle*, RDX=FatTransfer*. Explicit offset; independent position unchanged.
section '.text$fat_read_at' code readable executable comdat any align 16
proc fat_read_at
	mov dword [rdx + FatTransfer.done], 0
	fastcall f_handle, rcx
	test eax, eax
	jnz .done
	mov eax, F_ARGUMENT
	test dword [rcx + FatHandle.access], FH_READ
	jz .done
	mov r8, rdx
	fastcall f_read_record, [r10 + FatVolume.identity], r11, r8
.done:
	ret
endp

; RCX=object*, RDX=pre-operation {cluster,size} packed as one qword.
; Called only after success with version room checked before mutation.
section '.text$f_file_changed' code readable executable comdat align 16
proc f_file_changed
	inc qword [rcx + FatObject.file_version]
	cmp rdx, qword [rcx + FatRecord.cluster]
	je .done
	inc qword [rcx + FatObject.chain_version]
.done:
	xor eax, eax
	ret
endp

section '.text$fat_write_at' code readable executable comdat any align 16
proc fat_write_at uses rbx rsi rdi
	mov rsi, rdx
	mov dword [rsi + FatTransfer.done], 0
	fastcall f_mutable, rcx
	test eax, eax
	jnz .done
	mov rbx, r11
	mov rdi, qword [rbx + FatRecord.cluster]
	fastcall f_write_record, [r10 + FatVolume.identity], rbx, rsi
	test eax, eax
	jnz .done
	cmp dword [rsi + FatTransfer.length], 0
	je .done
	fastcall f_file_changed, rbx, rdi
.done:
	ret
endp

; RCX=handle*, RDX=uint64 new size.
section '.text$fat_handle_resize' code readable executable comdat any align 16
proc fat_handle_resize uses rbx
	fastcall f_mutable, rcx
	test eax, eax
	jnz .done
	mov rbx, r11
	mov r8, rdx
	fastcall f_resize_record, [r10 + FatVolume.identity], r11, r8
	test eax, eax
	jnz .done
	; Even equal-size resize may trim an overallocated tail.
	inc qword [rbx + FatObject.file_version]
	inc qword [rbx + FatObject.chain_version]
.done:
	ret
endp

; RCX=handle*, RDX=FatStamp*. Clearing readonly is permitted.
section '.text$fat_handle_set_info' code readable executable comdat any align 16
proc fat_handle_set_info uses rbx
	fastcall f_mutable, rcx
	test eax, eax
	jnz .done
	mov rbx, r11
	mov r8, rdx
	fastcall f_set_record, [r10 + FatVolume.identity], r11, r8
	test eax, eax
	jnz .done
	inc qword [rbx + FatObject.file_version]
.done:
	ret
endp

; RCX=handle*, RDX=FatBuffer*. Sequential position advances by completed bytes,
; including a read prefix on error. Reuse the same explicit-offset primitive.
section '.text$fat_read_next' code readable executable comdat any align 16
proc fat_read_next uses rbx rsi
	home_struct request, FatTransfer, 0
body:
	mov rbx, rcx
	mov rsi, rdx
	mov rax, qword [rdx + FatBuffer.data]
	mov qword [request.data], rax
	mov eax, dword [rdx + FatBuffer.length]
	mov dword [request.length], eax
	mov rax, qword [rcx + FatHandle.position]
	mov qword [request.offset], rax
	fastcall fat_read_at, rcx, addr request
	mov edx, dword [request.done]
	mov dword [rsi + FatBuffer.done], edx
	add qword [rbx + FatHandle.position], rdx
	ret
endp

section '.text$fat_write_next' code readable executable comdat any align 16
proc fat_write_next uses rbx rsi
	home_struct request, FatTransfer, 0
body:
	mov rbx, rcx
	mov rsi, rdx
	mov rax, qword [rdx + FatBuffer.data]
	mov qword [request.data], rax
	mov eax, dword [rdx + FatBuffer.length]
	mov dword [request.length], eax
	mov rax, qword [rcx + FatHandle.position]
	mov qword [request.offset], rax
	fastcall fat_write_at, rcx, addr request
	mov edx, dword [request.done]
	mov dword [rsi + FatBuffer.done], edx
	add qword [rbx + FatHandle.position], rdx
	ret
endp

; RCX=directory object*. Caller checked room; no other directory is retired.
section '.text$f_namespace_changed' code readable executable comdat align 16
proc f_namespace_changed
	inc qword [rcx + FatObject.file_version]
	inc qword [rcx + FatObject.chain_version]
	inc qword [rcx + FatObject.directory_version]
	xor eax, eax
	ret
endp

; RCX=parent handle*, RDX=FatCreate*, R8=closed output handle* (access=3).
; Reserve pool capacity before changing media. Publish only after acceptance.
section '.text$fat_new' code readable executable comdat any align 16
proc fat_new uses rbx rsi rdi rbp r13 r14
	locals
		entry FatEntry
	endl
body:
	mov rsi, rdx
	mov rdi, r8
	fastcall f_mutable, rcx
	test eax, eax
	jnz .done
	mov rbx, r10
	mov rbp, r11
	mov r13, qword [rbx + FatVolume.identity]
	mov eax, F_ARGUMENT
	test byte [rbp + FatRecord.raw+11], 10h
	jz .done
	mov eax, F_BUSY
	cmp qword [rdi + FatHandle.volume], 0
	jne .done
	fastcall f_object_room, rbx, rbp
	test eax, eax
	jnz .done
	mov r14, r10
	fastcall f_begin, r13
	test eax, eax
	jnz .done
	fastcall f_create, r13, [rbp + FatRecord.cluster], rsi, addr entry, 0
	fastcall f_finish, r13, eax
	test eax, eax
	jnz .done
	fastcall f_adopt_object, rbx, rbp, addr entry, r14
	fastcall f_namespace_changed, rbp
	fastcall f_bind_handle, rbx, r14, rdi, FH_READ or FH_WRITE
.done:
	ret
endp

; RCX=file/directory handle*, RDX=new component. Identity and child pins survive
; a name/slot change. Root cannot be renamed. Same-parent operation only.
section '.text$fat_handle_rename' code readable executable comdat any align 16
proc fat_handle_rename uses rbx rdi rbp
	mov r8, rdx
	fastcall f_mutable, rcx
	test eax, eax
	jnz .done
	mov rbx, r11
	mov rbp, qword [r10 + FatVolume.identity]
	mov rdi, qword [rbx + FatObject.parent]
	mov eax, F_ARGUMENT
	test rdi, rdi
	jz .done
	fastcall f_version_room, rdi
	test eax, eax
	jnz .done
	fastcall f_rename_record, rbp, rbx, r8
	test eax, eax
	jnz .done
	inc qword [rbx + FatObject.file_version]
	fastcall f_namespace_changed, rdi
.done:
	ret
endp

; RCX=parent handle*, RDX=component. Reject removal of any live object,
; including a directory pinned by a child/iterator. No open-unlinked lifetime.
section '.text$fat_unlink' code readable executable comdat any align 16
proc fat_unlink uses rbx rsi rdi rbp
	locals
		entry FatEntry
	endl
body:
	mov rsi, rdx
	fastcall f_mutable, rcx
	test eax, eax
	jnz .done
	mov rbx, r10
	mov rdi, r11
	mov rbp, qword [rbx + FatVolume.identity]
	mov eax, F_ARGUMENT
	test byte [rdi + FatRecord.raw+11], 10h
	jz .done
	fastcall fat_lookup, rbp, [rdi + FatRecord.cluster], rsi, addr entry
	test eax, eax
	jnz .done
	fastcall f_find_object, rbx, rdi, addr entry
	mov eax, F_BUSY
	test r10, r10
	jnz .done
	fastcall f_remove_record, rbp, addr entry
	test eax, eax
	jnz .done
	fastcall f_namespace_changed, rdi
.done:
	ret
endp

; RCX=directory handle*, RDX=zero/closed iterator*. Iterator owns a reference;
; it can survive closing the input handle and unrelated namespace mutations.
section '.text$fat_iter_open' code readable executable comdat any align 16
proc fat_iter_open uses rbx rsi rdi r12 rbp
	locals
		cursor FatCursor
	endl
body:
	mov r12, rdx
	fastcall f_handle, rcx
	test eax, eax
	jnz .done
	mov rbx, r10
	mov rbp, r11
	mov eax, F_ARGUMENT
	test dword [rcx + FatHandle.access], FH_READ
	jz .done
	test byte [rbp + FatRecord.raw+11], 10h
	jz .done
	mov eax, F_BUSY
	cmp qword [r12 + FatHandle.volume], 0
	jne .done
	fastcall fat_dir_open, [rbx + FatVolume.identity], [rbp + FatRecord.cluster], addr cursor
	test eax, eax
	jnz .done
	fastcall f_bind_handle, rbx, rbp, r12, FH_READ
	test eax, eax
	jnz .done
	mov rax, qword [rbp + FatObject.directory_version]
	mov qword [r12 + FatIterator.version], rax
	lea rsi, [cursor]
	lea rdi, [r12 + FatIterator.cursor]
	mov ecx, sizeof.FatCursor
	rep movsb
	xor eax, eax
.done:
	ret
endp

; RCX=iterator*, RDX=FatEntry* out. F_STALE only for its directory/lifetime.
section '.text$fat_iter_next' code readable executable comdat any align 16
proc fat_iter_next
	fastcall f_handle, rcx
	test eax, eax
	jnz .done
	mov r9, qword [rcx + FatIterator.version]
	mov eax, F_STALE
	cmp r9, [r11 + FatObject.directory_version]
	jne .done
	mov r9, rcx
	mov r8, rdx
	fastcall fat_dir_next, [r10 + FatVolume.identity], addr r9 + FatIterator.cursor, r8
.done:
	ret
endp

section '.text$fat_iter_close' code readable executable comdat any align 16
proc fat_iter_close
	fastcall fat_close, rcx
	ret
endp

; RCX=volume*, RDX=FatCall*. CAS once, return F_BUSY if leased, otherwise hold
; the gate through the entire call (including blocking/yielding callbacks).
; Caller-built target/arguments are trusted; target must return normally.
section '.text$fat_call_locked' code readable executable comdat any align 16
proc fat_call_locked uses rbx
	mov rbx, rcx
	mov r10, rdx
	xor eax, eax
	mov edx, 1
	lock cmpxchg dword [rbx + FatVolume.gate], edx
	jnz .busy
	fastcall [r10 + FatCall.target], [r10 + FatCall.arg1], [r10 + FatCall.arg2], [r10 + FatCall.arg3], [r10 + FatCall.arg4]
	mov dword [rbx + FatVolume.gate], 0
.done:
	ret
.busy:
	mov eax, F_BUSY
	jmp .done
endp
