; Optional session policy; no core entry references this state or dispatcher.
FP_READ := 1
FP_WRITE := 2
FP_INSPECT := 3
FP_NO_WRITE := 1
FP_NO_READ := 2
struct FatPolicy
    ?volume dq ?
    ?identity dq ?
    ?generation dq ?
    ?epoch dq ?
    ?reports dq ?
    ?capacity dd ?
    ?count dd ?
    ?flags dd ?
    ?active dd ?
    ?cause_target dq ?
    ?cause FatCheck
ends
struct FatPolicyCall
    ?action FatCall
    ?subject dq ?
    ?kind dd ?
    ?invoked dd ?
    ?diagnosis dq ?
    ?report dq ?
    ?diagnosed dd ?
    ?reserved dd ?
ends
assert sizeof.FatPolicy = 176
assert sizeof.FatPolicyCall = 80
