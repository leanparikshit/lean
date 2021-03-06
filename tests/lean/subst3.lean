import macros

theorem T (A : Type) (p : A -> Bool) (f : A -> A -> A) : forall x y z, p (f x x) → x = y → x = z → p (f y z) :=
   λ x y z, λ (H1 : p (f x x)) (H2 : x = y) (H3 : x = z),
      subst (subst H1 H2) H3

print environment 1.