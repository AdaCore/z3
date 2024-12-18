/*++
Copyright (c) 2024 Microsoft Corporation

Module Name:

    sls_seq_plugin.cpp

Abstract:

    Sequence/String SLS
    
Author:

    Nikolaj Bjorner (nbjorner) 2024-11-22

Notes:

Regex
  Assume regexes are ground and for zstring.
  to repair: 
     x in R
     - get prefix of x that can be in R
     - extend prefix by sampled string y, such that prefix(x)y in R

     x not in R:
     - assume x is in R, then 
       - sample prefix of x that is not in R
       - sample extension of x that is not in R
       - sample prefix of x in R, with extension not in R

     next_tokens(R) = { a | exists s: as in R }
     delta(a, R) = derivative of R with respect to a.
     delta(s, R) = delta(s[n-1], delta(s[0..n-2], R))
     nullable(R) = epsilon in R
     empty(R) = R is empty

     samples(x, R): 
        yield choose(R)
        for i in 0..|x|-1 & delta(x[0..i], R) != empty:
           yield x[0..i]choose(delta(x[0..i], R))
         
     choose(R):
       if nullable(R):
          return epsilon
       T = next_tokens(R)
       a = choose(T)  use a bias on characters that make progress (skip *).
       return choose(delta(a, R))

Sequences

Use length constraints as tabu for updates.

Alternate to lookahead strategy:
  Lookahead repair based of changing leaves:
  With each predicate, track the leaves of non-value arguments.
  Suppose x is a leaf string used in a violated predicate.
    then we can repair x by taking sub-string, or adding a character, 
    or adding x with an existing constant within the domain of known constants.
    or truncating x to the empty string.
  Suppose z is a leaf integer.
    we can increment, decrement z, set z to -1, 0, or a known bound.
  Lookahead works by updating strval1 starting from the leaf.
  - create a priority buffer array of vector<ptr_vector<expr>> based on depth.
  - walk from lowest depth up. Reset each inner buffer when processed. Parents always 
    have higher depth.
  - calculate repair/break depth when hitting a predicate based on bval1.
  - strval1 and bval1 are modified by 
    - use a global timestamp.
    - label each eval subterm by a timestamp that gets set.
    - strval0 evaluates to strval1 if timestamp matches global timestamp.
    
Revert bias on long strings:
- give preference to reset leaves that are assigned to long strings
- bake in bias for shorter strings into equation solving?

Equality solving using stochastic Nelson.
- When solving for an equality w = v, first convert them into two vectors by removing concatenations.
  The updates are then performed on the arguments to concatenations and not the concatenations themselves.
  This saves some amount of spurious work when pushing assignments down over concatenations, which is 
  what the current first version of the solver does.
- Given equality where current assignment does not satisfy it:
  - Xw = v:
     - let X' range over prefixes of X that matches v.
     - non-deterministic set X <- strval0(X')
     - non-deterministic set X <- strval0(X') + 'a' where strval0(X') + 'a' 
       matches prefix of strval0(v), and X' is longest prefix of X that matches v.
  - If X fully matches a prefix of v, then, in addition to the rules above:
    - consume constant character from strval0(X)w = v
    - reveal the next variable to solve for.

  - What scores make sense to use for partial solutions?

--*/

#include "ast/sls/sls_seq_plugin.h"
#include "ast/sls/sls_context.h"
#include "ast/ast_pp.h"
#include "ast/rewriter/seq_rewriter.h"
#include "ast/rewriter/th_rewriter.h"


namespace sls {
    
    seq_plugin::seq_plugin(context& c):
        plugin(c),
        seq(c.get_manager()),
        a(c.get_manager()) 
    {
        m_fid = seq.get_family_id();
    }
    
    void seq_plugin::propagate_literal(sat::literal lit) {
        SASSERT(ctx.is_true(lit));
        auto e = ctx.atom(lit.var());
        if (!is_seq_predicate(e))
            return;
        auto a = to_app(e);
        if (bval1(e) != lit.sign())
            return;
        ctx.new_value_eh(e);        
    }
    
    expr_ref seq_plugin::get_value(expr* e) {
        if (seq.is_string(e->get_sort()))
            return expr_ref(seq.str.mk_string(strval0(e)), m);

        NOT_IMPLEMENTED_YET();
        
        return expr_ref(m);
    }
    
    bool seq_plugin::propagate() {
        return false;
    }
    
    bool seq_plugin::is_sat() {

        for (expr* e : ctx.subterms()) {
            expr* x, * y, * z = nullptr;
            rational r;
            // coherence between string / integer functions is delayed
            // so we check and enforce it here.
            if (seq.str.is_length(e, x) && seq.is_string(x->get_sort())) {
                auto sx = strval0(x);
                auto ve = ctx.get_value(e);
                if (a.is_numeral(ve, r) && r == sx.length())
                    continue;
                // set e to length of x or
                // set x to a string of length e

                if (r == 0 || sx.length() == 0) {
                    verbose_stream() << "todo-create lemma: len(x) = 0 <=> x = \"\"\n";
                    // create a lemma: len(x) = 0 => x = ""
                }
                if (ctx.rand(2) == 0 && update(e, rational(sx.length())))
                    return false;
                if (r < sx.length() && update(x, sx.extract(0, r.get_unsigned())))
                    return false;
                if (update(e, rational(sx.length())))
                    return false;
                if (r > sx.length() && update(x, sx + zstring(m_chars[ctx.rand(m_chars.size())])))
                    return false;
                verbose_stream() << mk_pp(x, m) << " = " << sx << " " << ve << "\n";
                NOT_IMPLEMENTED_YET();
                return false;
            }

            if ((seq.str.is_index(e, x, y, z) || seq.str.is_index(e, x, y)) && seq.is_string(x->get_sort())) {
                auto sx = strval0(x);
                auto sy = strval0(y);
                rational val_z, val_e;
                if (z) {
                    VERIFY(a.is_numeral(ctx.get_value(z), val_z));
                }
                VERIFY(a.is_numeral(ctx.get_value(e), val_e));
                // case: x is empty, val_z = 0 
                if (val_e < 0 && (val_z < 0 || (val_z >= sx.length() && sx.length() > 0)))
                    continue;
                if (val_z.is_unsigned() && rational(sx.indexofu(sy, val_z.get_unsigned())) == val_e)
                    continue;
                if (val_z < 0 || (val_z >= sx.length() && sx.length() > 0)) 
                    update(e, rational(-1));                    
                else                 
                    update(e, rational(sx.indexofu(sy, val_z.get_unsigned())));
                return false;
            }
            // last-index-of
            // str-to-int
        }
        return true;
    }
    
    void seq_plugin::register_term(expr* e) {
        if (seq.is_string(e->get_sort())) {
            strval0(e) = strval1(e);
            for (unsigned i = 0; i < strval0(e).length(); ++i)
                m_chars.insert(strval0(e)[i]);

            if (is_app(e) && to_app(e)->get_family_id() == m_fid &&
                all_of(*to_app(e), [&](expr* arg) { return is_value(arg); })) 
                get_eval(e).is_value = true;            
        }   
    }
    
    std::ostream& seq_plugin::display(std::ostream& out) const {
        if (!m_chars.empty())
            out << "chars: " << m_chars << "\n";
        for (auto t : ctx.subterms()) {
            if (!seq.is_string(t->get_sort()))
                continue;
            if (m.is_value(t))
                continue;
            auto* ev = get_eval(t);
            if (!ev)
                continue;
            out << mk_pp(t, m) << " -> \"" << ev->val0.svalue << "\"";
            if (ev->min_length > 0)
                out << " min-length: " << ev->min_length;
            if (ev->max_length < UINT_MAX)
                out << " max-length: " << ev->max_length;
            out << "\n";
        }
    
        return out;
    }
    
    bool seq_plugin::set_value(expr* e, expr* v) {
        return false;
    }

    seq_plugin::eval& seq_plugin::get_eval(expr* e) {
        unsigned id = e->get_id();
        m_values.reserve(id + 1);
        if (!m_values[id]) 
            m_values.set(id, alloc(eval, m));
        return *m_values[id];
    }

    seq_plugin::eval* seq_plugin::get_eval(expr* e) const {
        unsigned id = e->get_id();
        return m_values.get(id, nullptr);
    }

    ptr_vector<expr> const& seq_plugin::lhs(expr* eq) {
        auto& ev = get_eval(eq);
        if (ev.lhs.empty()) {
            expr* x, * y;
            VERIFY(m.is_eq(eq, x, y));
            seq.str.get_concat(x, ev.lhs);
            seq.str.get_concat(y, ev.rhs);
        }
        return ev.lhs;
    }

    ptr_vector<expr> const& seq_plugin::concats(expr* x) {
        auto& ev = get_eval(x);
        if (ev.lhs.empty()) 
            seq.str.get_concat(x, ev.lhs);        
        return ev.lhs;
    }

    ptr_vector<expr> const& seq_plugin::rhs(expr* eq) {
        lhs(eq);
        auto& e = get_eval(eq);
        return e.rhs;
    }

    zstring& seq_plugin::strval0(expr* e) {
        SASSERT(seq.is_string(e->get_sort()));
        return get_eval(e).val0.svalue;
    }

    bool seq_plugin::is_seq_predicate(expr* e) {
        if (!is_app(e))
            return false;
        if (to_app(e)->get_family_id() == seq.get_family_id())
            return true;
        expr* x, *y;
        if (m.is_eq(e, x, y))
            return seq.is_seq(x->get_sort());
        if (m.is_distinct(e) && to_app(e)->get_num_args() > 0)
            return seq.is_seq(to_app(e)->get_arg(0));
        return false;
    }


    bool seq_plugin::bval1(expr* e) {
        SASSERT(is_app(e));
        if (to_app(e)->get_family_id() == seq.get_family_id())
            return bval1_seq(to_app(e));
        expr* x, * y;
        if (m.is_eq(e, x, y)) {
            if (seq.is_string(x->get_sort()))
                return strval0(x) == strval0(y);
            NOT_IMPLEMENTED_YET();
        }
        NOT_IMPLEMENTED_YET();
        return false;
    }

    bool seq_plugin::bval1_seq(app* e) {
        expr* a, *b;
        SASSERT(e->get_family_id() == seq.get_family_id());
        switch (e->get_decl_kind()) {
        case OP_SEQ_CONTAINS: 
            VERIFY(seq.str.is_contains(e, a, b));
            if (seq.is_string(a->get_sort())) 
                return strval0(a).contains(strval0(b));
            NOT_IMPLEMENTED_YET();
            break;
        case OP_SEQ_PREFIX:
            VERIFY(seq.str.is_prefix(e, a, b));
            if (seq.is_string(a->get_sort()))
                return strval0(a).prefixof(strval0(b));
            NOT_IMPLEMENTED_YET();
            break;
        case OP_SEQ_SUFFIX:
            VERIFY(seq.str.is_suffix(e, a, b));
            if (seq.is_string(a->get_sort()))
                return strval0(a).suffixof(strval0(b));
            NOT_IMPLEMENTED_YET();
            break;
        case OP_SEQ_IN_RE:
            VERIFY(seq.str.is_in_re(e, a, b));
            if (seq.is_string(a->get_sort()))
                return is_in_re(strval0(a), b);
            NOT_IMPLEMENTED_YET();
            break;
        case OP_SEQ_NTH:
        case OP_SEQ_NTH_I:
        case OP_SEQ_NTH_U:
        case OP_SEQ_FOLDL:
        case OP_SEQ_FOLDLI:
        case OP_STRING_LT:
        case OP_STRING_LE:
        case OP_STRING_IS_DIGIT:
            NOT_IMPLEMENTED_YET();
            break;
        default:
            UNREACHABLE();
            break;
        }
        return false;
    }

    zstring const& seq_plugin::strval1(expr* e) {
        SASSERT(is_app(e));
        SASSERT(seq.is_string(e->get_sort()));
        auto & ev = get_eval(e);
        if (ev.is_value)
            return ev.val0.svalue;
        
        if (to_app(e)->get_family_id() == seq.get_family_id()) {
            switch (to_app(e)->get_decl_kind()) {
            case OP_STRING_CONST: {
                zstring str;
                VERIFY(seq.str.is_string(e, str));
                ev.val0.svalue = str;
                return ev.val0.svalue;
            }
            case OP_SEQ_UNIT: {
                expr* arg = to_app(e)->get_arg(0);
                unsigned ch;
                if (seq.is_const_char(arg, ch)) {
                    zstring str(ch);
                    ev.val0.svalue = str;
                    return ev.val0.svalue;
                }
                NOT_IMPLEMENTED_YET();                
            }
            case OP_SEQ_EMPTY: {
                ev.val0.svalue = zstring();
                return ev.val0.svalue;
            }
            case OP_SEQ_CONCAT: {
                zstring r;
                for (auto arg : *to_app(e))
                    r = r + strval0(arg);
                ev.val1.svalue = r;
                return ev.val1.svalue;
            }
            case OP_SEQ_EXTRACT: {
                expr* x, * offset, * len;
                VERIFY(seq.str.is_extract(e, x, offset, len));
                zstring r = strval0(x);
                expr_ref offset_e = ctx.get_value(offset);
                expr_ref len_e = ctx.get_value(len);
                rational offset_val, len_val;
                VERIFY(a.is_numeral(offset_e, offset_val));
                VERIFY(a.is_numeral(len_e, len_val));
                if (offset_val.is_unsigned() && offset_val < r.length() &&
                    len_val.is_unsigned()) {
                    ev.val1.svalue = r.extract(offset_val.get_unsigned(), len_val.get_unsigned());
                    return ev.val1.svalue;
                }
                else {
                    ev.val1.svalue = zstring();
                    return ev.val1.svalue;
                }
            }
            case OP_SEQ_AT: {
                expr* x, * offset;
                VERIFY(seq.str.is_at(e, x, offset));
                zstring r = strval0(x);
                expr_ref offset_e = ctx.get_value(offset);
                rational offset_val;
                VERIFY(a.is_numeral(offset_e, offset_val));
                if (offset_val.is_unsigned() && offset_val < r.length()) {
                    ev.val1.svalue = zstring(r[offset_val.get_unsigned()]);
                    return ev.val1.svalue;
                }
                else {
                    ev.val1.svalue = zstring();
                    return ev.val1.svalue;
                }
            }
            case OP_SEQ_REPLACE: {
                expr* x, * y, * z;
                VERIFY(seq.str.is_replace(e, x, y, z));
                zstring r = strval0(x);
                zstring s = strval0(y);
                zstring t = strval0(z);
                ev.val1.svalue = r.replace(s, t);
                return ev.val1.svalue;
            }
            case OP_SEQ_NTH:
            case OP_SEQ_NTH_I:
            case OP_SEQ_NTH_U:
            case OP_SEQ_REPLACE_RE_ALL:
            case OP_SEQ_REPLACE_RE:
            case OP_SEQ_REPLACE_ALL:
            case OP_SEQ_MAP:
            case OP_SEQ_MAPI:
            case OP_SEQ_FOLDL:
            case OP_SEQ_FOLDLI:           
            case OP_RE_DERIVATIVE:
            case OP_STRING_ITOS:
            case OP_STRING_FROM_CODE:
            case OP_STRING_UBVTOS:
            case OP_STRING_SBVTOS:
                verbose_stream() << "strval1 " << mk_bounded_pp(e, m) << "\n";
                NOT_IMPLEMENTED_YET();
                break;
            case OP_RE_PLUS:
            case OP_RE_STAR:
            case OP_RE_OPTION:
            case OP_RE_RANGE:
            case OP_RE_CONCAT:
            case OP_RE_UNION:
            case OP_RE_DIFF:
            case OP_RE_INTERSECT:
            case OP_RE_LOOP:
            case OP_RE_POWER:
            case OP_RE_COMPLEMENT:
            case OP_RE_EMPTY_SET:
            case OP_RE_FULL_SEQ_SET:
            case OP_RE_FULL_CHAR_SET:
            case OP_RE_OF_PRED:
            case OP_RE_REVERSE:
            case OP_SEQ_TO_RE:
            case OP_SEQ_LENGTH:
            case OP_SEQ_INDEX:
            case OP_SEQ_LAST_INDEX:
            case OP_STRING_STOI:
            case OP_STRING_LT:
            case OP_STRING_LE:
            case OP_STRING_IS_DIGIT:
            case OP_STRING_TO_CODE:
                verbose_stream() << "strval1 " << mk_bounded_pp(e, m) << "\n";
                UNREACHABLE();
                break;
            default:
                UNREACHABLE();
                break;
            }
        }
        auto const& v = strval0(e);
        m_values[e->get_id()]->val1.svalue = v;
        return m_values[e->get_id()]->val1.svalue;
    }
    
    void seq_plugin::repair_up(app* e) {
        if (m.is_bool(e))
            return;
        if (is_value(e))
            return;
        if (seq.str.is_itos(e))
            repair_up_str_itos(e);
        else if (seq.str.is_stoi(e))
            repair_up_str_stoi(e);
        else if (seq.str.is_length(e))
            repair_up_str_length(e);
        else if (seq.str.is_index(e))
            repair_up_str_indexof(e);
        else if (seq.is_string(e->get_sort())) {
            strval0(e) = strval1(e);
            ctx.new_value_eh(e);
        }
        else 
            verbose_stream() << "repair up nyi: " << mk_bounded_pp(e, m) << "\n";
    }
    
    bool seq_plugin::repair_down(app* e) {
        if (m.is_bool(e) && bval1(e) == ctx.is_true(e))
            return true;
        if (seq.is_string(e->get_sort()) && strval0(e) == strval1(e))
            return true;
        if (e->get_family_id() == m_fid)
            return repair_down_seq(e);
        if (m.is_eq(e))
            return repair_down_eq(e);


        NOT_IMPLEMENTED_YET();
        return false;
    }

    bool seq_plugin::repair_down_str_length(app* e) {
        expr* x;
        VERIFY(seq.str.is_length(e, x));
        expr_ref len = ctx.get_value(e);
        rational r;
        unsigned len_u;
        VERIFY(a.is_numeral(len, r));
        if (!r.is_unsigned())
            return false;
        zstring val_x = strval0(x);
        len_u = r.get_unsigned();
        if (len_u == val_x.length())
            return true;
        if (len_u < val_x.length()) {
            for (unsigned i = 0; i + len_u < val_x.length(); ++i) 
                m_str_updates.push_back({ x, val_x.extract(i, len_u), 1 });            
        }
        if (!m_chars.empty()) {
            zstring ch(m_chars[ctx.rand(m_chars.size())]);
            m_str_updates.push_back({ x, val_x + ch, 1 });
            m_str_updates.push_back({ x, ch + val_x, 1 });            
        }
        return apply_update();
    }

    void seq_plugin::repair_up_str_stoi(app* e) {
        expr* x;
        VERIFY(seq.str.is_stoi(e, x));

        rational val_e;
        rational val_x(strval0(x).encode().c_str());
        VERIFY(a.is_numeral(ctx.get_value(e), val_e));
        if (val_e.is_unsigned() && val_e == val_x)
            return;
        if (val_x < 0)
            update(e, rational(0));
        else
            update(e, val_x);
    }

    void seq_plugin::repair_up_str_itos(app* e) {
        expr* x;
        VERIFY(seq.str.is_itos(e, x));
        rational val_x;
        VERIFY(a.is_numeral(ctx.get_value(x), val_x));
        rational val_e(strval0(e).encode().c_str());        
        if (val_x.is_unsigned() && val_x == val_e)
            return;
        if (val_x < 0)
            update(e, zstring());
        else
            update(e, zstring(val_x.to_string()));
    }

    void seq_plugin::repair_up_str_length(app* e) {
        expr* x;
        VERIFY(seq.str.is_length(e, x));
        zstring val_x = strval0(x);
        update(e, rational(val_x.length()));
    }

    void seq_plugin::repair_up_str_indexof(app* e) {
        expr* x, * y, * z = nullptr;
        VERIFY(seq.str.is_index(e, x, y, z) || seq.str.is_index(e, x, y));
        zstring val_x = strval0(x);
        zstring val_y = strval0(y);
        unsigned offset = 0;
        if (z) {
            rational r;
            VERIFY(a.is_numeral(ctx.get_value(z), r));
            if (!r.is_unsigned()) {
                update(e, rational(-1));
                return;
            }
            offset = r.get_unsigned();
        }
        int idx = val_x.indexofu(val_y, offset);
        update(e, rational(idx));
    }

    bool seq_plugin::repair_down_eq(app* e) {
        if (seq.is_string(e->get_arg(0)->get_sort()))
            return repair_down_str_eq(e);
        NOT_IMPLEMENTED_YET();
        return false;
    }

    bool seq_plugin::repair_down_str_eq(app* e) {
        bool is_true = ctx.is_true(e);
        expr* x, * y;
        VERIFY(m.is_eq(e, x, y));
        IF_VERBOSE(3, verbose_stream() << is_true << ": " << mk_bounded_pp(e, m, 3) << "\n");
        if (ctx.is_true(e)) {
            if (false && ctx.rand(2) != 0 && repair_down_str_eq_edit_distance_incremental(e))
                return true;
            if (ctx.rand(2) != 0 && repair_down_str_eq_edit_distance(e))
                return true;
            if (ctx.rand(2) != 0 && repair_down_str_eq_unify(e))
                return true;
            if (!is_value(x))
                m_str_updates.push_back({ x, strval1(y), 1 });
            if (!is_value(y))
                m_str_updates.push_back({ y, strval1(x), 1 });            
        }
        else {
            if (!is_value(x) && !m_chars.empty()) {
                zstring ch(m_chars[ctx.rand(m_chars.size())]);
                m_str_updates.push_back({ x, strval1(y) + ch, 1 });
                m_str_updates.push_back({ x, ch + strval1(y), 1 });
                m_str_updates.push_back({ x, ch, 1 });
                m_str_updates.push_back({ x, zstring(), 1 });
            }
            if (!is_value(y) && !m_chars.empty()) {
                zstring ch(m_chars[ctx.rand(m_chars.size())]);
                m_str_updates.push_back({ y, strval1(x) + ch, 1 });
                m_str_updates.push_back({ y, ch + strval1(x), 1 });
                m_str_updates.push_back({ x, ch, 1 });
                m_str_updates.push_back({ x, zstring(), 1});
            }
        }
        return apply_update();
    }

    /**
    * \brief compute the edit distance between two strings.
    */
    unsigned seq_plugin::edit_distance(zstring const& a, zstring const& b) {
        unsigned n = a.length();
        unsigned m = b.length();
        vector<unsigned_vector> d(n + 1);
        for (unsigned i = 0; i <= n; ++i)
            d[i].resize(m + 1, 0);
        for (unsigned i = 0; i <= n; ++i)
            d[i][0] = i;
        for (unsigned j = 0; j <= m; ++j)
            d[0][j] = j;
        for (unsigned j = 1; j <= m; ++j) {
            for (unsigned i = 1; i <= n; ++i) {
                if (a[i - 1] == b[j - 1])
                    d[i][j] = d[i - 1][j - 1];
                else
                    d[i][j] = 1 + std::min(std::min(d[i - 1][j], d[i][j - 1]), d[i - 1][j - 1]);
            }
        }
        return d[n][m];
    }


    void seq_plugin::add_edit_updates(ptr_vector<expr> const& w, zstring const& val, zstring const& val_other, uint_set const& chars) {
        for (auto x : w) {
            if (is_value(x))
                continue;
            zstring const & a = strval0(x);
            for (auto ch : chars)
                m_str_updates.push_back({ x, a + zstring(ch), 1 });
            for (auto ch : chars)
                m_str_updates.push_back({ x, zstring(ch) + a, 1 });
            if (a.length() > 0) {
                zstring b = a.extract(0, a.length() - 1);
                m_str_updates.push_back({ x, b, 1 }); // truncate a
                for (auto ch : chars)
                    m_str_updates.push_back({ x, b + zstring(ch), 1 }); // replace last character in a by ch
                b = a.extract(1, a.length() - 1);
                m_str_updates.push_back({ x, b, 1 }); // truncate a
                for (auto ch : chars)
                    m_str_updates.push_back({ x, zstring(ch) + b, 1 }); // replace first character in a by ch
            }
        }
        unsigned first_diff = UINT_MAX;
        for (unsigned i = 0; i < val.length() && i < val_other.length(); ++i) {
            if (val[i] != val_other[i]) {
                first_diff = i;
                break;
            }
        }
        if (first_diff != UINT_MAX) {
            unsigned index = first_diff;
            for (auto x : w) {
                auto const & val_x = strval0(x);
                auto len_x = val_x.length();
                if (index < len_x) {
                    if (is_value(x))
                        break;
                    auto new_val = val_x.extract(0, first_diff) + zstring(val_other[first_diff]) + val_x.extract(first_diff + 1, val_x.length());
                    m_str_updates.push_back({ x, new_val, 1 });
                    break;
                }
                index -= len_x;
            }
        }
        unsigned last_diff = 0;
        for (unsigned i = 1; i <= val.length() && i <= val_other.length(); ++i) {
            if (val[val.length() - i] != val_other[val_other.length() - i]) {
                last_diff = i;
                break;
            }
        }

#if 0
        if (last_diff != 0) {
            unsigned index = last_diff;
            for (auto x : w) {
                auto const& val_x = strval0(x);
                auto len_x = val_x.length();
                if (index < len_x) {
                    if (is_value(x))
                        break;
                    auto new_val = val_x.extract(0, len_x - last_diff) + zstring(val_other[val_other.length() - last_diff]) + val_x.extract(len_x - last_diff + 1, len_x);
                    m_str_updates.push_back({ x, new_val, 1 });
                    break;
                }
                index -= len_x;
            }
        }
#endif
    }

    void seq_plugin::init_string_instance(ptr_vector<expr> const& es, string_instance& a) {
        bool prev_is_var = false;        
        for (auto x : es) {
            auto const& val = strval0(x);
            auto len = val.length();         
            bool is_val = is_value(x);
            a.s += val;
            if (!prev_is_var && !is_val && !a.next_is_var.empty())
                a.next_is_var.back() = true;
            for (unsigned i = 0; i < len; ++i) {
                a.is_value.push_back(is_val);
                a.prev_is_var.push_back(false);
                a.next_is_var.push_back(false);
            }
            if (len > 0 && is_val && prev_is_var && !a.is_value.empty())
                a.prev_is_var[a.prev_is_var.size() - len] = true;
            prev_is_var = !is_val;
        }
    }


    /**
    * \brief edit distance with update calculation
    */
    unsigned seq_plugin::edit_distance_with_updates(string_instance const& a, string_instance const& b) {
        unsigned n = a.s.length();
        unsigned m = b.s.length();
        vector<unsigned_vector> d(n + 1); // edit distance
        vector<unsigned_vector> u(n + 1); // edit distance with updates.
        m_string_updates.reset();
        for (unsigned i = 0; i <= n; ++i) {
            d[i].resize(m + 1, 0);
            u[i].resize(m + 1, 0);
        }
        for (unsigned i = 0; i <= n; ++i)
            d[i][0] = i, u[i][0] = i;
        for (unsigned j = 0; j <= m; ++j)
            d[0][j] = j, u[0][j] = j;
        for (unsigned j = 1; j <= m; ++j) {
            for (unsigned i = 1; i <= n; ++i) {
                if (a.s[i - 1] == b.s[j - 1]) {
                    d[i][j] = d[i - 1][j - 1];
                    u[i][j] = u[i - 1][j - 1];
                }
                else {
                    u[i][j] = 1 + std::min(u[i - 1][j], std::min(u[i][j - 1], u[i - 1][j - 1]));
                    d[i][j] = 1 + std::min(d[i - 1][j], std::min(d[i][j - 1], d[i - 1][j - 1]));

                    if (d[i - 1][j] < u[i][j] && a.can_add(i - 1)) {
                        m_string_updates.reset();
                        u[i][j] = d[i - 1][j];
                    }
                    if (d[i][j - 1] < u[i][j] && b.can_add(i - 1)) {
                        m_string_updates.reset();
                        u[i][j] = d[i][j - 1];
                    }
                    if (d[i - 1][j - 1] < u[i][j] && (a.can_add(i - 1) || b.can_add(j - 1))) {
                        m_string_updates.reset();
                        u[i][j] = d[i - 1][j - 1];
                    }
                    if (d[i - 1][j] == u[i][j] && a.can_add(i - 1))
                        add_string_update(side_t::left, op_t::add, j - 1, i - 1);

                    if (d[i][j - 1] == u[i][j] && b.can_add(j - 1))
                        add_string_update(side_t::right, op_t::add, i - 1, j - 1);

                    if (d[i - 1][j] == u[i][j] && !a.is_value[i - 1])
                        add_string_update(side_t::left, op_t::del, i - 1, 0);

                    if (d[i][j - 1] == u[i][j] && !b.is_value[j - 1])
                        add_string_update(side_t::right, op_t::del, j - 1, 0);

                    if (d[i - 1][j - 1] == u[i][j] && !a.is_value[i - 1])
                        add_string_update(side_t::left, op_t::copy, j - 1, i - 1);

                    if (d[i - 1][j - 1] == u[i][j] && !b.is_value[j - 1])
                        add_string_update(side_t::right, op_t::copy, i - 1, j - 1);
                }
            }
        }
        return u[n][m];
    }


    bool seq_plugin::repair_down_str_eq_edit_distance_incremental(app* eq) {        
        auto const& L = lhs(eq);
        auto const& R = rhs(eq);
        string_instance a, b;
        init_string_instance(L, a);
        init_string_instance(R, b);
        
        if (a.s == b.s)
            return update(eq->get_arg(0), a.s) && update(eq->get_arg(1), b.s);

        unsigned diff = edit_distance_with_updates(a, b);


        verbose_stream() << "diff \"" << a.s << "\" \"" << b.s << "\" diff " << diff << " updates " << m_string_updates.size() << "\n";
#if 1
        for (auto const& [side, op, i, j] : m_string_updates) {
            switch (op) {
            case op_t::del:
                if (side == side_t::left)
                    verbose_stream() << "del " << a.s[i] << " @ " << i << " left\n";
                else
                    verbose_stream() << "del " << b.s[i] << " @ " << i << " right\n";
                break;
            case op_t::add:
                if (side == side_t::left)
                    verbose_stream() << "add " << b.s[i] << " @ " << j << " left\n";
                else
                    verbose_stream() << "add " << a.s[i] << " @ " << j << " right\n";
                break;
            case op_t::copy:
                if (side == side_t::left)
                    verbose_stream() << "copy " << b.s[i] << " @ " << j << " left\n";
                else
                    verbose_stream() << "copy " << a.s[i] << " @ " << j << " right\n";
                break;
            }
        }
#endif
        auto delete_char = [&](auto const& es, unsigned i) {
            for (auto x : es) {
                auto const& value = strval0(x);
                if (i >= value.length())
                    i -= value.length();
                else {
                    if (!is_value(x))
                        m_str_updates.push_back({ x, value.extract(0, i) + value.extract(i + 1, value.length()), 1 });
                    break;
                }
            }
        };

        auto add_char = [&](auto const& es, unsigned j, uint32_t ch) {
            for (auto x : es) {
                auto const& value = strval0(x);
                //verbose_stream() << "add " << j << " " << value << " " << value.length() << " " << is_value(x) << "\n";
                if (j > value.length() || (j == value.length() && j > 0)) {
                    j -= value.length();
                    continue;
                }
                if (!is_value(x))
                    m_str_updates.push_back({ x, value.extract(0, j) + zstring(ch) + value.extract(j, value.length()), 1 });
                if (j < value.length())
                    break;
            }
        };

        auto copy_char = [&](auto const& es, unsigned j, uint32_t ch) {
            for (auto x : es) {
                auto const& value = strval0(x);
                if (j >= value.length())
                    j -= value.length();
                else {
                    if (!is_value(x))
                        m_str_updates.push_back({ x, value.extract(0, j) + zstring(ch) + value.extract(j + 1, value.length()), 1 });
                    break;
                }
            }
        };

        for (auto& [side, op, i, j] : m_string_updates) {
            if (op == op_t::del && side == side_t::left)
                delete_char(L, i);
            else if (op == op_t::del && side == side_t::right)
                delete_char(R, i);
            else if (op == op_t::add && side == side_t::left)
                add_char(L, j, b.s[i]);
            else if (op == op_t::add && side == side_t::right)
                add_char(R, j, a.s[i]);
            else if (op == op_t::copy && side == side_t::left)
                copy_char(L, j, b.s[i]);
            else if (op == op_t::copy && side == side_t::right)
                copy_char(R, j, a.s[i]);
        }
        verbose_stream() << "num updates " << m_str_updates.size() << "\n";
        bool r = apply_update();
        verbose_stream() << "apply update " << r << "\n";
        //VERIFY(r);
        return r;
    }

    bool seq_plugin::repair_down_str_eq_edit_distance(app* eq) {
        auto const& L = lhs(eq);
        auto const& R = rhs(eq);
        zstring a, b;
        uint_set a_chars, b_chars;

        for (auto x : L) {
            for (auto ch : strval0(x))
                a_chars.insert(ch);
            a += strval0(x);
        }
        for (auto y : R) {
            for (auto ch : strval0(y))
                b_chars.insert(ch);
            b += strval0(y);
        }
        if (a == b)
            return update(eq->get_arg(0), a) && update(eq->get_arg(1), b);    

        unsigned diff = edit_distance(a, b);     

        //verbose_stream() << "solve: " << diff << " " << a << " " << b << "\n";

        add_edit_updates(L, a, b, b_chars);
        add_edit_updates(R, b, a, a_chars);

        for (auto& [x, s, score] : m_str_updates) {
            a.reset();
            b.reset();
            for (auto z : L) {
                if (z == x)
                    a += s;
                else
                    a += strval0(z);
            }
            for (auto z : R) {
                if (z == x)
                    b += s;
                else
                    b += strval0(z);
            }
            unsigned local_diff = edit_distance(a, b);

            //verbose_stream() << local_diff << " " << a << " " << b << "\n";
            if (local_diff > diff)
                score = 0.01;
            else if (local_diff == diff)
                score = 0.1;
            else 
                score = 2 * (diff - local_diff) * (diff - local_diff);            
        }
        return apply_update();
    }

    bool seq_plugin::repair_down_str_eq_unify(app* eq) {
        auto const& L = lhs(eq);
        auto const& R = rhs(eq);
        unsigned i = 0, j = 0;    // position into current string
        unsigned ni = 0, nj = 0;  // current string in concatenation
        double score = 1.0;       // priority of update. Doubled when score of equal strings are increased.

        IF_VERBOSE(4,
            verbose_stream() << "unify: \"" << strval0(eq->get_arg(0)) << "\" == \"" << strval0(eq->get_arg(1)) << "\"\n";
            for (auto x : L) verbose_stream() << mk_pp(x, m) << " ";
            verbose_stream() << " == ";
            for (auto x : R) verbose_stream() << mk_pp(x, m) << " ";
            verbose_stream() << "\n";
            for (auto x : L) verbose_stream() << "\"" << strval0(x) << "\" ";
            verbose_stream() << " == ";
            for (auto x : R) verbose_stream() << "\"" << strval0(x) << "\" ";
            verbose_stream() << "\n";
            );

        while (ni < L.size() && nj < R.size()) {
            auto const& xi = L[ni];
            auto const& yj = R[nj];
            auto const& vi = strval0(xi);
            auto const& vj = strval0(yj);

            if (vi.length() == i && vj.length() == j) {
                score *= 2;
                if (nj + 1 < R.size() && !strval0(R[nj + 1]).empty())
                    m_str_updates.push_back({ xi, vi + zstring(strval0(R[nj + 1])[0]), score });
                if (ni + 1 < L.size() && !strval0(L[ni + 1]).empty())
                    m_str_updates.push_back({ yj, vj + zstring(strval0(L[ni + 1])[0]), score });
                i = 0;
                j = 0;
                ++ni;
                ++nj;
                continue;
            }
            if (vi.length() == i) {
                // xi -> vi + vj[j]
                SASSERT(j < vj.length());
                m_str_updates.push_back({ xi, vi + zstring(vj[j]), score});
                score *= 2;
                i = 0;
                ++ni;
                continue;
            }
            if (vj.length() == j) {
                // yj -> vj + vi[i]
                SASSERT(i < vi.length());
                m_str_updates.push_back({ yj, vj + zstring(vi[i]), score });
                score *= 2;
                j = 0;
                ++nj;
                continue;
            }
            SASSERT(i < vi.length());
            SASSERT(j < vj.length());
            if (is_value(xi) && is_value(yj)) {
                if (vi[i] != vj[j])
                    score = 1;
                ++i, ++j;
                continue;
            }

            if (vi[i] == vj[j]) {
                ++i, ++j;
                continue;
            }
            if (!is_value(xi)) {
                m_str_updates.push_back({ xi, vi.extract(0, i), score });
                m_str_updates.push_back({ xi, vi.extract(0, i) + zstring(vj[j]), score});                
            }
            if (!is_value(yj)) {
                m_str_updates.push_back({ yj, vj.extract(0, j), score });
                m_str_updates.push_back({ yj, vj.extract(0, j) + zstring(vi[i]), score });
            }
            break;
        }
        for (; ni < L.size(); ++ni) 
            if (!is_value(L[ni]) && !strval0(L[ni]).empty())
                m_str_updates.push_back({ L[ni], zstring(), 1 });
        
        for (; nj < R.size(); ++nj)
            if (!is_value(R[nj]) && !strval0(R[nj]).empty())
                m_str_updates.push_back({ R[nj], zstring(), 1 });

        return apply_update();
    }

    bool seq_plugin::repair_down_seq(app* e) {
        switch (e->get_decl_kind()) {
        case OP_SEQ_CONTAINS:
            if (seq.is_string(to_app(e)->get_arg(0)->get_sort()))
                return repair_down_str_contains(e);
            break;
        case OP_SEQ_EMPTY:
            return true;
        case OP_SEQ_CONCAT:
            if (seq.is_string(e->get_sort()))
                return repair_down_str_concat(to_app(e));
            break;   
        case OP_SEQ_EXTRACT:
            if (seq.is_string(e->get_sort()))
                return repair_down_str_extract(e);
            break;
        case OP_SEQ_LENGTH:
            if (seq.is_string(to_app(e)->get_arg(0)->get_sort()))
                return repair_down_str_length(e);
            break;
        case OP_SEQ_PREFIX:
            if (seq.is_string(to_app(e)->get_arg(0)->get_sort()))
                return repair_down_str_prefixof(e);
            break;
        case OP_SEQ_SUFFIX:
            if (seq.is_string(to_app(e)->get_arg(0)->get_sort()))
                return repair_down_str_suffixof(e);
            break;
        case OP_SEQ_AT:
            if (seq.is_string(to_app(e)->get_arg(0)->get_sort()))
                return repair_down_str_at(e);
            break;
        case OP_SEQ_INDEX:
            if (seq.is_string(to_app(e)->get_arg(0)->get_sort()))
                return repair_down_str_indexof(e);
            break;
        case OP_STRING_CONST:
            UNREACHABLE();
            break;
        case OP_STRING_ITOS:
            return repair_down_str_itos(e);
        case OP_STRING_STOI:
            return repair_down_str_stoi(e);
        case OP_SEQ_IN_RE:
            if (seq.is_string(to_app(e)->get_arg(0)->get_sort()))
                return repair_down_in_re(e);
            break;
        case OP_SEQ_REPLACE:
            if (seq.is_string(e->get_sort()))
                return repair_down_str_replace(e);
            break;
        case OP_STRING_UBVTOS:
        case OP_STRING_SBVTOS:
        case OP_STRING_TO_CODE:
        case OP_STRING_FROM_CODE:
        case OP_SEQ_UNIT:
        case OP_SEQ_NTH:
        case OP_SEQ_NTH_I:
        case OP_SEQ_NTH_U:

        case OP_SEQ_REPLACE_RE_ALL:
        case OP_SEQ_REPLACE_RE:
        case OP_SEQ_REPLACE_ALL:
        case OP_SEQ_MAP:
        case OP_SEQ_MAPI:
        case OP_SEQ_FOLDL:
        case OP_SEQ_FOLDLI:

        case OP_SEQ_TO_RE:
        case OP_RE_PLUS:            
        case OP_RE_STAR:
        case OP_RE_OPTION:
        case OP_RE_RANGE:
        case OP_RE_CONCAT:
        case OP_RE_UNION:
        case OP_RE_DIFF:
        case OP_RE_INTERSECT:
        case OP_RE_LOOP:
        case OP_RE_POWER:
        case OP_RE_COMPLEMENT:
        case OP_RE_EMPTY_SET:
        case OP_RE_FULL_SEQ_SET:
        case OP_RE_FULL_CHAR_SET:
        case OP_RE_OF_PRED:
        case OP_RE_REVERSE:
        case OP_RE_DERIVATIVE:
        case OP_STRING_LT:
        case OP_STRING_LE:
        case OP_STRING_IS_DIGIT:            
            break;
        default:    
            verbose_stream() << "unexpected repair down " << mk_bounded_pp(e, m) << "\n";
            UNREACHABLE();
        }
        verbose_stream() << "nyi repair down " << mk_bounded_pp(e, m) << "\n";
        return false;
    }

    bool seq_plugin::repair_down_str_replace(app* e) {
        expr* x, * y, * z;
        VERIFY(seq.str.is_replace(e, x, y, z));
        zstring r = strval0(e);
        if (r == strval1(e))
            return true;
        if (!is_value(x))
            m_str_updates.push_back({ x, r, 1 });
        if (!is_value(y))
            m_str_updates.push_back({ y, zstring(), 1});
        if (!is_value(z))
            m_str_updates.push_back({ z, zstring(), 1 });

        // TODO some more possible ways, also deal with y, z if they are not values.
        // apply reverse substitution of r to replace z by y, update x to this value
        // update x using an edit distance reducing update based on the reverse substitution.
        // reverse substitution isn't unique, so take into account different possibilities (randomly).
        return apply_update();
    }

    bool seq_plugin::repair_down_str_itos(app* e) {
        expr* x;
        VERIFY(seq.str.is_itos(e, x));
        zstring se = strval0(e);
        rational r(se.encode().c_str());
        if (r.is_int())
            m_int_updates.push_back({ x, r, 1 });
        else
            m_int_updates.push_back({ x, rational(-1 - ctx.rand(10)), 1 });
        return apply_update();
    }

    bool seq_plugin::repair_down_str_stoi(app* e) {
        expr* x;
        rational r;
        VERIFY(seq.str.is_stoi(e, x));
        VERIFY(a.is_numeral(ctx.get_value(e), r) && r.is_int());
        if (r < 0)
            return false;
        zstring r_val(r.to_string());
        m_str_updates.push_back({ x, r_val, 1 });
        return apply_update();        
    }

    bool seq_plugin::repair_down_str_at(app* e) {
        expr* x, * y;
        VERIFY(seq.str.is_at(e, x, y));
        zstring se = strval0(e);
        verbose_stream() << "repair down at " << mk_pp(e, m) << " " << se << "\n";
        if (se.length() > 1)
            return false;
        zstring sx = strval0(x);
        unsigned lenx = sx.length();
        expr_ref idx = ctx.get_value(y);
        rational r;
        VERIFY(a.is_numeral(idx, r));

        if (se.length() == 0) {
            // index should be out of bounds of a.
            if (!is_value(x)) {
                m_str_updates.push_back({ x, zstring(), 1 });
                if (lenx > r && r >= 0) 
                    m_str_updates.push_back({ x, sx.extract(0, r.get_unsigned()), 1 });                
            }
            if (!m.is_value(y)) {
                m_int_updates.push_back({ y, rational(lenx), 1 });
                m_int_updates.push_back({ y, rational(lenx + 1), 1 });
                m_int_updates.push_back({ y, rational(-1), 1 });
            }
        }
        else {
            SASSERT(se.length() == 1);
            // index should be in bounds of a.
            if (!is_value(x)) {
                if (lenx > r && r >= 0) {
                    zstring new_x = sx.extract(0, r.get_unsigned()) + se + sx.extract(r.get_unsigned() + 1, lenx);
                    m_str_updates.push_back({ x, new_x, 1 });
                }
                if (lenx <= r) {
                    zstring new_x = sx + se;
                    m_str_updates.push_back({ x, new_x, 1 });
                }
            }
            if (!m.is_value(y)) {
                for (unsigned i = 0; i < sx.length(); ++i) {
                    if (se[0] == sx[i])
                        m_int_updates.push_back({ y, rational(i), 1 });
                }
            }
        }
        return apply_update();
    }

    bool seq_plugin::repair_down_str_indexof(app* e) {
        expr* x, * y, * offset = nullptr;
        VERIFY(seq.str.is_index(e, x, y, offset) || seq.str.is_index(e, x, y));
        rational value;
        VERIFY(a.is_numeral(ctx.get_value(e), value) && value.is_int());        
        zstring sx = strval0(x);
        zstring sy = strval0(y);
        unsigned lenx = sx.length();
        unsigned leny = sy.length();
        rational offset_r(0);
        if (offset) 
            VERIFY(a.is_numeral(ctx.get_value(offset), offset_r));
        unsigned offset_u = 0;
        if (offset_r.is_unsigned())
            offset_u = offset_r.get_unsigned();

        // set to not a member:
        if (value == -1) {
            m_str_updates.push_back({ y, zstring(m_chars[ctx.rand(m_chars.size())]), 1 });
            if (lenx > 0)
                m_str_updates.push_back({ x, zstring(), 1 }); 
        }
        // change x:
        // insert y into x at offset
        if (offset_r.is_unsigned() && 0 <= value && offset_u + value <= lenx && leny > 0) {
            unsigned offs = offset_u + value.get_unsigned();
            zstring prefix = sx.extract(0, offs);
            for (unsigned i = 0; i <= leny && offs + i < lenx; ++i) {
                zstring suffix = sx.extract(offs + i, lenx);
                m_str_updates.push_back({ x, prefix + sy + suffix, 1 });
            }
        }

        // change y:
        // replace y by substring of x at offset
        if (offset_r.is_unsigned() && 0 <= value && offset_u + value < lenx) {
            unsigned offs = offset_u + value.get_unsigned();
            for (unsigned i = offs; i < lenx; ++i) 
                m_str_updates.push_back({ y, sx.extract(offs, i - offs + 1), 1 });            
        }

        // change offset:
        // update offset such that value can be the index of y in x at offset
        for (int i = sx.indexofu(sy, 0); leny > 0 && value >= 0 && i >= 0; ++i, i = sx.indexofu(sy, i)) 
            if (value < i)
                m_int_updates.push_back({ offset, rational(i) - value, 1 });
                
        return apply_update();
    }

    bool seq_plugin::repair_down_str_prefixof(app* e) {
        expr* a, * b;
        VERIFY(seq.str.is_prefix(e, a, b));
        zstring sa = strval0(a);
        zstring sb = strval0(b);
        unsigned lena = sa.length();
        unsigned lenb = sb.length();
        verbose_stream() << "repair prefixof " << mk_bounded_pp(e, m) << "\n";
        if (ctx.is_true(e)) {
            unsigned n = std::min(lena, lenb);
            if (!is_value(a)) {                
                for (unsigned i = 0; i < n; ++i)
                    m_str_updates.push_back({ a, sb.extract(0, i), 1 });
            }
            if (!is_value(b)) {
                zstring new_b = sa + sb.extract(sa.length(), lenb);
                m_str_updates.push_back({ b, new_b, 1 });
                m_str_updates.push_back({ b, sa, 1 });
            }
        }
        else {
            SASSERT(lena <= lenb);
            if (!is_value(a)) {
                zstring ch = zstring(m_chars[ctx.rand(m_chars.size())]);
                m_str_updates.push_back({ a, sa + ch, 1 });
                m_str_updates.push_back({ a, ch + sa, 1 });
                m_str_updates.push_back({ a, sb + ch, 1 });
                m_str_updates.push_back({ a, ch + sb, 1 });
            }
            if (!is_value(b)) {
                zstring ch = zstring(m_chars[ctx.rand(m_chars.size())]);
                m_str_updates.push_back({ b, ch + sb, 1 });
                m_str_updates.push_back({ b, zstring(), 1});
            }
        }
        return apply_update();
    }

    bool seq_plugin::repair_down_str_suffixof(app* e) {
        expr* a, * b;
        VERIFY(seq.str.is_suffix(e, a, b));
        zstring sa = strval0(a);
        zstring sb = strval0(b);
        unsigned lena = sa.length();
        unsigned lenb = sb.length();
        verbose_stream() << "repair suffixof " << mk_bounded_pp(e, m) << "\n";
        if (ctx.is_true(e)) {
            unsigned n = std::min(lena, lenb);
            if (!is_value(a)) {
                for (unsigned i = 0; i < n; ++i)
                    m_str_updates.push_back({ a, sb.extract(lenb - i, i), 1 });
            }
            if (!is_value(b)) {
                zstring new_b = sb.extract(0, lenb - n) + sa;
                m_str_updates.push_back({ b, new_b, 1 });
                m_str_updates.push_back({ b, sa, 1 });
            }
        }
        else {
            SASSERT(lena <= lenb);
            if (!is_value(a)) {
                zstring ch = zstring(m_chars[ctx.rand(m_chars.size())]);
                m_str_updates.push_back({ a, ch + sa, 1 });
                m_str_updates.push_back({ a, sa + ch, 1 });
                m_str_updates.push_back({ a, ch + sb, 1 });
                m_str_updates.push_back({ a, sb + ch, 1 });
            }
            if (!is_value(b)) {
                zstring ch = zstring(m_chars[ctx.rand(m_chars.size())]);
                m_str_updates.push_back({ b, sb + ch, 1 });
                m_str_updates.push_back({ b, zstring(), 1 });
            }
        }
        return apply_update();
    }

    bool seq_plugin::repair_down_str_contains(expr* e) {
        expr* a, *b;
        VERIFY(seq.str.is_contains(e, a, b));
        zstring sa = strval0(a);
        zstring sb = strval0(b);
        unsigned lena = sa.length();
        unsigned lenb = sb.length();

        verbose_stream() << "repair contains " << mk_bounded_pp(e, m) << "\n";

        if (ctx.is_true(e)) {            
            // add b to a in front
            // add b to a in back
            // add part of b to a front/back
            // take random subsequence of a and set it to b
            // reduce size of b

            if (!is_value(a)) {
                m_str_updates.push_back({ a, sb + sa, 1 });
                m_str_updates.push_back({ a, sa + sb, 1 });
                if (lena > 2) {
                    unsigned mid = ctx.rand(lena-2) + 1;
                    zstring sa1 = sa.extract(0, mid);
                    zstring sa2 = sa.extract(mid, lena - mid); 
                    m_str_updates.push_back({ a, sa1 + sb + sa2, 1});
                }
            }
            if (!is_value(b) && lenb > 0) {
                m_str_updates.push_back({ b, sb.extract(0, lenb - 1), 1});
                m_str_updates.push_back({ b, sb.extract(1, lenb - 1), 1});
            }
        }
        else {
            // remove occurrences of b in a, if b is non-empty
            // append or pre-pend character to b

            if (!is_value(a)) {
                int idx = sa.indexofu(sb, 0);
                SASSERT(idx >= 0);
                zstring su;
                if (idx > 0)
                    su = sa.extract(0, idx);
                su = su + sa.extract(idx + sb.length(), sa.length() - idx - sb.length());            
                m_str_updates.push_back({a, su, 1});
            }
            if (!m_chars.empty() && !is_value(b)) {
                zstring sb1 = sb + zstring(m_chars[ctx.rand(m_chars.size())]);
                zstring sb2 = zstring(m_chars[ctx.rand(m_chars.size())]) + sb;
                m_str_updates.push_back({b, sb1, 1});
                m_str_updates.push_back({b, sb2, 1});
            }
        }
        return apply_update();
    }

    bool seq_plugin::repair_down_str_extract(app* e) {
        expr* x, * offset, * len;
        VERIFY(seq.str.is_extract(e, x, offset, len));
        zstring v = strval0(e);
        zstring r = strval0(x);
        expr_ref offset_e = ctx.get_value(offset);
        expr_ref len_e = ctx.get_value(len);
        rational offset_val, len_val;
        VERIFY(a.is_numeral(offset_e, offset_val));
        VERIFY(a.is_numeral(len_e, len_val));
        if (offset_val < 0)
            return false;
        if (len_val < 0)
            return false;
        SASSERT(offset_val.is_unsigned());
        SASSERT(len_val.is_unsigned());
        unsigned offset_u = offset_val.get_unsigned();
        unsigned len_u = len_val.get_unsigned();
        zstring prefix = r.extract(0, offset_u);
        zstring suffix = r.extract(offset_u + len_u, r.length());
        zstring new_r = prefix + v + suffix;
        m_str_updates.push_back({ x, new_r, 1 });
        return apply_update();
    }

#if 1
    bool seq_plugin::repair_down_str_concat(app* e) {
        auto const& es = concats(e);
        zstring value;
        zstring value0 = strval0(e);
        for (auto const& e : es)
            value += strval0(e);
        if (value == value0)
            return true;     
        uint_set chars;

        for (auto ch : value0) 
            chars.insert(ch);            

        add_edit_updates(es, value, value0, chars);

        unsigned diff = edit_distance(value, value0);
        for (auto& [x, s, score] : m_str_updates) {
            value.reset();
            for (auto z : es) {
                if (z == x)
                    value += s;
                else
                    value += strval0(z);
            }
            unsigned local_diff = edit_distance(value, value0);
            if (local_diff >= diff)
                score = 0.1;
            else
                score = (diff - local_diff) * (diff - local_diff);
        }
        return apply_update();        

    }
#else

    bool seq_plugin::repair_down_str_concat(app* e) {
        zstring val_e = strval0(e);
        unsigned len_e = val_e.length();
        // sample a random partition.
        // the current sample algorithm isn't uniformly sampling 
        // each possible partition, but favors what would be a 
        // normal distribution
        sbuffer<unsigned> lengths(e->get_num_args(), 0);
        sbuffer<unsigned> non_values;
        unsigned i = 0;
        //verbose_stream() << "repair concat " << mk_bounded_pp(e, m) << "\n";
        for (expr* arg : *e) {
            ++i;
            if (!is_value(arg)) {
                non_values.push_back(i - 1);
                continue;
            }
            auto const& arg_val = strval0(arg);
            if (arg_val.length() > len_e)
                return false;
            lengths[i - 1] = arg_val.length();
            len_e -= arg_val.length();
        }
        // TODO: take duplications into account
        while (len_e > 0 && !non_values.empty()) {
            lengths[non_values[ctx.rand(non_values.size())]]++;
            --len_e;
        }
        if (len_e > 0 && non_values.empty())
            return false;
        i = 0;
        //verbose_stream() << "repair concat2 " << mk_bounded_pp(e, m) << "\n";
        unsigned len_prefix = 0;
        for (expr* arg : *e) {
            auto len = lengths[i];
            auto val_arg = val_e.extract(len_prefix, len);
            //verbose_stream() << "repair concat3 " << mk_bounded_pp(arg, m) << " " << val_arg << "\n";
            if (!update(arg, val_arg))
                return false;
            ++i;
            len_prefix += len;
        }
        return true;
    }
#endif



    bool seq_plugin::apply_update() {
        double sum_scores = 0;
        for (auto const& [e, val, score] : m_str_updates)
            sum_scores += score;
        for (auto const& [e, val, score] : m_int_updates)
            sum_scores += score;

        IF_VERBOSE(4,
            for (auto const& [e, val, score] : m_str_updates)
                verbose_stream() << mk_pp(e, m) << " := \"" << val << "\" score: " << score << "\n";
        for (auto const& [e, val, score] : m_int_updates)
            verbose_stream() << mk_pp(e, m) << " := " << val << " score: " << score << "\n";
            );
        
        while (!m_str_updates.empty() || !m_int_updates.empty()) {
            bool is_str_update = false;
            unsigned i = m_str_updates.size();
            double lim = sum_scores * ((double)ctx.rand() / random_gen().max_value());
            if (i > 0) {
                do {
                    lim -= m_str_updates[--i].m_score;
                } while (lim >= 0 && i > 0);
            }
            is_str_update = lim == 0 || m_int_updates.empty();
            
            if (!is_str_update) {
                i = m_int_updates.size();
                do {
                    lim -= m_str_updates[--i].m_score;
                } while (lim >= 0 && i > 0);
            }
            
            if (is_str_update) {
                auto [e, value, score] = m_str_updates[i];

                if (update(e, value)) {
                    IF_VERBOSE(3, verbose_stream() << "set value " << mk_bounded_pp(e, m) << " := \"" << value << "\"\n");
                    m_str_updates.reset();
                    m_int_updates.reset();
                    return true;
                }

                sum_scores -= score;
                m_str_updates[i] = m_str_updates.back();
                m_str_updates.pop_back();
            }
            else {
                auto [e, value, score] = m_int_updates[i];

                IF_VERBOSE(3, verbose_stream() << "set value " << mk_bounded_pp(e, m) << " := " << value << "\n");

                if (update(e, value)) {
                    m_int_updates.reset();
                    m_str_updates.reset();
                    return true;
                }
                sum_scores -= score;
                m_int_updates[i] = m_int_updates.back();
                m_int_updates.pop_back();
            }
        }

        return false;
    }

    bool seq_plugin::update(expr* e, zstring const& value) {
        if (value == strval0(e))
            return true;
        if (is_value(e))
            return false;
        if (get_eval(e).min_length > value.length() || get_eval(e).max_length < value.length())
            return false;
        strval0(e) = value;
        ctx.new_value_eh(e);
        return true;
    }

    bool seq_plugin::update(expr* e, rational const& value) {        
        expr_ref val(a.mk_int(value), m);
        return ctx.set_value(e, val);
    }

    void seq_plugin::initialize() {
        if (m_initialized)
            return;
        m_initialized = true;
        expr_ref val(m);
        for (auto lit : ctx.unit_literals()) {
            auto e = ctx.atom(lit.var());
            expr* x, * y, * z;
            rational r;
            if (!lit.sign() && (a.is_le(e, x, y) || a.is_ge(e, y, x))) {
                if (a.is_numeral(x, r) && r.is_unsigned() && seq.str.is_length(y, z)) {
                    auto& ev = get_eval(z);
                    ev.min_length = std::max(ev.min_length, r.get_unsigned());
                }
                if (a.is_numeral(y, r) && r.is_unsigned() && seq.str.is_length(x, z)) {
                    auto& ev = get_eval(z);
                    ev.max_length = std::min(ev.max_length, r.get_unsigned());
                }
            }
        }
        for (auto t : ctx.subterms()) {
            if (seq.str.is_string(t)) {
                auto& ev = get_eval(t);
                ev.min_length = strval0(t).length();
                ev.max_length = strval0(t).length();
            }
            if (seq.str.is_concat(t)) {
                unsigned min_length = 0;
                unsigned max_length = 0;
                for (expr* arg : *to_app(t)) {
                    auto& ev = get_eval(arg);
                    min_length += ev.min_length;
                    if (ev.max_length < UINT_MAX && max_length != UINT_MAX)
                        max_length += ev.max_length;
                    else 
                        max_length = UINT_MAX;
                }
                auto& ev = get_eval(t);
                ev.min_length = std::max(min_length, ev.min_length);
                ev.max_length = std::min(max_length, ev.max_length);
            }
            if (seq.str.is_at(t)) {
                auto& ev = get_eval(t);
                ev.max_length = 1;
            }
            expr* x, * offset, * len;
            rational len_r;
            if (seq.str.is_extract(t, x, offset, len) && a.is_numeral(len, len_r)) {
                auto& ev = get_eval(t);
                if (len_r < 0)
                    ev.max_length = 0;
                if (len_r.is_unsigned())
                    ev.max_length = std::min(ev.max_length, len_r.get_unsigned());
            }
            // TBD: assumes arithmetic is already initialized
            if (seq.str.is_length(t, x) && ctx.is_fixed(t, val) && 
                a.is_numeral(val, len_r) && len_r.is_unsigned()) {
                auto& ev = get_eval(x);
                ev.min_length = ev.max_length = len_r.get_unsigned();                
            }
        }
    }
    
    void seq_plugin::repair_literal(sat::literal lit) {
        SASSERT(ctx.is_true(lit));
        auto e = ctx.atom(lit.var());
        if (!is_seq_predicate(e))
            return;
        auto a = to_app(e);
        // verbose_stream() << "repair " << lit << " " << mk_pp(e, m) << " " << bval1(e) << "\n";
        if (bval1(e) == lit.sign())
            ctx.flip(lit.var());
    }

    bool seq_plugin::is_value(expr* e) {
        if (seq.is_seq(e))
            return get_eval(e).is_value;
        return m.is_value(e);
    }    

    // Regular expressions

    bool seq_plugin::is_in_re(zstring const& s, expr* r) {
        expr_ref sval(seq.str.mk_string(s), m);
        th_rewriter rw(m);
        expr_ref in_re(seq.re.mk_in_re(sval, r), m);
        rw(in_re);
        SASSERT(m.limit().is_canceled() || m.is_true(in_re) || m.is_false(in_re));
        return m.is_true(in_re);
    }

    bool seq_plugin::repair_down_in_re(app* e) {
        expr* x, * y;
        VERIFY(seq.str.is_in_re(e, x, y));
        auto info = seq.re.get_info(y);
        if (!info.interpreted)
            return false;
        auto s = strval0(x);
        expr_ref xval(seq.str.mk_string(s), m);
        expr_ref in_re(seq.re.mk_in_re(xval, y), m);
        th_rewriter rw(m);
        rw(in_re);
        SASSERT(m.limit().is_canceled() || m.is_true(in_re) || m.is_false(in_re));
        if (m.is_true(in_re) == ctx.is_true(e))
            return true;

        if (is_value(x))
            return false;

        vector<lookahead> lookaheads;
        expr_ref d_r(y, m);
        seq_rewriter seqrw(m);
        for (unsigned i = 0; i < s.length(); ++i) {
            IF_VERBOSE(3, verbose_stream() << "Derivative " << s.extract(0, i) << ": " << d_r << "\n");
            if (seq.re.is_empty(d_r))
                break;
            zstring prefix = s.extract(0, i);
            choose(d_r, 2, prefix, lookaheads);
            expr_ref ch(seq.str.mk_char(s[i]), m);
            d_r = seqrw.mk_derivative(ch, d_r);            
        }
        unsigned current_min_length = UINT_MAX;
        if (!seq.re.is_empty(d_r)) {
            choose(d_r, 2, s, lookaheads);
            current_min_length = info.min_length;
        }

        unsigned global_min_length = UINT_MAX;
        for (auto& [str, min_length] : lookaheads)
            global_min_length = std::min(min_length, global_min_length);

        IF_VERBOSE(3, verbose_stream() << "repair in_re " << current_min_length << " " 
            << global_min_length << " " << mk_pp(e, m) << " " << s << "\n");

        
        // TODO: do some length analysis to prune out short candidates when there are longer ones.
        // TODO: when matching .*"bcd" with string ab, the extension abc is more interesting than aba.
        if (ctx.is_true(e)) {
            for (auto& [str, min_length] : lookaheads) {
                if (min_length == UINT_MAX && current_min_length < UINT_MAX)
                    continue;
                if (global_min_length < min_length)
                    continue;
                double score = 0.001;
                if (min_length < UINT_MAX && s.length() < str.length()) {
                    // reward small lengths
                    // penalize size differences (unless min_length decreases)
                    score = 1 << (current_min_length - min_length);
                    score /= ((double)abs((int)s.length() - (int)str.length()) + 1);
                }
                IF_VERBOSE(3, verbose_stream() << "prefix " << score << " " << min_length << ": " << str << "\n");
                m_str_updates.push_back({ x, str, score });
            }
        }
        else {
            for (auto& [str, min_length] : lookaheads)
                m_str_updates.push_back({ x, str + zstring(m_chars[ctx.rand(m_chars.size())]), 1});
        }
        return apply_update();
    }

    void seq_plugin::next_char(expr* r, unsigned_vector& chars) {
        SASSERT(seq.is_re(r));
        expr* x, * y;
        zstring s;
        if (seq.re.is_concat(r, x, y)) {
            auto info = seq.re.get_info(x);
            next_char(x, chars);
            if (info.nullable == l_true)
                next_char(y, chars);
        }
        else if (seq.re.is_to_re(r, x)) {
            if (seq.str.is_string(x, s) && !s.empty())
                chars.push_back(s[0]);            
        }
        else if (seq.re.is_union(r, x, y)) {
            next_char(x, chars);
            next_char(y, chars);
        }
        else if (seq.re.is_range(r, x, y)) {
            zstring s1, s2;
            seq.str.is_string(x, s1);
            seq.str.is_string(y, s2);
            if (s1.length() == 1 && s2.length() == 1 && s1[0] <= s2[0]) {
                chars.push_back(s1[0] + ctx.rand(s2[0] - s1[0] + 1));
                chars.push_back(s1[0]);
                chars.push_back(s2[0]);
            }
        }
        else if (seq.re.is_star(r, x) || seq.re.is_plus(r, x)) {
            next_char(x, chars);
        }
        else if (seq.re.is_empty(r)) {
            ;
        }
        else if (seq.re.is_full_seq(r)) {
            if (!m_chars.empty())
                chars.push_back(m_chars[ctx.rand(m_chars.size())]);
        }
        else if (seq.re.is_full_char(r)) {
            if (!m_chars.empty())
                chars.push_back(m_chars[ctx.rand(m_chars.size())]);
        }
        else {
            verbose_stream() << "regex nyi " << mk_bounded_pp(r, m) << "\n";
            NOT_IMPLEMENTED_YET();
        }
    }

    void seq_plugin::choose(expr* r, unsigned k, zstring& prefix, vector<lookahead>& result) {
        auto info = seq.re.get_info(r);
        result.push_back({ prefix, info.min_length });
        if (k == 0)
            return;
        unsigned_vector chars;
        next_char(r, chars);
        std::stable_sort(chars.begin(), chars.end());
        auto it = std::unique(chars.begin(), chars.end());
        chars.shrink((unsigned)(it - chars.begin()));
        for (auto ch : chars) {
            expr_ref c(seq.str.mk_char(ch), m);
            seq_rewriter rw(m);
            expr_ref r2 = rw.mk_derivative(c, r);
            zstring prefix2 = prefix + zstring(ch);
            choose(r2, k - 1, prefix2, result);
        }        
    }
}
