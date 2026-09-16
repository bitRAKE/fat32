; Optional geometry/FAT read-only view. The source provider remains quiescent
; for the view's complete lifetime. See docs/CHECKING.md; no base state is added.
struct FatView
    ?identity FatIdentity
    ?provider SectorOps
ends
assert FatView.provider = 152
assert sizeof.FatView = 216

; Explicit volume-relative candidate BPB, never a partition/device selector.
struct FatBootSource
    ?sector dd ?
    ?reserved dd ?
    ?oem dq ?
ends
assert sizeof.FatBootSource = 16
