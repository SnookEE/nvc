//
//  Copyright (C) 2011-2015  Nick Gasson
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
#include <inttypes.h>

typedef struct interval interval_t;

struct interval {
   int64_t     low;
   int64_t     high;
   interval_t *next;
};

static int errors = 0;

#define bounds_error(t, ...) \
   do { errors++; error_at(tree_loc(t), __VA_ARGS__); } while (0)

static void bounds_check_string_literal(tree_t t)
{
   type_t type = tree_type(t);
   if (type_is_unconstrained(type))
      return;

   int64_t expect;
   if (folded_length(type_dim(type, 0), &expect) && expect != tree_chars(t))
      bounds_error(t, "expected %"PRIi64" elements in string literal but "
                   "have %d", expect, tree_chars(t));
}

static void bounds_check_literal(tree_t t)
{
   switch (tree_subkind(t)) {
   case L_STRING:
      bounds_check_string_literal(t);
      break;
   default:
      break;
   }
}

static tree_t bounds_check_call_args(tree_t t)
{
   tree_t decl = tree_ref(t);

   const int nparams = tree_params(t);
   const int nports  = tree_ports(decl);

   for (int i = 0; (i < nparams) && (i < nports); i++) {
      tree_t param = tree_param(t, i);
      assert(tree_subkind(param) == P_POS);

      tree_t value = tree_value(param);
      tree_t port  = tree_port(decl, tree_pos(param));

      type_t ftype = tree_type(port);
      type_t atype = tree_type(tree_value(param));

      if (type_is_array(ftype)) {
         // Check bounds of constrained array parameters

         if ((type_is_unconstrained(atype)) || (type_is_unconstrained(ftype)))
            continue;

         const int ndims = type_dims(ftype);

         for (int j = 0; j < ndims; j++) {
            range_t formal_r = type_dim(ftype, j);
            range_t actual_r = type_dim(atype, j);

            int64_t f_left, f_right, a_left, a_right;

            const bool folded =
               folded_int(formal_r.left, &f_left)
               && folded_int(formal_r.right, &f_right)
               && folded_int(actual_r.left, &a_left)
               && folded_int(actual_r.right, &a_right);

            if (!folded)
               continue;

            const int f_len = (actual_r.kind == RANGE_TO)
               ? a_right - a_left + 1: a_left - a_right + 1;
            const int a_len = (formal_r.kind == RANGE_TO)
               ? f_right - f_left + 1 : f_left - f_right + 1;

            if (f_len != a_len) {
               if (ndims > 1)
                  bounds_error(param, "actual length %d for dimension %d does "
                               "not match formal length %d",
                               a_len, j + 1, f_len);
               else
                  bounds_error(param, "actual length %d does not match formal "
                               "length %d", a_len, f_len);
            }
         }
      }
      else if (type_is_integer(ftype)) {
         int64_t ival;
         if (!folded_int(value, &ival))
            continue;

         range_t r = type_dim(ftype, 0);

         int64_t low, high;
         if (!folded_bounds(r, &low, &high))
            continue;

         if ((ival < low) || (ival > high))
            bounds_error(value, "value %"PRIi64" out of bounds %"PRIi64" %s "
                         "%"PRIi64" for parameter %s", ival,
                         (r.kind == RANGE_TO) ? low : high,
                         (r.kind == RANGE_TO) ? "to" : "downto",
                         (r.kind == RANGE_TO) ? high : low,
                         istr(tree_ident(port)));
      }
      else if (type_is_real(ftype)) {
         // TODO
      }
      else if (type_is_enum(ftype)) {
         // TODO
     }
   }

   return t;
}

static void bounds_check_array_ref(tree_t t)
{
   tree_t value = tree_value(t);

   if (!tree_has_type(value))
      return;

   type_t value_type = tree_type(value);

   if (type_is_unconstrained(value_type))
      return;

   int nstatic = 0;
   const int nparams = tree_params(t);
   for (int i = 0; i < nparams; i++) {
      tree_t p = tree_param(t, i);

      int64_t index;
      if (!folded_int(tree_value(p), &index))
         continue;

      range_t b = type_dim(value_type, i);

      if ((b.kind != RANGE_TO) && (b.kind != RANGE_DOWNTO))
         continue;

      int64_t left, right;
      if (folded_int(b.left, &left) && folded_int(b.right, &right)) {
         const int64_t low  = (b.kind == RANGE_TO) ? left : right;
         const int64_t high = (b.kind == RANGE_TO) ? right : left;
         if ((index < low) || (index > high)) {
            const char *name = (tree_kind(value) == T_REF)
               ? istr(tree_ident(value)) : NULL;
            bounds_error(t, "array %s%sindex %"PRIi64" out of bounds "
                         "%"PRIi64" %s %"PRIi64,
                         name ? name : "", name ? " " : "",
                         index, left,
                         (b.kind == RANGE_TO) ? "to" : "downto", right);
         }
         else
            nstatic++;
      }
   }

   if (nstatic == nparams)
      tree_add_attr_int(t, elide_bounds_i, 1);
}

static void bounds_check_array_slice(tree_t t)
{
   tree_t value = tree_value(t);

   if (!tree_has_type(value))
      return;

   type_t value_type = tree_type(value);

   if (type_is_unconstrained(value_type))
      return;

   range_t b = type_dim(value_type, 0);
   range_t r = tree_range(t);

   if ((b.kind != RANGE_TO) && (b.kind != RANGE_DOWNTO))
      return;
   else if ((r.kind != RANGE_TO) && (r.kind != RANGE_DOWNTO))
      return;

   int64_t b_left, r_left;
   bool left_error = false;
   if (folded_int(b.left, &b_left) && folded_int(r.left, &r_left))
      left_error = ((b.kind == RANGE_TO) && (r_left < b_left))
         || ((b.kind == RANGE_DOWNTO) && (r_left > b_left));

   int64_t b_right, r_right;
   bool right_error = false;
   if (folded_int(b.right, &b_right) && folded_int(r.right, &r_right))
      right_error = ((b.kind == RANGE_TO) && (r_right > b_right))
         || ((b.kind == RANGE_DOWNTO) && (r_right < b_right));

   if (left_error || right_error) {
      const char *name = (tree_kind(value) == T_REF)
         ? istr(tree_ident(value)) : NULL;
      bounds_error(t, "%s%sslice %s index %"PRIi64" out of bounds "
                   "%"PRIi64" %s %"PRIi64,
                   name ? name : "", name ? " " : "",
                   left_error ? "left" : "right",
                   left_error ? r_left : r_right, b_left,
                   (b.kind == RANGE_TO) ? "to" : "downto", b_right);
   }
}

static void bounds_within(tree_t i, range_kind_t kind, const char *what,
                          int64_t low, int64_t high)
{
   int64_t folded;
   if (folded_int(i, &folded)) {
      if (folded < low || folded > high)
         bounds_error(i, "%s index %"PRIi64" out of bounds %"PRIi64
                      " %s %"PRIi64, what, folded,
                      (kind == RANGE_TO) ? low : high,
                      (kind == RANGE_TO) ? "to" : "downto",
                      (kind == RANGE_TO) ? high : low);
   }
}

static void bounds_check_aggregate(tree_t t)
{
   type_t type = tree_type(t);
   if (!type_is_array(type))
      return;

   assert(type_kind(type) != T_UARRAY);

   // Find the tightest bounds for the index

   int64_t low  = -INT64_MAX;
   int64_t high = INT64_MAX;

   range_t type_r = type_dim(type, 0);

   const bool unconstrained = tree_attr_int(t, unconstrained_i, 0);

   if (unconstrained) {
      // Aggregate of unconstrained array type
      type_t base = type_base_recur(type);
      assert(type_kind(base) == T_UARRAY);

      type_t index = type_index_constr(base, 0);
      if (type_kind(index) == T_ENUM)
         return;   // TODO

      range_t base_r = type_dim(index, 0);

      if ((tree_kind(base_r.left) == T_LITERAL)
          && (tree_kind(base_r.right) == T_LITERAL))
         range_bounds(base_r, &low, &high);
   }
   else if ((tree_kind(type_r.left) == T_LITERAL)
            && (tree_kind(type_r.right) == T_LITERAL))
      range_bounds(type_r, &low, &high);

   if ((low == -INT64_MAX) && (high == INT64_MAX))
      return;

   // Check for out of bounds indexes

   bool known_elem_count = true;
   int nelems = 0;
   const int nassocs = tree_assocs(t);
   for (int i = 0; i < nassocs; i++) {
      tree_t a = tree_assoc(t, i);

      switch (tree_subkind(a)) {
      case A_NAMED:
         bounds_within(tree_name(a), type_r.kind, "aggregate", low, high);
         nelems++;
         break;

      case A_RANGE:
         {
            range_t r = tree_range(a);
            bounds_within(r.left, r.kind, "aggregate", low, high);
            bounds_within(r.right, r.kind, "aggregate", low, high);

            int64_t length;
            if (folded_length(r, &length))
               nelems += length;
            else
               known_elem_count = false;
         }
         break;

      case A_OTHERS:
         known_elem_count = false;
         break;

      default:
         nelems++;
         break;
      }
   }

   // Check the actual against the expected element count

   const int ndims = type_dims(type);

   if (known_elem_count) {
      int64_t expect;
      if (folded_length(type_dim(type, 0), &expect) && expect != nelems)
         bounds_error(t, "expected %"PRIi64" elements in aggregate but have %d",
                      expect, nelems);
   }

   // Check each sub-aggregate has the same length for an unconstrained
   // array aggregate

   if ((ndims > 1) && unconstrained) {
      int64_t length = -1;
      for (int i = 0; i < nassocs; i++) {
         tree_t a = tree_assoc(t, i);
         type_t value_type = tree_type(tree_value(a));

         int64_t this_length;
         if (!folded_length(type_dim(value_type, 0), &this_length))
             break;

         if (length == -1)
            length = this_length;
         else if (length != this_length)
            bounds_error(a, "length of sub-aggregate %"PRIi64" does not match "
                         "expected length %"PRIi64,
                         this_length, length);
      }
   }
}

static void bounds_check_decl(tree_t t)
{
   type_t type = tree_type(t);

   if (type_is_array(type) && (type_kind(type) != T_UARRAY)) {
      // Check folded range does not violate index constraints

      const int ndims = type_dims(type);
      for (int i = 0; i < ndims; i++) {
         range_t dim = type_dim(type, i);

         type_t cons = tree_type(dim.left);

         if (type_kind(cons) == T_ENUM)
            continue;    // TODO: checking for enum constraints

         range_t bounds = type_dim(cons, 0);

         // Only check here if range can be determined to be non-null

         int64_t dim_left, bounds_left;
         int64_t dim_right, bounds_right;

         const bool is_static =
            folded_int(dim.left, &dim_left)
            && folded_int(bounds.left, &bounds_left)
            && folded_int(dim.right, &dim_right)
            && folded_int(bounds.right, &bounds_right);

         const bool is_null =
            (dim.kind == RANGE_TO && dim_left > dim_right)
            || (dim.kind == RANGE_DOWNTO && dim_left < dim_right);

         if (is_static && !is_null) {
            if (dim_left < bounds_left)
               bounds_error(dim.left, "left index %"PRIi64" violates "
                            "constraint %s", dim_left, type_pp(cons));

            if (dim_right > bounds_right)
               bounds_error(dim.right, "right index %"PRIi64" violates "
                            "constraint %s", dim_right, type_pp(cons));
         }
      }
   }
}

static void bounds_check_assignment(tree_t target, tree_t value)
{
   type_t target_type = tree_type(target);
   type_t value_type  = tree_type(value);

   const bool check_array_length =
      type_is_array(target_type)
      && !type_is_unconstrained(target_type)
      && !type_is_unconstrained(value_type);

   if (check_array_length) {
      const int ndims = type_dims(target_type);
      for (int i = 0; i < ndims; i++) {
         int64_t target_w, value_w;
         if (folded_length(type_dim(target_type, i), &target_w)
             && folded_length(type_dim(value_type, i), &value_w)) {
            if (target_w != value_w) {
               if (i > 0)
                  bounds_error(value, "length of dimension %d of value %"PRIi64
                               " does not match length of target %"PRIi64,
                               i + 1, value_w, target_w);
               else
                  bounds_error(value, "length of value %"PRIi64" does not "
                               "match length of target %"PRIi64,
                               value_w, target_w);
            }
         }
      }
   }

   const bool check_scalar_subtype_range =
      !type_is_array(target_type)
      && !type_is_record(target_type)
      && (type_kind(target_type) == T_SUBTYPE);

   if (check_scalar_subtype_range) {
      range_t r = type_dim(target_type, 0);

      int64_t ivalue;
      if (folded_int(value, &ivalue)) {
         int64_t left, right;
         if (folded_int(r.left, &left) && folded_int(r.right, &right)) {
            switch (r.kind) {
            case RANGE_TO:
               if ((ivalue < left) || (ivalue > right))
                  bounds_error(value, "value %"PRIi64" out of target bounds %"
                               PRIi64" to %"PRIi64, ivalue, left, right);
               break;

            case RANGE_DOWNTO:
               if ((ivalue > left) || (ivalue < right))
                  bounds_error(value, "value %"PRIi64" out of target bounds %"
                               PRIi64" downto %"PRIi64, ivalue, left, right);
               break;

            default:
               break;
            }
         }
      }

      unsigned pos;
      if (folded_enum(value, &pos)) {
         unsigned left, right;
         if (folded_enum(r.left, &left) && folded_enum(r.right, &right)) {
            type_t value_base  = type_base_recur(value_type);
            type_t target_base = type_base_recur(target_type);

            tree_t value_lit = type_enum_literal(value_base, pos);
            tree_t left_lit  = type_enum_literal(target_base, left);
            tree_t right_lit = type_enum_literal(target_base, right);

            switch (r.kind) {
            case RANGE_TO:
               if ((pos < left) || (pos > right))
                  bounds_error(value, "value %s out of target bounds %s to %s",
                               istr(tree_ident(value_lit)),
                               istr(tree_ident(left_lit)),
                               istr(tree_ident(right_lit)));
               break;

            case RANGE_DOWNTO:
               if ((pos > left) || (pos < right))
                  bounds_error(value, "value %s out of target bounds %s "
                               "downto %s", istr(tree_ident(value_lit)),
                               istr(tree_ident(left_lit)),
                               istr(tree_ident(right_lit)));
               break;

            default:
               break;
            }
         }
      }
   }
}

static void bounds_check_signal_assign(tree_t t)
{
   tree_t target = tree_target(t);

   const int nwaves = tree_waveforms(t);
   for (int i = 0; i < nwaves; i++)
      bounds_check_assignment(target, tree_value(tree_waveform(t, i)));
}

static void bounds_check_var_assign(tree_t t)
{
   bounds_check_assignment(tree_target(t), tree_value(t));
}

static void bounds_case_cover(interval_t **isp, tree_t t,
                              int64_t low, int64_t high)
{
   interval_t *it, *prev;
   for (it = *isp, prev = NULL;
        (it != NULL) && (it->low <= high);
        prev = it, it = it->next) {

      if ((low <= it->high) && (it->low <= high)) {
         const int64_t rlow  = MAX(low, it->low);
         const int64_t rhigh = MIN(high, it->high);
         if (rlow == rhigh)
            bounds_error(t, "value %"PRIi64" is already covered", rlow);
         else
            bounds_error(t, "range %"PRIi64" to %"PRIi64" is already covered",
                         rlow, rhigh);
         return;
      }
      else if (high == it->low - 1) {
         it->low = low;
         return;
      }
      else if (low == it->high + 1) {
         it->high = high;
         return;
      }
   }

   interval_t *new = xmalloc(sizeof(interval_t));
   new->low  = low;
   new->high = high;

   if ((*isp == NULL) || (prev == NULL)) {
      new->next = *isp;
      *isp = new;
   }
   else {
      new->next = prev->next;
      prev->next = new;
   }
}

static void bounds_fmt_case_missing(text_buf_t *tb, int64_t low, int64_t high)
{
   if (low == high)
      tb_printf(tb, "\n    %"PRIi64, low);
   else
      tb_printf(tb, "\n    %"PRIi64" to %"PRIi64, low, high);
}

static void bounds_check_case(tree_t t)
{
   type_t type = tree_type(tree_value(t));

   if (type_is_enum(type)) {
      // Check the choices cover all elements of an enumerated type

      unsigned nlits, low, high;
      if (type_kind(type) == T_SUBTYPE) {
         assert(type_dims(type) == 1);

         range_t r = type_dim(type, 0);
         assert(r.kind == RANGE_TO);

         if ((tree_kind(r.left) != T_REF)
             || ((tree_kind(r.right) != T_REF)))
            return;

         tree_t ldecl = tree_ref(r.left);
         tree_t rdecl = tree_ref(r.right);

         assert(tree_kind(ldecl) == T_ENUM_LIT);
         assert(tree_kind(rdecl) == T_ENUM_LIT);

         low   = tree_pos(ldecl);
         high  = tree_pos(rdecl);
         nlits = high - low + 1;
      }
      else {
         nlits = type_enum_literals(type);
         low   = 0;
         high  = nlits - 1;
      }

      bool have[nlits];
      for (unsigned i = 0; i < nlits; i++)
         have[i] = false;

      type_t base = type_base_recur(type);

      bool have_others = false;

      const int nassocs = tree_assocs(t);
      for (unsigned i = 0; i < nassocs; i++) {
         tree_t a = tree_assoc(t, i);

         if (tree_subkind(a) == A_OTHERS) {
            have_others = true;
            continue;
         }

         ident_t name = tree_ident(tree_name(a));
         for (unsigned j = low; j <= high; j++) {
            if (tree_ident(type_enum_literal(base, j)) == name) {
               if (have[j - low])
                  bounds_error(tree_name(a), "choice %s appears multiple times "
                               "in case statement", istr(name));
               else
                  have[j - low] = true;
            }
         }
      }

      bool have_all = true;
      for (unsigned i = low; i <= high; i++) {
         if (!have[i - low] && !have_others)
            bounds_error(t, "missing choice %s in case statement",
                         istr(tree_ident(type_enum_literal(base, i))));
         have_all = have_all && have[i - low];
      }
   }
   else if (type_is_integer(type)) {
      // Check that the full range of the type is covered

      int64_t tlow, thigh;
      if (!folded_bounds(type_dim(type, 0), &tlow, &thigh))
         return;

      bool have_others = false;
      interval_t *covered = NULL;

      const int nassocs = tree_assocs(t);
      for (int i = 0; i < nassocs; i++) {
         tree_t a = tree_assoc(t, i);

         int64_t low = INT64_MIN, high = INT64_MAX;
         switch (tree_subkind(a)) {
         case A_OTHERS:
            have_others = true;
            continue;

         case A_NAMED:
            {
               low = high = assume_int(tree_name(a));
            }
            break;

         case A_RANGE:
            {
               range_t r = tree_range(a);
               assert(r.kind == RANGE_TO);
               low  = assume_int(r.left);
               high = assume_int(r.right);
            }
            break;
         }

         if ((low < tlow) || (high > thigh))
            bounds_error(a, "value %"PRIi64" outside %s bounds %"PRIi64
                         " to %"PRIi64, (low < tlow) ? low : high,
                         type_pp(type), tlow, thigh);
         else
            bounds_case_cover(&covered, a, low, high);
      }

      if (!have_others) {
         LOCAL_TEXT_BUF tb = tb_new();
         tb_printf(tb, "case choices do not cover the following "
                   "values of %s:", type_pp(type));

         bool missing = false;
         int64_t walk = tlow;
         interval_t *it, *tmp;
         for (it = covered, tmp = NULL; it != NULL; it = tmp) {
            if (it->low != walk) {
               bounds_fmt_case_missing(tb, walk, it->low - 1);
               missing = true;
            }

            walk = it->high + 1;

            tmp = it->next;
            free(it);
         }

         if (walk != thigh + 1) {
            bounds_fmt_case_missing(tb, walk, thigh);
            missing = true;
         }

         if (missing)
            bounds_error(t, "%s", tb_get(tb));
      }
   }
   else if (type_is_array(type)) {
      // Calculate how many values each element has
      type_t elem = type_elem(type);
      int64_t elemsz = 0;
      switch (type_kind(elem)) {
      case T_SUBTYPE:
      case T_CARRAY:
      case T_INTEGER:
         {
            int64_t low, high;
            if (!folded_bounds(type_dim(elem, 0), &low, &high))
               return;
            elemsz = high - low + 1;
         }
         break;
      case T_ENUM:
         elemsz = type_enum_literals(elem);
         break;
      default:
         return;
      }

      int64_t length;
      if (!folded_length(type_dim(type, 0), &length))
          return;

      const int64_t expect =
         elemsz > INT32_MAX ? INT64_MAX : ipow(elemsz, length);

      int64_t have = 0;
      const int nassocs = tree_assocs(t);
      for (int i = 0; i < nassocs; i++) {
         tree_t a = tree_assoc(t, i);

         switch (tree_subkind(a)) {
         case A_OTHERS:
            have = expect;
            continue;

         case A_NAMED:
            have++;
            break;

         case A_RANGE:
            assert(false);
         }
      }

      if (have != expect) {
         if (expect == INT64_MAX)
            bounds_error(t, "choices do not cover all possible values");
         else
            bounds_error(t, "choices cover only %"PRIi64" of %"PRIi64
                         " possible values", have, expect);
      }
   }
}

static void bounds_check_type_conv(tree_t t)
{
   tree_t value = tree_value(tree_param(t, 0));

   type_t from = tree_type(value);
   type_t to   = tree_type(t);

   if (type_is_integer(to)) {
      int64_t ival = 0;
      double rval = 0.0;
      bool folded = false;
      if (type_is_real(from) && (folded = folded_real(value, &rval)))
         ival = (int64_t)rval;
      else if (type_is_integer(from) && (folded = folded_int(value, &ival)))
         ;

      if (folded) {
         int64_t b_low, b_high;
         folded_bounds(type_dim(to, 0), &b_low, &b_high);
         if (folded_bounds(type_dim(to, 0), &b_low, &b_high)
             && (ival < b_low || ival > b_high)) {
            char *argstr LOCAL = type_is_real(from)
               ? xasprintf("%lg", rval) : xasprintf("%"PRIi64, ival);
            bounds_error(value, "type conversion argument %s out of "
                         "bounds %"PRIi64" to %"PRIi64, argstr, b_low, b_high);
         }
      }
   }
}

static void bounds_check_attr_ref(tree_t t)
{
   const predef_attr_t predef = tree_attr_int(t, builtin_i, -1);
   switch (predef) {
   case ATTR_LENGTH:
   case ATTR_LOW:
   case ATTR_HIGH:
   case ATTR_LEFT:
   case ATTR_RIGHT:
      if (tree_params(t) > 0)
      {
         type_t type = tree_type(tree_name(t));
         if (type_is_array(type) && !type_is_unconstrained(type)) {
            tree_t dim_tree = tree_value(tree_param(t, 0));

            int64_t dim;
            const bool f = folded_int(dim_tree, &dim);
            assert(f);

            if (dim < 1 || dim > type_dims(type))
               bounds_error(dim_tree, "invalid dimension %"PRIi64" for type %s",
                            dim, type_pp(type));
         }
      }
      break;

   default:
      break;
   }
}

static void bounds_visit_fn(tree_t t, void *context)
{
   switch (tree_kind(t)) {
   case T_PCALL:
   case T_FCALL:
      bounds_check_call_args(t);
      break;
   case T_ARRAY_REF:
      bounds_check_array_ref(t);
      break;
   case T_ARRAY_SLICE:
      bounds_check_array_slice(t);
      break;
   case T_AGGREGATE:
      bounds_check_aggregate(t);
      break;
   case T_SIGNAL_DECL:
   case T_CONST_DECL:
   case T_VAR_DECL:
      bounds_check_decl(t);
      break;
   case T_SIGNAL_ASSIGN:
      bounds_check_signal_assign(t);
      break;
   case T_VAR_ASSIGN:
      bounds_check_var_assign(t);
      break;
   case T_CASE:
      bounds_check_case(t);
      break;
   case T_LITERAL:
      bounds_check_literal(t);
      break;
   case T_TYPE_CONV:
      bounds_check_type_conv(t);
      break;
   case T_ATTR_REF:
      bounds_check_attr_ref(t);
      break;
   default:
      break;
   }
}

void bounds_check(tree_t top)
{
   tree_visit(top, bounds_visit_fn, NULL);
}

int bounds_errors(void)
{
   return errors;
}
