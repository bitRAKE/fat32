; All filesystem knowledge is compiled into this one library object.
include 'common/policy.g'
include 'fat32.h'

; RCX=identity* out, RDX=SectorOps*, R8=OEM map or 0; EAX=status.
public fat_mount
; RCX=identity* in-out; EAX=F_OK. Drop caches/snapshots; no I/O.
public fat_invalidate
; RCX=identity*, EDX=FAT index, R8=uint32_t* out; EAX=status.
public fat_get
; RCX=identity*, EDX=first or 0, R8=uint32_t[2]* {count,last}; EAX=status.
public fat_chain
; RCX=identity*, EDX=first directory cluster, R8=cursor* out; EAX=status.
public fat_dir_open
; RCX=identity*, RDX=cursor* in-out, R8=entry* out; EAX=F_OK/F_END/error.
public fat_dir_next
; RCX=identity*, EDX=parent, R8=UTF-16 component, R9=entry* out; EAX=status.
public fat_lookup
; RCX=identity*, RDX=const entry*, R8=transfer* in-out; EAX=status.
public fat_read
; RCX=identity*, RDX=entry* in-out, R8=transfer* in-out; EAX=status.
public fat_write
; RCX=identity*, RDX=entry* in-out, R8=uint64 byte size; EAX=status.
public fat_resize
; RCX=identity*, RDX=entry* in-out, R8=const stamp*; EAX=status.
public fat_set_info
; RCX=identity*, EDX=parent, R8=const request*, R9=entry* out; EAX=status.
public fat_create
; RCX=identity*, RDX=const entry* to delete; EAX=status.
public fat_remove
; RCX=identity*, RDX=entry* in-out, R8=new UTF-16 component; EAX=status.
public fat_rename

include 'fat/volume.inc'
include 'fat/directory.inc'
include 'fat/file.inc'
include 'fat/create.inc'
