; Generic sector staging; this layer knows no filesystem structures.
include 'fat32.h'
if ~ definite BUFFER_H
BUFFER_H := 1
struct SectorBuffer
	?ops       SectorOps
	?backend   dq ?
	?head      dq ?
	?mark      dq ?
	?pages     dq ?
	?limit     dq ?
	?active    dd ?
	?poisoned  dd ?
ends
struct SectorPage
	?next      dq ?
	?lba       dq ?
	?data      db F_MAX_SECTOR dup ?
ends
; Raw device is a volume handle, not a physical-disk handle. io storage must
; outlive its SectorBuffer. Write mode retains an exclusive lock until close.
struct WinVolume
	?ops       SectorOps
	?handle    dq ?
	?locked    dd ?
	?error     dd ?
	?bounce    dq ?
ends
end if
