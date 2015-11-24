//
//  Copyright (C) 2013-2015  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "phase.h"
#include "util.h"
#include "common.h"

#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#define VTABLE_SZ 16
#define MAX_ITERS 1000

typedef struct vtable vtable_t;
typedef struct vtframe vtframe_t;

struct vtframe {
   struct {
      ident_t name;
      tree_t  value;
   } binding[VTABLE_SZ];

   size_t     size;
   vtframe_t *down;
};

struct vtable {
   vtframe_t *top;
   bool       failed;
   ident_t    exit;
   tree_t     result;
};

static void eval_stmt(tree_t t, vtable_t *v);
static tree_t eval_expr(tree_t t, vtable_t *v);

static bool debug = true;

#define eval_error(t, ...) do {                 \
      if (unlikely(debug))                      \
         warn_at(tree_loc(t),  __VA_ARGS__);    \
      v->failed = true;                         \
      return;                                   \
   } while (0)

static void vtable_push(vtable_t *v)
{
   vtframe_t *f = xmalloc(sizeof(vtframe_t));
   f->size = 0;
   f->down = v->top;

   v->top = f;
}

static void vtable_pop(vtable_t *v)
{
   vtframe_t *f = v->top;
   v->top    = f->down;
   v->result = NULL;
   free(f);
}

static void vtable_bind(vtable_t *v, ident_t name, tree_t value)
{
   vtframe_t *f = v->top;
   if (f == NULL)
      return;

   for (size_t i = 0; i < f->size; i++) {
      if (f->binding[i].name == name) {
         f->binding[i].value = value;
         return;
      }
   }

   assert(f->size < VTABLE_SZ);
   f->binding[f->size].name  = name;
   f->binding[f->size].value = value;
   ++(f->size);
}

static tree_t vtframe_get(vtframe_t *f, ident_t name)
{
   if (f == NULL)
      return NULL;
   else {
      for (size_t i = 0; i < f->size; i++) {
         if (f->binding[i].name == name)
            return f->binding[i].value;
      }
      return vtframe_get(f->down, name);
   }
}

static tree_t vtable_get(vtable_t *v, ident_t name)
{
   return vtframe_get(v->top, name);
}

static bool folded(tree_t t)
{
   tree_kind_t kind = tree_kind(t);
   if (kind == T_LITERAL)
      return true;
   else if (kind == T_REF) {
      bool dummy;
      return folded_bool(t, &dummy);
   }
   else
      return false;
}

static tree_t eval_fcall_log(tree_t t, ident_t builtin, bool *args)
{
   if (icmp(builtin, "not"))
      return get_bool_lit(t, !args[0]);
   else if (icmp(builtin, "and"))
      return get_bool_lit(t, args[0] && args[1]);
   else if (icmp(builtin, "nand"))
      return get_bool_lit(t, !(args[0] && args[1]));
   else if (icmp(builtin, "or"))
      return get_bool_lit(t, args[0] || args[1]);
   else if (icmp(builtin, "nor"))
      return get_bool_lit(t, !(args[0] || args[1]));
   else if (icmp(builtin, "xor"))
      return get_bool_lit(t, args[0] ^ args[1]);
   else if (icmp(builtin, "xnor"))
      return get_bool_lit(t, !(args[0] ^ args[1]));
   else if (icmp(builtin, "eq"))
      return get_bool_lit(t, args[0] == args[1]);
   else if (icmp(builtin, "neq"))
      return get_bool_lit(t, args[0] != args[1]);
   else
      return t;
}

static tree_t eval_fcall_real(tree_t t, ident_t builtin, double *args)
{
   if (icmp(builtin, "mul"))
      return get_real_lit(t, args[0] * args[1]);
   else if (icmp(builtin, "div"))
      return get_real_lit(t, args[0] / args[1]);
   else if (icmp(builtin, "add"))
      return get_real_lit(t, args[0] + args[1]);
   else if (icmp(builtin, "sub"))
      return get_real_lit(t, args[0] - args[1]);
   else if (icmp(builtin, "neg"))
      return get_real_lit(t, -args[0]);
   else if (icmp(builtin, "identity"))
      return get_real_lit(t, args[0]);
   else if (icmp(builtin, "eq"))
      return get_bool_lit(t, args[0] == args[1]);
   else if (icmp(builtin, "neq"))
      return get_bool_lit(t, args[0] != args[1]);
   else if (icmp(builtin, "gt"))
      return get_bool_lit(t, args[0] > args[1]);
   else if (icmp(builtin, "lt"))
      return get_bool_lit(t, args[0] < args[1]);
   else
      return t;
}

static tree_t eval_fcall_int(tree_t t, ident_t builtin, int64_t *args, int n)
{
   if (icmp(builtin, "mul"))
      return get_int_lit(t, args[0] * args[1]);
   else if (icmp(builtin, "div"))
      return get_int_lit(t, args[0] / args[1]);
   else if (icmp(builtin, "add"))
      return get_int_lit(t, args[0] + args[1]);
   else if (icmp(builtin, "sub"))
      return get_int_lit(t, args[0] - args[1]);
   else if (icmp(builtin, "neg"))
      return get_int_lit(t, -args[0]);
   else if (icmp(builtin, "identity"))
      return get_int_lit(t, args[0]);
   else if (icmp(builtin, "eq"))
      return get_bool_lit(t, args[0] == args[1]);
   else if (icmp(builtin, "neq"))
      return get_bool_lit(t, args[0] != args[1]);
   else if (icmp(builtin, "gt"))
      return get_bool_lit(t, args[0] > args[1]);
   else if (icmp(builtin, "lt"))
      return get_bool_lit(t, args[0] < args[1]);
   else if (icmp(builtin, "leq"))
      return get_bool_lit(t, args[0] <= args[1]);
   else if (icmp(builtin, "geq"))
      return get_bool_lit(t, args[0] >= args[1]);
   else if (icmp(builtin, "exp")) {
      int64_t result = 1;
      int64_t a = args[0];
      int64_t b = args[1];

      if (a == 0)
         return get_int_lit(t, 0);
      else if (b == 0)
         return get_int_lit(t, 1);
      else if (b < 0)
         return t;

      while (b != 0) {
         if (b & 1)
            result *= a;
         a *= a;
         b >>= 1;
      }

      return get_int_lit(t, result);
   }
   else if (icmp(builtin, "min")) {
      assert(n > 0);
      int64_t r = args[0];
      for (int i = 1; i < n; i++)
         r = MIN(r, args[i]);
      return get_int_lit(t, r);
   }
   else if (icmp(builtin, "max")) {
      assert(n > 0);
      int64_t r = args[0];
      for (int i = 1; i < n; i++)
         r = MAX(r, args[i]);
      return get_int_lit(t, r);
   }
   else if (icmp(builtin, "mod"))
      return get_int_lit(t, llabs(args[0]) % llabs(args[1]));
   else if (icmp(builtin, "rem"))
      return get_int_lit(t, args[0] % args[1]);
   else
      return t;
}

static tree_t eval_fcall_enum(tree_t t, ident_t builtin, unsigned *args, int n)
{
   if (icmp(builtin, "min")) {
      assert(n > 0);
      unsigned r = args[0];
      for (int i = 1; i < n; i++)
         r = MIN(r, args[i]);
      return get_int_lit(t, r);
   }
   else if (icmp(builtin, "max")) {
      assert(n > 0);
      unsigned r = args[0];
      for (int i = 1; i < n; i++)
         r = MAX(r, args[i]);
      return get_int_lit(t, r);
   }
   else if (icmp(builtin, "eq"))
      return get_bool_lit(t, args[0] == args[1]);
   else if (icmp(builtin, "neq"))
      return get_bool_lit(t, args[0] != args[1]);
   else
      return t;
}

static tree_t eval_fcall_universal(tree_t t, ident_t builtin, tree_t *args)
{
   int64_t ival;
   double rval;

   if (icmp(builtin, "mulri") && folded_real(args[0], &rval)
       && folded_int(args[1], &ival))
      return get_real_lit(t, rval * (double)ival);
   else if (icmp(builtin, "mulir") && folded_real(args[1], &rval)
            && folded_int(args[0], &ival))
      return get_real_lit(t, rval * (double)ival);
   else if (icmp(builtin, "divri") && folded_real(args[0], &rval)
            && folded_int(args[1], &ival))
      return get_real_lit(t, rval / (double)ival);
   else
      fatal_at(tree_loc(t), "universal expression cannot be evaluated");
}

static tree_t eval_fcall_str(tree_t t, ident_t builtin, tree_t *args)
{
   if (icmp(builtin, "aeq") || icmp(builtin, "aneq")) {
      const int lchars = tree_chars(args[0]);
      const int rchars = tree_chars(args[1]);

      const bool invert = icmp(builtin, "aneq");

      if (lchars != rchars)
         return get_bool_lit(t, invert);

      for (int i = 0; i < lchars; i++) {
         tree_t c0 = tree_char(args[0], i);
         tree_t c1 = tree_char(args[1], i);
         if (tree_ident(c0) != tree_ident(c1))
            return get_bool_lit(t, invert);
      }

      return get_bool_lit(t, !invert);
   }
   else
      return t;
}

static void eval_stmts(tree_t t, unsigned (*count)(tree_t),
                       tree_t (*get)(tree_t, unsigned), vtable_t *v)
{
   const int nstmts = (*count)(t);
   for (int i = 0; i < nstmts; i++) {
      eval_stmt((*get)(t, i), v);
      if (v->failed || (v->result != NULL) || (v->exit != NULL))
         return;
   }
}

static void eval_func_body(tree_t t, vtable_t *v)
{
   const int ndecls = tree_decls(t);
   for (int i = 0; i < ndecls; i++) {
      tree_t decl = tree_decl(t, i);
      if ((tree_kind(decl) == T_VAR_DECL) && tree_has_value(decl))
         vtable_bind(v, tree_ident(decl), eval_expr(tree_value(decl), v));
   }

   eval_stmts(t, tree_stmts, tree_stmt, v);
}

static tree_t eval_fcall(tree_t t, vtable_t *v)
{
   tree_t decl = tree_ref(t);
   assert(tree_kind(decl) == T_FUNC_DECL
          || tree_kind(decl) == T_FUNC_BODY);

   ident_t builtin = tree_attr_str(decl, builtin_i);
   if (builtin == NULL) {
      if (tree_kind(decl) != T_FUNC_BODY)
         return t;

      // Only evaluating scalar functions is supported at the moment
      if (type_is_array(tree_type(t)))
         return t;

      const int nports = tree_ports(decl);
      tree_t params[nports];

      for (int i = 0; i < nports; i++) {
         params[i] = eval_expr(tree_value(tree_param(t, i)), v);

         if (!folded(params[i]))
            return t;    // Cannot fold this function call
      }

      vtable_push(v);
      for (int i = 0; i < nports; i++)
         vtable_bind(v, tree_ident(tree_port(decl, i)), params[i]);

      eval_func_body(decl, v);
      tree_t result = v->result;
      vtable_pop(v);

      return ((result != NULL) && folded(result)) ? result : t;
   }

   const int nparams = tree_params(t);

   tree_t targs[nparams];
   for (int i = 0; i < nparams; i++) {
      tree_t p = tree_param(t, i);
      targs[i] = eval_expr(tree_value(p), v);
   }

   if (icmp(builtin, "mulri") || icmp(builtin, "mulir")
       || icmp(builtin, "divri"))
      return eval_fcall_universal(t, builtin, targs);

   bool can_fold_int  = true;
   bool can_fold_log  = true;
   bool can_fold_real = true;
   bool can_fold_enum = true;
   bool can_fold_str  = true;
   int64_t iargs[nparams];
   double rargs[nparams];
   bool bargs[nparams];
   unsigned eargs[nparams];
   for (int i = 0; i < nparams; i++) {
      can_fold_int  = can_fold_int && folded_int(targs[i], &iargs[i]);
      can_fold_log  = can_fold_log && folded_bool(targs[i], &bargs[i]);
      can_fold_real = can_fold_real && folded_real(targs[i], &rargs[i]);
      can_fold_enum = can_fold_enum && folded_enum(targs[i], &eargs[i]);
      can_fold_str  = can_fold_str && tree_kind(targs[i]) == T_LITERAL
         && tree_subkind(targs[i]) == L_STRING;
   }

   if (can_fold_int)
      return eval_fcall_int(t, builtin, iargs, nparams);
   else if (can_fold_log)
      return eval_fcall_log(t, builtin, bargs);
   else if (can_fold_real)
      return eval_fcall_real(t, builtin, rargs);
   else if (can_fold_enum)
      return eval_fcall_enum(t, builtin, eargs, nparams);
   else if (can_fold_str)
      return eval_fcall_str(t, builtin, targs);
   else
      return t;
}

static tree_t eval_ref(tree_t t, vtable_t *v)
{
   tree_t decl = tree_ref(t);
   if (tree_kind(decl) == T_CONST_DECL)
      return eval_expr(tree_value(decl), v);
   else {
      tree_t binding = vtable_get(v, tree_ident(tree_ref(t)));
      return (binding != NULL) ? binding : t;
   }
}

static tree_t eval_type_conv(tree_t t, vtable_t *v)
{
   tree_t value = eval_expr(tree_value(tree_param(t, 0)), v);

   type_t from = tree_type(value);
   type_t to   = tree_type(t);

   type_kind_t from_k = type_kind(from);
   type_kind_t to_k   = type_kind(to);

   if (from_k == T_INTEGER && to_k == T_REAL) {
      int64_t l;
      if (folded_int(value, &l))
         return get_real_lit(t, (double)l);
   }
   else if (from_k == T_REAL && to_k == T_INTEGER) {
      double l;
      if (folded_real(value, &l))
         return get_int_lit(t, (int)l);
   }

   return t;
}

static tree_t eval_expr(tree_t t, vtable_t *v)
{
   switch (tree_kind(t)) {
   case T_FCALL:
      return eval_fcall(t, v);
   case T_REF:
      return eval_ref(t, v);
   case T_TYPE_CONV:
      return eval_type_conv(t, v);
   default:
      return t;
   }
}

static void eval_return(tree_t t, vtable_t *v)
{
   assert(tree_has_value(t));
   assert(v->result == NULL);
   v->result = eval_expr(tree_value(t), v);
}

static void eval_if(tree_t t, vtable_t *v)
{
   tree_t cond = eval_expr(tree_value(t), v);
   bool cond_b;
   if (!folded_bool(cond, &cond_b))
      eval_error(cond, "cannot constant fold expression");

   if (cond_b)
      eval_stmts(t, tree_stmts, tree_stmt, v);
   else
      eval_stmts(t, tree_else_stmts, tree_else_stmt, v);
}

static void eval_case(tree_t t, vtable_t *v)
{
   tree_t value = tree_value(t);

   if (type_is_array(tree_type(value)))
      eval_error(value, "cannot constant fold array case");

   int64_t value_int;
   if (!folded_int(eval_expr(value, v), &value_int))
      eval_error(value, "cannot constant fold expression");

   const int nassocs = tree_assocs(t);
   for (int i = 0; i < nassocs; i++) {
      tree_t a = tree_assoc(t, i);
      switch (tree_subkind(a)) {
      case A_NAMED:
         {
            int64_t cmp;
            if (!folded_int(eval_expr(tree_name(a), v), &cmp))
               eval_error(tree_name(a), "cannot constant fold expression");
            else if (cmp == value_int) {
               eval_stmt(tree_value(a), v);
               return;
            }
         }
         break;

      case A_OTHERS:
         eval_stmt(tree_value(a), v);
         break;

      default:
         assert(false);
      }
   }
}

static void eval_while(tree_t t, vtable_t *v)
{
   int iters = 0;
   tree_t value = tree_has_value(t) ? tree_value(t) : NULL;
   while (v->result == NULL) {
      bool cond_b = true;
      if (value != NULL) {
         tree_t cond = eval_expr(value, v);
         if (!folded_bool(cond, &cond_b))
            eval_error(value, "cannot constant fold expression");
      }

      if (!cond_b || v->failed)
         break;
      else if (++iters == MAX_ITERS) {
         warn_at(tree_loc(t), "iteration limit exceeded");
         v->failed = true;
         break;
      }

      eval_stmts(t, tree_stmts, tree_stmt, v);

      if (v->exit != NULL) {
         if (v->exit == tree_ident(t))
            v->exit = NULL;
         break;
      }
   }
}

static void eval_for(tree_t t, vtable_t *v)
{
   range_t r = tree_range(t);
   if (r.kind != RANGE_TO && r.kind != RANGE_DOWNTO) {
      v->failed = true;
      return;
   }

   tree_t left  = eval_expr(r.left, v);
   tree_t right = eval_expr(r.right, v);

   int64_t lefti, righti;
   if (!folded_int(left, &lefti) || !folded_int(right, &righti)) {
      v->failed = true;
      return;
   }

   if ((r.kind == RANGE_TO && lefti > righti)
       || (r.kind == RANGE_DOWNTO && righti < lefti))
      return;

   tree_t idecl = tree_decl(t, 0);
   int64_t ival = lefti;
   do {
      vtable_bind(v, tree_ident(idecl), get_int_lit(left, ival));
      eval_stmts(t, tree_stmts, tree_stmt, v);
      if (ival == righti)
         break;
      ival += (r.kind == RANGE_TO ? 1 : -1);
   } while (v->result == NULL);
}

static void eval_var_assign(tree_t t, vtable_t *v)
{
   tree_t target = tree_target(t);
   if (tree_kind(target) != T_REF)
      eval_error(target, "cannot evaluate this target");

   tree_t value = tree_value(t);
   tree_t updated = eval_expr(value, v);
   if (!folded(updated))
      eval_error(value, "cannot constant fold expression");

   vtable_bind(v, tree_ident(tree_ref(target)), updated);
}

static void eval_block(tree_t t, vtable_t *v)
{
   assert(tree_decls(t) == 0);
   eval_stmts(t, tree_stmts, tree_stmt, v);
}

static void eval_exit(tree_t t, vtable_t *v)
{
   if (tree_has_value(t)) {
      bool cond_b = true;
      tree_t cond = eval_expr(tree_value(t), v);
      if (!folded_bool(cond, &cond_b))
         eval_error(tree_value(t), "cannot constant fold expression");
      else if (!cond_b)
         return;
   }

   v->exit = tree_ident2(t);
}

static void eval_stmt(tree_t t, vtable_t *v)
{
   switch (tree_kind(t)) {
   case T_RETURN:
      eval_return(t, v);
      break;
   case T_WHILE:
      eval_while(t, v);
      break;
   case T_FOR:
      eval_for(t, v);
      break;
   case T_IF:
      eval_if(t, v);
      break;
   case T_VAR_ASSIGN:
      eval_var_assign(t, v);
      break;
   case T_BLOCK:
      eval_block(t, v);
      break;
   case T_EXIT:
      eval_exit(t, v);
      break;
   case T_CASE:
      eval_case(t, v);
      break;
   default:
      eval_error(t, "cannot evaluate statement %s",
                 tree_kind_str(tree_kind(t)));
   }
}

tree_t eval(tree_t fcall)
{
   assert(tree_kind(fcall) == T_FCALL);

   static bool have_debug = false;
   if (!have_debug) {
      debug = (getenv("NVC_EVAL_DEBUG") != NULL);
      have_debug = true;
   }

   vtable_t vt = {
      .top    = NULL,
      .failed = false,
      .exit   = NULL,
      .result = NULL
   };
   tree_t r = eval_fcall(fcall, &vt);
   return vt.failed ? fcall : r;
}
