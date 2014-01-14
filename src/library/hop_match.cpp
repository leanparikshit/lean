/*
Copyright (c) 2013 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include "util/optional.h"
#include "util/buffer.h"
#include "kernel/free_vars.h"
#include "kernel/instantiate.h"
#include "library/eq_heq.h"
#include "library/kernel_bindings.h"

namespace lean {

class hop_match_fn {
    buffer<optional<expr>> & m_subst;
    buffer<expr>             m_vars;

    bool is_free_var(expr const & x, unsigned ctx_size) const {
        return is_var(x) && var_idx(x) >= ctx_size;
    }

    bool is_locally_bound(expr const & x, unsigned ctx_size) const {
        return is_var(x) && var_idx(x) < ctx_size;
    }

    optional<expr> get_subst(expr const & x, unsigned ctx_size) const {
        lean_assert(is_free_var(x, ctx_size));
        unsigned vidx = var_idx(x) - ctx_size;
        unsigned sz = m_subst.size();
        if (vidx >= sz)
            throw exception("ill-formed higher-order matching problem");
        return m_subst[sz - vidx - 1];
    }

    bool has_locally_bound_var(expr const & t, unsigned ctx_size) const {
        return has_free_var(t, 0, ctx_size);
    }

    void assign(expr const & p, expr const & t, unsigned ctx_size) {
        lean_assert(is_free_var(p, ctx_size));
        lean_assert(!has_locally_bound_var(t, ctx_size));
        lean_assert(!get_subst(p, ctx_size));
        unsigned vidx = var_idx(p) - ctx_size;
        unsigned sz = m_subst.size();
        m_subst[sz - vidx - 1] = lower_free_vars(t, ctx_size, ctx_size);
    }

    bool args_are_distinct_locally_bound_vars(expr const & p, unsigned ctx_size, buffer<expr> & vars) {
        lean_assert(is_app(p));
        vars.clear();
        unsigned num = num_args(p);
        for (unsigned i = 1; i < num; i++) {
            expr const & v = arg(p, i);
            if (!is_locally_bound(v, ctx_size))
                return false;
            if (std::find(vars.begin(), vars.end(), v) != vars.end())
                return false;
            vars.push_back(v);
        }
        return true;
    }


    /**
       \brief Return t' when all locally bound variables in \c t occur in vars at positions [0, vars_size).
       The locally bound variables occurring in \c t are replaced using the following mapping:

             vars[vars_size - 1] ==> #0
             ...
             vars[0]             ==> #vars_size - 1

       None is returned if \c t contains a locally bound variable that does not occur in \c vars.
       Remark: vars_size <= vars.size()
     */
    optional<expr> proj_core(expr const & t, unsigned ctx_size, buffer<expr> const & vars, unsigned vars_size) {
        bool failed = false;
        expr r = replace(t, [&](expr const & e, unsigned offset) -> expr {
                if (is_var(e)) {
                    unsigned vidx = var_idx(e);
                    if (vidx < offset)
                        return e;
                    vidx -= offset;
                    if (vidx < ctx_size) {
                        // e is locally bound
                        for (unsigned i = 0; i < vars_size; i++) {
                            if (var_idx(vars[i]) == vidx)
                                return mk_var(offset + vars_size - i - 1);
                        }
                        failed = true;
                        return e;
                    } else if (ctx_size != vars_size) {
                        return mk_var(offset + vidx - ctx_size + vars_size);
                    } else {
                        return e;
                    }
                } else {
                    return e;
                }
            });
        if (failed)
            return none_expr();
        else
            return some_expr(r);
    }

    /**
       \brief Return <tt>(fun (x1 ... xn) t')</tt> if all locally bound variables in \c t occur in vars.
       \c n is the size of \c vars.
       None is returned if \c t contains a locally bound variable that does not occur in \c vars.
    */
    optional<expr> proj(expr const & t, context const & ctx, unsigned ctx_size, buffer<expr> const & vars) {
        optional<expr> t_prime = proj_core(t, ctx_size, vars, vars.size());
        if (!t_prime)
            return none_expr();
        expr r     = *t_prime;
        unsigned i = vars.size();
        while (i > 0) {
            --i;
            unsigned vidx = var_idx(vars[i]);
            auto p = lookup_ext(ctx, vidx);
            context_entry const & entry = p.first;
            context entry_ctx = p.second;
            if (!entry.get_domain())
                return none_expr();
            expr d     = *entry.get_domain();
            optional<expr> new_d = proj_core(d, entry_ctx.size(), vars, i);
            if (!new_d)
                return none_expr();
            r = mk_lambda(entry.get_name(), *new_d, r);
        }
        return some_expr(r);
    }

    bool match(expr const & p, expr const & t, context const & ctx, unsigned ctx_size) {
        lean_assert(ctx.size() == ctx_size);
        if (is_free_var(p, ctx_size)) {
            auto s = get_subst(p, ctx_size);
            if (s) {
                return match(lift_free_vars(*s, ctx_size), t, ctx, ctx_size);
            } else if (has_locally_bound_var(t, ctx_size)) {
                return false;
            } else {
                assign(p, t, ctx_size);
                return true;
            }
        } else if (is_app(p) && is_free_var(arg(p, 0), ctx_size)) {
            auto s = get_subst(arg(p, 0), ctx_size);
            unsigned num = num_args(p);
            if (s) {
                expr f     = lift_free_vars(*s, ctx_size);
                expr new_p = apply_beta(f, num - 1, &(arg(p, 1)));
                return match(new_p, t, ctx, ctx_size);
            } else {
                // Check if p is a higher-order pattern.
                // That is, all arguments are distinct locally bound variables
                if (args_are_distinct_locally_bound_vars(p, ctx_size, m_vars)) {
                    optional<expr> new_t = proj(t, ctx, ctx_size, m_vars);
                    if (new_t) {
                        assign(arg(p, 0), *new_t, ctx_size);
                        return true;
                    } else {
                        return false;
                    }
                }
            }
        }

        if (p == t)
            return true;

        if (is_eq_heq(p) && is_eq_heq(t)) {
            expr_pair p1 = eq_heq_args(p);
            expr_pair p2 = eq_heq_args(t);
            return match(p1.first, p2.first, ctx, ctx_size) && match(p1.second, p2.second, ctx, ctx_size);
        } else {
            if (p.kind() != t.kind())
                return false;
            switch (p.kind()) {
            case expr_kind::Var: case expr_kind::Constant: case expr_kind::Type:
            case expr_kind::Value: case expr_kind::MetaVar:
                return false;
            case expr_kind::App: {
                unsigned i1 = num_args(p);
                unsigned i2 = num_args(t);
                while (i1 > 0 && i2 > 0) {
                    --i1;
                    --i2;
                    if (i1 == 0 && i2 > 0) {
                        if (!match(arg(p, i1), mk_app(i2+1, begin_args(t)), ctx, ctx_size))
                            return false;
                    } else if (i2 == 0 && i1 > 0) {
                        if (!match(mk_app(i1+1, begin_args(p)), arg(t, i2), ctx, ctx_size))
                            return false;
                    } else {
                        if (!match(arg(p, i1), arg(t, i2), ctx, ctx_size))
                            return false;
                    }
                }
                return true;
            }
            case expr_kind::HEq:
                lean_unreachable(); break; // LCOV_EXCL_LINE
            case expr_kind::Lambda: case expr_kind::Pi:
                return
                    match(abst_domain(p), abst_domain(t), ctx, ctx_size) &&
                    match(abst_body(p), abst_body(t), extend(ctx, abst_name(t), abst_domain(t)), ctx_size+1);
            case expr_kind::Let:
                // TODO(Leo)
                return false;
            }
        }
        lean_unreachable();
    }
public:
    hop_match_fn(buffer<optional<expr>> & subst):m_subst(subst) {}

    bool operator()(expr const & p, expr const & t) {
        return match(p, t, context(), 0);
    }
};

bool hop_match(expr const & p, expr const & t, buffer<optional<expr>> & subst) {
    return hop_match_fn(subst)(p, t);
}

static int hop_match(lua_State * L) {
    int nargs = lua_gettop(L);
    expr p    = to_expr(L, 1);
    expr t    = to_expr(L, 2);
    int k     = 0;
    if (nargs == 3) {
        k = luaL_checkinteger(L, 3);
        if (k < 0)
            throw exception("hop_match, arg #3 must be non-negative");
    } else {
        k = free_var_range(p);
    }
    buffer<optional<expr>> subst;
    subst.resize(k);
    if (hop_match(p, t, subst)) {
        lua_newtable(L);
        int i = 1;
        for (auto s : subst) {
            if (s) {
                push_expr(L, *s);
            } else {
                lua_pushnil(L);
            }
            lua_rawseti(L, -2, i);
            i = i + 1;
        }
    } else {
        lua_pushnil(L);
    }
    return 1;
}

void open_hop_match(lua_State * L) {
    SET_GLOBAL_FUN(hop_match, "hop_match");
}
}