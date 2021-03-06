local f  = Const("f")
local x  = Const("x")
local a  = Const("a")
local t1 = f(x, a, Var(1))
local t2 = Fun(x, Bool, t1)
assert(not t2:tag())
assert(not t2:binder_body():tag())
t2:set_tag(2)
t2:binder_body():set_tag(1)
assert(t2:tag() == 2)
assert(t2:binder_body():tag() == 1)
print(t2)
local new_t2 = t2:instantiate(Const("b"))
assert(new_t2:tag() == 2)
assert(new_t2:binder_body():tag() == 1)
assert(not (t2 == new_t2))

