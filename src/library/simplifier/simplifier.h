/*
Copyright (c) 2014 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#pragma once
#include <memory>
#include "util/lua.h"
#include "kernel/environment.h"
#include "kernel/metavar.h"
#include "library/expr_pair.h"
#include "library/simplifier/rewrite_rule_set.h"

namespace lean {
class simplifier_monitor;

/** \brief Simplifier object cell. */
class simplifier_cell {
    friend class simplifier;
    class imp;
    std::unique_ptr<imp> m_ptr;
public:
    /**
       \brief Simplification result
    */
    class result {
        friend class imp;
        expr           m_expr;      // new expression
        optional<expr> m_proof;     // a proof that the m_expr is equal to the input (when m_proofs_enabled)
        bool           m_heq_proof; // true if the proof has type lhs == rhs (i.e., it is a heterogeneous equality)
        explicit result(expr const & out, bool heq_proof = false):m_expr(out), m_heq_proof(heq_proof) {}
        result(expr const & out, expr const & pr, bool heq_proof = false):m_expr(out), m_proof(pr), m_heq_proof(heq_proof) {}
        result(expr const & out, optional<expr> const & pr, bool heq_proof = false):
            m_expr(out), m_proof(pr), m_heq_proof(heq_proof) {}
        result update_expr(expr const & new_e) const { return result(new_e, m_proof, m_heq_proof); }
        result update_proof(expr const & new_pr) const { return result(m_expr, new_pr, m_heq_proof); }
    public:
        result() {}
        expr get_expr() const { return m_expr; }
        optional<expr> get_proof() const { return m_proof; }
        bool is_heq_proof() const { return m_heq_proof; }
    };

    simplifier_cell(ro_environment const & env, options const & o, unsigned num_rs, rewrite_rule_set const * rs,
                    std::shared_ptr<simplifier_monitor> const & monitor);
    result operator()(expr const & e, optional<ro_metavar_env> const & menv);
    void clear();

    unsigned get_depth() const;
    ro_environment const & get_environment() const;
    options const & get_options() const;
};

/** \brief Reference to simplifier object */
class simplifier {
    friend class ro_simplifier;
    std::shared_ptr<simplifier_cell> m_ptr;
public:
    typedef simplifier_cell::result result;
    simplifier(ro_environment const & env, options const & o, unsigned num_rs, rewrite_rule_set const * rs,
               std::shared_ptr<simplifier_monitor> const & monitor);
    simplifier_cell * operator->() const { return m_ptr.get(); }
    simplifier_cell & operator*() const { return *(m_ptr.get()); }
    result operator()(expr const & e, optional<ro_metavar_env> const & menv) {
        return (*m_ptr)(e, menv);
    }
};

/** \brief Read only reference to simplifier object */
class ro_simplifier {
    std::shared_ptr<simplifier_cell const> m_ptr;
public:
    typedef std::weak_ptr<simplifier_cell const> weak_ref;
    ro_simplifier(simplifier const & s);
    ro_simplifier(weak_ref const & s);
    explicit operator weak_ref() const { return weak_ref(m_ptr); }
    weak_ref to_weak_ref() const { return weak_ref(m_ptr); }
    simplifier_cell const * operator->() const { return m_ptr.get(); }
    simplifier_cell const & operator*() const { return *(m_ptr.get()); }
};

/**
   \brief Abstract class that specifies the interface for monitoring
   the behavior of the simplifier.
*/
class simplifier_monitor {
public:
    virtual ~simplifier_monitor() {}
    /**
       \brief This method is invoked to sign that the simplifier is starting to process the expression \c e.
    */
    virtual void pre_eh(ro_simplifier const & s, expr const & e) = 0;

    /**
       \brief This method is invoked to sign that \c e has be rewritten into \c new_e with proof \c pr.
       The proof is none if proof generation is disabled or if \c e and \c new_e are definitionally equal.
    */
    virtual void step_eh(ro_simplifier const & s, expr const & e, expr const & new_e, optional<expr> const & pr) = 0;
    /**
       \brief This method is invoked to sign that \c e has be rewritten into \c new_e using the conditional equation \c ceq.
    */
    virtual void rewrite_eh(ro_simplifier const & s, expr const & e, expr const & new_e, expr const & ceq, name const & ceq_id) = 0;

    enum class failure_kind { Unsupported, TypeMismatch, AssumptionNotProved, MissingArgument, LoopPrevention, AbstractionBody };

    /**
       \brief This method is invoked when the simplifier fails to rewrite an application \c e.
       \c i is the argument where the simplifier gave up, and \c k is the reason for failure.
       Two possible values are: Unsupported or TypeMismatch (may happen when simplifying terms that use dependent types).
    */
    virtual void failed_app_eh(ro_simplifier const & s, expr const & e, unsigned i, failure_kind k) = 0;

    /**
       \brief This method is invoked when the simplifier fails to apply a conditional equation \c ceq to \c e.
       The \c ceq may have several arguments, the value \c i is the argument where the simplifier gave up, and \c k is the reason for failure.
       The possible failure values are: AssumptionNotProved (failed to synthesize a proof for an assumption required by \c ceq),
       MissingArgument (failed to infer one of the arguments needed by the conditional equation), PermutationGe
       (the conditional equation is a permutation, and the result is not smaller in the term ordering, \c i is irrelevant in this case).
    */
    virtual void failed_rewrite_eh(ro_simplifier const & s, expr const & e, expr const & ceq, name const & ceq_id, unsigned i, failure_kind k) = 0;

    /**
       \brief This method is invoked when the simplifier fails to simplify an abstraction (Pi or Lambda).
       The possible failure values are: Unsupported, TypeMismatch, and AbstractionBody (failed to rewrite the body of the abstraction,
       this may happen when we are using dependent types).
    */
    virtual void failed_abstraction_eh(ro_simplifier const & s, expr const & e, failure_kind k) = 0;
};

class simplifier_stack_space_exception : public stack_space_exception {
public:
    simplifier_stack_space_exception();
    virtual char const * what() const noexcept;
    virtual exception * clone() const;
    virtual void rethrow() const;
};

simplifier::result simplify(expr const & e, ro_environment const & env, options const & pts,
                            unsigned num_rs, rewrite_rule_set const * rs,
                            optional<ro_metavar_env> const & menv = none_ro_menv(),
                            std::shared_ptr<simplifier_monitor> const & monitor = std::shared_ptr<simplifier_monitor>());
simplifier::result simplify(expr const & e, ro_environment const & env, options const & opts,
                            unsigned num_ns, name const * ns,
                            optional<ro_metavar_env> const & menv = none_ro_menv(),
                            std::shared_ptr<simplifier_monitor> const & monitor = std::shared_ptr<simplifier_monitor>());
void open_simplifier(lua_State * L);
/**
   \brief Associate the given simplifier monitor with the lua_State object \c L.
*/
void set_global_simplifier_monitor(lua_State * L, std::shared_ptr<simplifier_monitor> const & monitor);
/**
   \brief Return the simplifier monitor associated with the given lua_State.
   The result is nullptr if the lua_State object does not have a monitor associated with it.
*/
std::shared_ptr<simplifier_monitor> get_global_simplifier_monitor(lua_State * L);
/**
   \brief Return a simplifier object at position \c i on the Lua stack. If there is a nil stored
   on this position of the stack, then return \c get_global_simplifier_monitor.

   \remark This procedure throws an exception if the object stored at position \c i not a
   simplifier monitor nor nil.
*/
std::shared_ptr<simplifier_monitor> get_simplifier_monitor(lua_State * L, int i);
}
