(* import("tactic.lua") *)
variables p q r : Bool

theorem T1 : p → p /\ q → r → q /\ r /\ p := _.
    (* Repeat(OrElse(conj_tac(), conj_hyp_tac(), assumption_tac())) *)
    done

-- Display proof term generated by previous tactic
print environment 1