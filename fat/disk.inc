; Private on-disk layout shared by interpretation and formatting. These are
; sector bytes, not caller ABI structures; FSInfo occupies the first 512 bytes
; even when the provider's sector is larger.
struct FDiskInfo
	?lead_signature dd ?
	?reserved_1 db 480 dup ?
	?structure_signature dd ?
	?free_count dd ?
	?next_free dd ?
	?reserved_2 db 12 dup ?
	?trail_signature dd ?
ends

FSI_LEAD_SIGNATURE      := 041615252h
FSI_STRUCTURE_SIGNATURE := 061417272h
FSI_TRAIL_SIGNATURE     := 0AA550000h
