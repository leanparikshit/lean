import Int.
check | -2 |

-- Unfortunately, we can't write |-2|, because |- is considered a single token.
-- It is not wise to change that since the symbol |- can be used as the notation for
-- entailment relation in Lean.
eval |3|
definition x : Int := -3
check |x + 1|
check |x + 1| > 0
variable y : Int
check |x + y|
print |x + y| > x
set_option pp::notation false
print |x + y| > x
print |x + y| + |y + x| > x