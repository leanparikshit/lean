local env1 = empty_environment()
local env2 = add_decl(env1, mk_var_decl("A", Type))
assert(env2:is_descendant(env1))
assert(env2:is_descendant(env2))
assert(not env1:is_descendant(env2))
local env3 = env2:forget()
assert(not env3:is_descendant(env1))
assert(not env3:is_descendant(env2))
assert(env3:is_descendant(env3))
