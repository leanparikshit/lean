/*
Copyright (c) 2013 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include <vector>
#include <utility>
#include <functional>
#include <string>
#include "util/thread.h"
#include "util/map.h"
#include "util/sstream.h"
#include "util/exception.h"
#include "util/name_map.h"
#include "util/name_set.h"
#include "kernel/environment.h"
#include "kernel/expr_maps.h"
#include "kernel/expr_sets.h"
#include "kernel/kernel.h"
#include "kernel/io_state.h"
#include "library/io_state_stream.h"
#include "library/expr_pair.h"
#include "library/expr_pair_maps.h"
#include "library/arith/nat.h"
#include "library/arith/int.h"
#include "library/arith/real.h"
#include "frontends/lean/operator_info.h"
#include "frontends/lean/coercion.h"
#include "frontends/lean/frontend.h"
#include "frontends/lean/notation.h"
#include "frontends/lean/pp.h"

namespace lean {
class mark_implicit_command : public neutral_object_cell {
    name              m_obj_name;
    std::vector<bool> m_implicit;
public:
    mark_implicit_command(name const & n, unsigned sz, bool const * implicit):
        m_obj_name(n), m_implicit(implicit, implicit+sz) {}
    virtual ~mark_implicit_command() {}
    virtual char const * keyword() const { return "MarkImplicit"; }
    virtual void write(serializer & s) const {
        unsigned sz = m_implicit.size();
        s << "Imp" << m_obj_name << sz;
        for (auto b : m_implicit)
            s << b;
    }
};
static void read_mark_implicit(environment const & env, io_state const &, deserializer & d) {
    name n = read_name(d);
    buffer<bool> implicit;
    unsigned num = d.read_unsigned();
    for (unsigned i = 0; i < num; i++)
        implicit.push_back(d.read_bool());
    mark_implicit_arguments(env, n, implicit.size(), implicit.data());
}
static object_cell::register_deserializer_fn mark_implicit_ds("Imp", read_mark_implicit);

static std::vector<bool> g_empty_vector;
/**
   \brief Environment extension object for the Lean default frontend.
*/
struct lean_extension : public environment_extension {
    typedef std::pair<std::vector<bool>, name> implicit_info;
    // Remark: only named objects are stored in the dictionary.
    typedef name_map<operator_info>              operator_table;
    typedef name_map<implicit_info>              implicit_table;
    typedef name_map<unsigned>                   precedence_table;
    typedef expr_struct_map<list<operator_info>> expr_to_operators;
    typedef expr_pair_struct_map<expr>           coercion_map;
    typedef expr_struct_map<list<expr_pair>>     expr_to_coercions;
    typedef expr_struct_set coercion_set;
    typedef expr_struct_map<list<name>>          inv_aliases;

    operator_table        m_nud; // nud table for Pratt's parser
    operator_table        m_led; // led table for Pratt's parser
    // The following table stores the precedence of other operator parts.
    // The m_nud and m_led only map the first part of an operator to its definition.
    precedence_table      m_other_lbp;
    expr_to_operators     m_expr_to_operators; // map denotations to operators (this is used for pretty printing)
    implicit_table        m_implicit_table; // track the number of implicit arguments for a symbol.
    coercion_map          m_coercion_map; // mapping from (given_type, expected_type) -> coercion
    coercion_set          m_coercion_set; // Set of coercions
    expr_to_coercions     m_type_coercions; // mapping type -> list (to-type, function)
    name_set              m_explicit_names; // set of explicit version of constants with implicit parameters
    name_map<expr>        m_aliases;
    inv_aliases           m_inv_aliases;  // inverse map for m_aliases

    lean_extension() {
    }

    lean_extension const * get_parent() const {
        return environment_extension::get_parent<lean_extension>();
    }

    /** \brief Return the nud operator for the given symbol. */
    operator_info find_nud(name const & n) const {
        auto it = m_nud.find(n);
        if (it != m_nud.end())
            return it->second;
        lean_extension const * parent = get_parent();
        if (parent)
            return parent->find_nud(n);
        else
            return operator_info();
    }

    /** \brief Return the led operator for the given symbol. */
    operator_info find_led(name const & n) const {
        auto it = m_led.find(n);
        if (it != m_led.end())
            return it->second;
        lean_extension const * parent = get_parent();
        if (parent)
            return parent->find_led(n);
        else
            return operator_info();
    }

    optional<unsigned> get_other_lbp(name const & n) const {
        auto it = m_other_lbp.find(n);
        if (it != m_other_lbp.end())
            return some(it->second);
        lean_extension const * parent = get_parent();
        if (parent)
            return parent->get_other_lbp(n);
        else
            return optional<unsigned>();
    }

    /** \brief Return the precedence (aka binding power) of the given name. */
    optional<unsigned> get_lbp(name const & n) const {
        operator_info info = find_led(n);
        if (info)
            return some(info.get_precedence());
        else
            return get_other_lbp(n);
    }

    /**
        \brief Return true if the given operator is defined in this
        frontend object (parent frontends are ignored).
    */
    bool defined_here(operator_info const & n, bool led) const {
        if (led)
            return m_led.find(n.get_op_name()) != m_led.end();
        else
            return m_nud.find(n.get_op_name()) != m_nud.end();
    }

    /** \brief Return the led/nud operator for the given symbol. */
    operator_info find_op(name const & n, bool led) const {
        return led ? find_led(n) : find_nud(n);
    }

    /** \brief Insert a new led/nud operator. */
    void insert_op(operator_info const & op, bool led) {
        if (led)
            insert(m_led, op.get_op_name(), op);
        else
            insert(m_nud, op.get_op_name(), op);
    }

    /** \brief Find the operator that is used as notation for the given expression. */
    operator_info find_op_for(expr const & e, bool unicode) const {
        auto it = m_expr_to_operators.find(e);
        if (it != m_expr_to_operators.end()) {
            auto l = it->second;
            for (auto op : l) {
                if (unicode || op.is_safe_ascii())
                    return op;
            }
        }
        lean_extension const * parent = get_parent();
        if (parent)
            return parent->find_op_for(e, unicode);
        else
            return operator_info();
    }

    /** \brief Remove all internal denotations that are associated with the given operator symbol (aka notation) */
    void remove_bindings(operator_info const & op) {
        lean_extension const * parent = get_parent();
        for (expr const & d : op.get_denotations()) {
            if (parent && parent->find_op_for(d, true)) {
                // parent has an association for d... we must hide it.
                insert(m_expr_to_operators, d, list<operator_info>(operator_info()));
            } else {
                m_expr_to_operators.erase(d);
            }
        }
    }

    /** \brief Add a new entry d -> op in the mapping m_expr_to_operators */
    void insert_expr_to_operator_entry(expr const & d, operator_info const & op) {
        list<operator_info> & l = m_expr_to_operators[d];
        l = cons(op, l);
    }

    void check_precedence(name const & n, unsigned prec, io_state const & ios) const {
        auto old_prec = get_lbp(n);
        if (old_prec && *old_prec != prec)
            diagnostic(ios) << "The precedence of '" << n << "' changed from " << *old_prec << " to " << prec << ".\n";
    }

    /** \brief Register the new operator in the tables for parsing and pretty printing. */
    void register_new_op(operator_info new_op, expr const & d, bool led, io_state const & ios) {
        new_op.add_expr(d);
        insert_op(new_op, led);
        insert_expr_to_operator_entry(d, new_op);
        auto parts = new_op.get_op_name_parts();
        auto prec  = new_op.get_precedence();
        if (led)
            check_precedence(head(parts), prec, ios);
        for (name const & part : tail(parts)) {
            check_precedence(part, prec, ios);
            m_other_lbp[part] = prec;
        }
    }

    /**
        \brief Two operator (aka notation) denotations are compatible
        iff after ignoring all implicit arguments in the prefix and
        explicit arguments in the suffix, the remaining implicit arguments
        occur in the same positions.

        Let us denote implicit arguments with a '_' and explicit with a '*'.
        Then a denotation can be associated with a pattern containing one or more
        '_' and '*'.
        Two denotations are compatible, if we have the same pattern after
        removed the '_' from the prefix and '*' from the suffix.

        Here is an example of compatible denotations
                 f : Int -> Int -> Int              Pattern   * *
                 g : Pi {A : Type}, A -> A -> A     Pattern   _ * *
                 h : Pi {A B : Type}, A -> B -> A   Pattern   _ _ * *
            They are compatible, because after we remove the _ from the prefix, and * from the suffix,
            all of them reduce to the empty sequence

        Here is another example of compatible denotations:
                 f : Pi {A : Type} (a : A) {B : Type} (b : B), A    Pattern _ * _ *
                 g : Pi (i : Int) {T : Type} (x : T), T             Pattern * _ *
            They are compatible, because after we remove the _ from the prefix, and * from the suffix,
            we get the same sequence:  * _

        The following two are not compatible
                 f : Pi {A : Type} (a : A) {B : Type} (b : B), A    Pattern _ * _ *
                 g : Pi {A B : Type} (a : A) (b : B), A             Pattern _ _ * *

        Remark: we remove the explicit suffix at mark_implicit_arguments.
    */
    bool compatible_denotation(expr const & d1, expr const & d2) {
        auto imp1 = get_implicit_arguments(d1);
        auto imp2 = get_implicit_arguments(d2);
        auto it1  = std::find(imp1.begin(), imp1.end(), false);
        auto it2  = std::find(imp2.begin(), imp2.end(), false);
        for (; it1 != imp1.end() && it2 != imp2.end() && *it1 == *it2; ++it1, ++it2) {}
        return it1 == imp1.end() && it2 == imp2.end();
    }

    /**
        \brief Return true iff the existing denotations (aka
        overloads) for an operator op are compatible with the new
        denotation d.

        The compatibility is only an issue if implicit arguments are
        used. If one of the denotations has implicit arguments, then
        all of them should have implicit arguments, and the implicit
        arguments should occur in the same positions.
    */
    bool compatible_denotations(operator_info const & op, expr const & d) {
        return std::all_of(op.get_denotations().begin(), op.get_denotations().end(), [&](expr const & prev_d) { return compatible_denotation(prev_d, d); });
    }

    /**
        \brief Add a new operator and save information as object.

        If the new operator does not conflict with existing operators,
        then we just register it.
        If it conflicts, there are two options:
        1) It is an overload (we just add the internal name \c n as
        new option.
        2) It is a real conflict, and report the issue in the
        diagnostic channel, and override the existing operator (aka notation).
    */
    void add_op(operator_info new_op, expr const & d, bool led, environment const & env, io_state const & ios) {
        name const & opn = new_op.get_op_name();
        operator_info old_op = find_op(opn, led);
        if (!old_op) {
            register_new_op(new_op, d, led, ios);
        } else if (old_op == new_op) {
            if (compatible_denotations(old_op, d)) {
                // overload
                if (defined_here(old_op, led)) {
                    old_op.add_expr(d);
                    insert_expr_to_operator_entry(d, old_op);
                } else {
                    // we must copy the operator because it was defined in
                    // a parent frontend.
                    new_op = old_op.copy();
                    register_new_op(new_op, d, led, ios);
                }
            } else {
                diagnostic(ios) << "The denotation(s) for the existing notation:\n  " << old_op
                                << "\nhave been replaced with the new denotation:\n  " << d
                                << "\nbecause they conflict on how implicit arguments are used.\n";
                remove_bindings(old_op);
                register_new_op(new_op, d, led, ios);
            }
        } else {
            diagnostic(ios) << "Notation has been redefined, the existing notation:\n  " << old_op
                            << "\nhas been replaced with:\n  " << new_op << "\nbecause they conflict with each other.\n";
            remove_bindings(old_op);
            register_new_op(new_op, d, led, ios);
        }
        env->add_neutral_object(new notation_declaration(new_op, d));
    }

    static name mk_explicit_name(name const & n) {
        if (n.is_anonymous()) {
            throw exception("anonymous names cannot be used in definitions");
        } else if (n.is_numeral()) {
            return name(n, "explicit");
        } else {
            std::string new_name = "@";
            new_name += n.get_string();
            if (n.is_atomic())
                return name(new_name);
            else
                return name(n.get_prefix(), new_name.c_str());
        }
    }

    void mark_implicit_arguments(name const & n, unsigned sz, bool const * implicit, environment const & env) {
        if (env->has_children())
            throw exception(sstream() << "failed to mark implicit arguments, frontend object is read-only");
        object const & obj = env->get_object(n);
        if (obj.kind() != object_kind::Definition && obj.kind() != object_kind::Postulate && obj.kind() != object_kind::Builtin)
            throw exception(sstream() << "failed to mark implicit arguments, the object '" << n << "' is not a definition or postulate");
        if (has_implicit_arguments(n))
            throw exception(sstream() << "the object '" << n << "' already has implicit argument information associated with it");
        name explicit_version = mk_explicit_name(n);
        if (env->find_object(explicit_version))
            throw exception(sstream() << "failed to mark implicit arguments for '" << n << "', the frontend already has an object named '" << explicit_version << "'");
        expr const & type = obj.get_type();
        unsigned num_args = 0;
        expr it = type;
        while (is_pi(it)) { num_args++; it = abst_body(it); }
        if (sz > num_args)
            throw exception(sstream() << "failed to mark implicit arguments for '" << n << "', object has only " << num_args << " arguments, but trying to mark " << sz << " arguments");
        // remove explicit suffix
        while (sz > 0 && !implicit[sz - 1]) sz--;
        if (sz == 0)
            throw exception(sstream() << "failed to mark implicit arguments for '" << n << "', all arguments are explicit");
        std::vector<bool> v(implicit, implicit+sz);
        m_implicit_table[n] = mk_pair(v, explicit_version);
        expr body = mk_constant(n);
        m_explicit_names.insert(explicit_version);
        env->add_neutral_object(new mark_implicit_command(n, sz, implicit));
        env->auxiliary_section([&]() {
                if (obj.is_axiom() || obj.is_theorem()) {
                    env->add_theorem(explicit_version, type, body);
                } else {
                    env->add_definition(explicit_version, type, body);
                }
            });
    }

    bool has_implicit_arguments(name const & n) const {
        if (m_implicit_table.find(n) != m_implicit_table.end())
            return true;
        lean_extension const * parent = get_parent();
        if (parent)
            return parent->has_implicit_arguments(n);
        else
            return false;
    }

    std::vector<bool> const & get_implicit_arguments(name const & n) const {
        auto it = m_implicit_table.find(n);
        if (it != m_implicit_table.end())
            return it->second.first;
        lean_extension const * parent = get_parent();
        if (parent)
            return parent->get_implicit_arguments(n);
        else
            return g_empty_vector;
    }

    std::vector<bool> const & get_implicit_arguments(expr const & n) const {
        if (is_constant(n))
            return get_implicit_arguments(const_name(n));
        else
            return g_empty_vector;
    }

    name const & get_explicit_version(name const & n) const {
        auto it = m_implicit_table.find(n);
        if (it != m_implicit_table.end())
            return it->second.second;
        lean_extension const * parent = get_parent();
        if (parent)
            return parent->get_explicit_version(n);
        else
            return name::anonymous();
    }

    bool is_explicit(name const & n) const {
        if (m_explicit_names.find(n) != m_explicit_names.end())
            return true;
        lean_extension const * parent = get_parent();
        if (parent)
            return parent->is_explicit(n);
        else
            return false;
    }

    /**
        \brief It is too expensive to normalize \c t when checking if there is a coercion for it.
        So, we just do a 'quick' normalization following a chain of definitions.
    */
    expr coercion_type_normalization(expr t, ro_environment const & env) const {
        while (true) {
            if (is_constant(t)) {
                auto obj = env->find_object(const_name(t));
                if (obj && obj->is_definition()) {
                    t = obj->get_value();
                } else {
                    return t;
                }
            } else {
                return t;
            }
        }
    }

    void add_coercion(expr const & f, environment const & env) {
        expr type      = env->type_check(f);
        expr norm_type = type; // env->normalize(type);
        if (!is_arrow(norm_type))
            throw exception("invalid coercion declaration, a coercion must have an arrow type (i.e., a non-dependent functional type)");
        expr from      = coercion_type_normalization(abst_domain(norm_type), env);
        expr to        = coercion_type_normalization(abst_body(norm_type), env);
        if (from == to)
            throw exception("invalid coercion declaration, 'from' and 'to' types are the same");
        if (get_coercion_core(from, to))
            throw exception("invalid coercion declaration, frontend already has a coercion for the given types");
        m_coercion_map[expr_pair(from, to)] = f;
        m_coercion_set.insert(f);
        list<expr_pair> l = get_coercions_core(from);
        insert(m_type_coercions, from, cons(expr_pair(to, f), l));
        env->add_neutral_object(new coercion_declaration(f));
    }

    optional<expr> get_coercion_core(expr const & from_type, expr const & to_type) const {
        expr_pair p(from_type, to_type);
        auto it = m_coercion_map.find(p);
        if (it != m_coercion_map.end())
            return some_expr(it->second);
        lean_extension const * parent = get_parent();
        if (parent)
            return parent->get_coercion_core(from_type, to_type);
        else
            return none_expr();
    }

    optional<expr> get_coercion(expr const & from_type, expr const & to_type, ro_environment const & env) const {
        return get_coercion_core(coercion_type_normalization(from_type, env),
                                 coercion_type_normalization(to_type, env));
    }

    list<expr_pair> get_coercions_core(expr const & from_type) const {
        auto r = m_type_coercions.find(from_type);
        if (r != m_type_coercions.end())
            return r->second;
        lean_extension const * parent = get_parent();
        if (parent)
            return parent->get_coercions_core(from_type);
        else
            return list<expr_pair>();
    }

    list<expr_pair> get_coercions(expr const & from_type, ro_environment const & env) const {
        return get_coercions_core(coercion_type_normalization(from_type, env));
    }

    bool is_coercion(expr const & f) const {
        if (m_coercion_set.find(f) != m_coercion_set.end())
            return true;
        lean_extension const * parent = get_parent();
        return parent && parent->is_coercion(f);
    }

    optional<expr> get_alias(name const & n) const {
        auto it = m_aliases.find(n);
        if (it != m_aliases.end())
            return some_expr(it->second);
        lean_extension const * parent = get_parent();
        if (parent)
            return parent->get_alias(n);
        else
            return none_expr();
    }

    optional<list<name>> get_aliased(expr const & e) const {
        auto it = m_inv_aliases.find(e);
        if (it != m_inv_aliases.end())
            return optional<list<name>>(it->second);
        lean_extension const * parent = get_parent();
        if (parent)
            return parent->get_aliased(e);
        else
            return optional<list<name>>();
    }

    void add_alias(name const & n, expr const & e, environment const & env) {
        if (get_alias(n))
            throw exception(sstream() << "alias '" << n << "' was already defined");
        m_aliases[n]     = e;
        auto l = get_aliased(e);
        if (l)
            m_inv_aliases[e] = list<name>(n, *l);
        else
            m_inv_aliases[e] = list<name>(n);
        env->add_neutral_object(new alias_declaration(n, e));
    }
};

struct lean_extension_initializer {
    unsigned m_extid;
    lean_extension_initializer() {
        m_extid = environment_cell::register_extension([](){ return std::unique_ptr<environment_extension>(new lean_extension()); });
    }
};

static lean_extension_initializer g_lean_extension_initializer;

static lean_extension const & to_ext(ro_environment const & env) {
    return env->get_extension<lean_extension>(g_lean_extension_initializer.m_extid);
}

static lean_extension & to_ext(environment const & env) {
    return env->get_extension<lean_extension>(g_lean_extension_initializer.m_extid);
}

io_state init_frontend(environment const & env, bool no_kernel) {
    io_state ios(mk_pp_formatter(env));
    if (!no_kernel) {
        import_kernel(env, ios);
        import_nat(env, ios);
    }
    return ios;
}

io_state init_test_frontend(environment const & env) {
    env->set_trusted_imported(true);
    io_state ios = init_frontend(env);
    import_int(env, ios);
    import_real(env, ios);
    return ios;
}

void add_infix(environment const & env, io_state const & ios, name const & opn, unsigned p, expr const & d)  {
    to_ext(env).add_op(infix(opn, p), d, true, env, ios);
}
void add_infixl(environment const & env, io_state const & ios, name const & opn, unsigned p, expr const & d) {
    to_ext(env).add_op(infixl(opn, p), d, true, env, ios);
}
void add_infixr(environment const & env, io_state const & ios, name const & opn, unsigned p, expr const & d) {
    to_ext(env).add_op(infixr(opn, p), d, true, env, ios);
}
void add_prefix(environment const & env, io_state const & ios, name const & opn, unsigned p, expr const & d) {
    to_ext(env).add_op(prefix(opn, p), d, false, env, ios);
}
void add_postfix(environment const & env, io_state const & ios, name const & opn, unsigned p, expr const & d) {
    to_ext(env).add_op(postfix(opn, p), d, true, env, ios);
}
void add_mixfixl(environment const & env, io_state const & ios, unsigned sz, name const * opns, unsigned p, expr const & d) {
    to_ext(env).add_op(mixfixl(sz, opns, p), d, false, env, ios);
}
void add_mixfixr(environment const & env, io_state const & ios, unsigned sz, name const * opns, unsigned p, expr const & d) {
    to_ext(env).add_op(mixfixr(sz, opns, p), d, true, env, ios);
}
void add_mixfixc(environment const & env, io_state const & ios, unsigned sz, name const * opns, unsigned p, expr const & d) {
    to_ext(env).add_op(mixfixc(sz, opns, p), d, false, env, ios);
}
void add_mixfixo(environment const & env, io_state const & ios, unsigned sz, name const * opns, unsigned p, expr const & d) {
    to_ext(env).add_op(mixfixo(sz, opns, p), d, true, env, ios);
}
operator_info find_op_for(ro_environment const & env, expr const & n, bool unicode) {
    operator_info r = to_ext(env).find_op_for(n, unicode);
    if (r) {
        return r;
    } else if (is_constant(n)) {
        optional<object> obj = env->find_object(const_name(n));
        if (obj && obj->is_builtin() && obj->get_name() == const_name(n)) {
            // n is a constant that is referencing a builtin object.
            // If the notation is associated with the builtin object, we should try it.
            // TODO(Leo): Remark: in the new approach using .Lean files, the table is populated with constants.
            // So, this branch is not really needed anymore.
            return to_ext(env).find_op_for(obj->get_value(), unicode);
        } else {
            return r;
        }
    } else if (is_value(n)) {
        // Check whether the notation was declared for a constant referencing this builtin object.
        return to_ext(env).find_op_for(mk_constant(to_value(n).get_name()), unicode);
    } else {
        return r;
    }
}
operator_info find_nud(ro_environment const & env, name const & n) {
    return to_ext(env).find_nud(n);
}
operator_info find_led(ro_environment const & env, name const & n) {
    return to_ext(env).find_led(n);
}
optional<unsigned> get_lbp(ro_environment const & env, name const & n) {
    return to_ext(env).get_lbp(n);
}
void mark_implicit_arguments(environment const & env, name const & n, unsigned sz, bool const * implicit) {
    to_ext(env).mark_implicit_arguments(n, sz, implicit, env);
}
void mark_implicit_arguments(environment const & env, name const & n, std::initializer_list<bool> const & l) {
    mark_implicit_arguments(env, n, l.size(), l.begin());
}
bool has_implicit_arguments(ro_environment const & env, name const & n) {
    return to_ext(env).has_implicit_arguments(n);
}
std::vector<bool> const & get_implicit_arguments(ro_environment const & env, name const & n) {
    return to_ext(env).get_implicit_arguments(n);
}
std::vector<bool> const & get_implicit_arguments(ro_environment const & env, expr const & n) {
    if (is_constant(n))
        return get_implicit_arguments(env, const_name(n));
    else if (is_value(n))
        return get_implicit_arguments(env, to_value(n).get_name());
    else
        return g_empty_vector;
}
name const & get_explicit_version(ro_environment const & env, name const & n) {
    return to_ext(env).get_explicit_version(n);
}
bool is_explicit(ro_environment const & env, name const & n) {
    return to_ext(env).is_explicit(n);
}
void add_coercion(environment const & env, expr const & f) {
    to_ext(env).add_coercion(f, env);
}
optional<expr> get_coercion(ro_environment const & env, expr const & from_type, expr const & to_type) {
    return to_ext(env).get_coercion(from_type, to_type, env);
}
list<expr_pair> get_coercions(ro_environment const & env, expr const & from_type) {
    return to_ext(env).get_coercions(from_type, env);
}
bool is_coercion(ro_environment const & env, expr const & f) {
    return to_ext(env).is_coercion(f);
}
optional<expr> get_alias(ro_environment const & env, name const & n) {
    return to_ext(env).get_alias(n);
}
optional<list<name>> get_aliased(ro_environment const & env, expr const & e) {
    return to_ext(env).get_aliased(e);
}
void add_alias(environment const & env, name const & n, expr const & e) {
    to_ext(env).add_alias(n, e, env);
}
}
