; All filesystem knowledge is compiled into this one library object.
include 'common/policy.g'
include 'fat32.h'

public fat_mount
public fat_invalidate
public fat_get
public fat_chain
public fat_dir_open
public fat_dir_next
public fat_lookup
public fat_read
public fat_write
public fat_resize
public fat_set_info
public fat_create
public fat_remove
public fat_rename

include 'fat/volume.inc'
include 'fat/directory.inc'
include 'fat/file.inc'
include 'fat/create.inc'
