; Optional read-only candidate prefix. One stable provider lease covers plan/use.
FC_SALVAGE := 9
FC_PAIR_BUDGET := 21
struct FatExtent
    ?first dd ?
    ?count dd ?
ends
struct FatSalvageRequest
    ?extents dq ?
    ?capacity dd ?
    ?first dd ?
    ?bytes dd ?
    ?fat_budget dd ?
    ?pair_budget dd ?
    ?reserved dd ?
ends
struct FatSalvage
    ?check FatCheck
    ?extents dq ?
    ?capacity dd ?
    ?extent_count dd ?
    ?bytes dd ?
    ?available dd ?
    ?pairs dd ?
    ?pair_budget dd ?
ends
assert sizeof.FatExtent = 8
assert sizeof.FatSalvageRequest = 32
assert sizeof.FatSalvage = 144
