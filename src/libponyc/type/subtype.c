#include "subtype.h"
#include "alias.h"
#include "assemble.h"
#include "cap.h"
#include "matchtype.h"
#include "reify.h"
#include "typeparam.h"
#include "viewpoint.h"
#include "../ast/astbuild.h"
#include "../expr/literal.h"
#include <string.h>
#include <assert.h>

static bool is_eq_typeargs(ast_t* a, ast_t* b, errorframe_t* errorf,
  pass_opt_t* opt);
static bool is_isect_sub_x(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt);

static __pony_thread_local ast_t* subtype_assume;

static void struct_cant_be_x(ast_t* sub, ast_t* super, errorframe_t* errorf,
  const char* entity)
{
  if(errorf == NULL)
    return;

  ast_error_frame(errorf, sub,
    "%s is not a subtype of %s: a struct can't be a subtype of %s",
    ast_print_type(sub), ast_print_type(super), entity);
}

static bool exact_nominal(ast_t* a, ast_t* b, pass_opt_t* opt)
{
  AST_GET_CHILDREN(a, a_pkg, a_id, a_typeargs, a_cap, a_eph);
  AST_GET_CHILDREN(b, b_pkg, b_id, b_typeargs, b_cap, b_eph);

  ast_t* a_def = (ast_t*)ast_data(a);
  ast_t* b_def = (ast_t*)ast_data(b);

  return (a_def == b_def) && is_eq_typeargs(a, b, false, opt);
}

static bool check_assume(ast_t* sub, ast_t* super, pass_opt_t* opt)
{
  // Returns true if we have already assumed sub is a subtype of super.
  if(subtype_assume != NULL)
  {
    ast_t* assumption = ast_child(subtype_assume);

    while(assumption != NULL)
    {
      AST_GET_CHILDREN(assumption, assume_sub, assume_super);

      if(exact_nominal(sub, assume_sub, opt) &&
        exact_nominal(super, assume_super, opt))
        return true;

      assumption = ast_sibling(assumption);
    }
  }

  return false;
}

static bool push_assume(ast_t* sub, ast_t* super, pass_opt_t* opt)
{
  if(check_assume(sub, super, opt))
    return true;

  if(subtype_assume == NULL)
    subtype_assume = ast_from(sub, TK_NONE);

  BUILD(assume, sub, NODE(TK_NONE, TREE(ast_dup(sub)) TREE(ast_dup(super))));
  ast_add(subtype_assume, assume);
  return false;
}

static void pop_assume()
{
  ast_t* assumption = ast_pop(subtype_assume);
  ast_free_unattached(assumption);

  if(ast_child(subtype_assume) == NULL)
  {
    ast_free_unattached(subtype_assume);
    subtype_assume = NULL;
  }
}

static bool is_sub_cap_and_eph(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  (void)opt;
  ast_t* sub_cap = cap_fetch(sub);
  ast_t* sub_eph = ast_sibling(sub_cap);
  ast_t* super_cap = cap_fetch(super);
  ast_t* super_eph = ast_sibling(super_cap);

  if(!is_cap_sub_cap(ast_id(sub_cap), ast_id(sub_eph),
    ast_id(super_cap), ast_id(super_eph)))
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, sub,
        "%s is not a subtype of %s: %s%s is not a subtype of %s%s",
        ast_print_type(sub), ast_print_type(super),
        ast_print_type(sub_cap), ast_print_type(sub_eph),
        ast_print_type(super_cap), ast_print_type(super_eph));
    }

    return false;
  }

  return true;
}

static bool is_eq_typeargs(ast_t* a, ast_t* b, errorframe_t* errorf,
  pass_opt_t* opt)
{
  assert(ast_id(a) == TK_NOMINAL);
  assert(ast_id(b) == TK_NOMINAL);

  // Check typeargs are the same.
  ast_t* a_arg = ast_child(ast_childidx(a, 2));
  ast_t* b_arg = ast_child(ast_childidx(b, 2));
  bool ret = true;

  while((a_arg != NULL) && (b_arg != NULL))
  {
    if(!is_eqtype(a_arg, b_arg, errorf, opt))
      ret = false;

    a_arg = ast_sibling(a_arg);
    b_arg = ast_sibling(b_arg);
  }

  if(!ret && errorf != NULL)
  {
    ast_error_frame(errorf, a, "%s has different type arguments than %s",
      ast_print_type(a), ast_print_type(b));
  }

  // Make sure we had the same number of typeargs.
  if((a_arg != NULL) || (b_arg != NULL))
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, a,
        "%s has a different number of type arguments than %s",
        ast_print_type(a), ast_print_type(b));
    }

    ret = false;
  }

  return ret;
}

static bool is_reified_fun_sub_fun(ast_t* sub, ast_t* super,
  errorframe_t* errorf, pass_opt_t* opt)
{
  AST_GET_CHILDREN(sub, sub_cap, sub_id, sub_typeparams, sub_params,
    sub_result, sub_throws);

  AST_GET_CHILDREN(super, super_cap, super_id, super_typeparams, super_params,
    super_result, super_throws);

  switch(ast_id(sub))
  {
    case TK_NEW:
    {
      // Covariant receiver.
      if(!is_cap_sub_cap(ast_id(sub_cap), TK_NONE, ast_id(super_cap), TK_NONE))
      {
        if(errorf != NULL)
        {
          ast_error_frame(errorf, sub,
            "%s constructor is not a subtype of %s constructor",
            ast_print_type(sub_cap), ast_print_type(super_cap));
        }

        return false;
      }

      // Covariant result.
      if(!is_subtype(sub_result, super_result, errorf, opt))
      {
        if(errorf != NULL)
        {
          ast_error_frame(errorf, sub,
            "constructor result %s is not a subtype of %s",
            ast_print_type(sub_result), ast_print_type(super_result));
        }

        return false;
      }
      break;
    }

    case TK_FUN:
    case TK_BE:
    {
      // Contravariant receiver.
      if(!is_cap_sub_cap(ast_id(super_cap), TK_NONE, ast_id(sub_cap), TK_NONE))
      {
        if(errorf != NULL)
        {
          ast_error_frame(errorf, sub,
            "%s method is not a subtype of %s method",
            ast_print_type(sub_cap), ast_print_type(super_cap));
        }

        return false;
      }

      // Covariant result.
      if(!is_subtype(sub_result, super_result, errorf, opt))
      {
        if(errorf != NULL)
        {
          ast_error_frame(errorf, sub,
            "method result %s is not a subtype of %s",
            ast_print_type(sub_result), ast_print_type(super_result));
        }

        return false;
      }
      break;
    }

    default: {}
  }

  // Contravariant type parameter constraints.
  ast_t* sub_typeparam = ast_child(sub_typeparams);
  ast_t* super_typeparam = ast_child(super_typeparams);

  while((sub_typeparam != NULL) && (super_typeparam != NULL))
  {
    ast_t* sub_constraint = ast_childidx(sub_typeparam, 1);
    ast_t* super_constraint = ast_childidx(super_typeparam, 1);

    if(!is_subtype(super_constraint, sub_constraint, errorf, opt))
    {
      if(errorf != NULL)
      {
        ast_error_frame(errorf, sub,
          "type parameter constraint %s is not a supertype of %s",
          ast_print_type(sub_constraint), ast_print_type(super_constraint));
      }

      return false;
    }

    sub_typeparam = ast_sibling(sub_typeparam);
    super_typeparam = ast_sibling(super_typeparam);
  }

  // Contravariant parameters.
  ast_t* sub_param = ast_child(sub_params);
  ast_t* super_param = ast_child(super_params);

  while((sub_param != NULL) && (super_param != NULL))
  {
    ast_t* sub_type = ast_childidx(sub_param, 1);
    ast_t* super_type = ast_childidx(super_param, 1);

    // Contravariant: the super type must be a subtype of the sub type.
    if(!is_subtype(super_type, sub_type, errorf, opt))
    {
      if(errorf != NULL)
      {
        ast_error_frame(errorf, sub, "parameter %s is not a supertype of %s",
          ast_print_type(sub_type), ast_print_type(super_type));
      }

      return false;
    }

    sub_param = ast_sibling(sub_param);
    super_param = ast_sibling(super_param);
  }

  // Covariant throws.
  if((ast_id(sub_throws) == TK_QUESTION) &&
    (ast_id(super_throws) != TK_QUESTION))
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, sub,
        "a partial function is not a subtype of a total function");
    }

    return false;
  }

  return true;
}

static bool is_fun_sub_fun(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  token_id tsub = ast_id(sub);
  token_id tsuper = ast_id(super);

  switch(tsub)
  {
    case TK_NEW:
    case TK_BE:
    case TK_FUN:
      break;

    default:
      return false;
  }

  switch(tsuper)
  {
    case TK_NEW:
    case TK_BE:
    case TK_FUN:
      break;

    default:
      return false;
  }

  AST_GET_CHILDREN(sub, sub_cap, sub_id, sub_typeparams, sub_params);
  AST_GET_CHILDREN(super, super_cap, super_id, super_typeparams, super_params);

  // Must have the same name.
  if(ast_name(sub_id) != ast_name(super_id))
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, sub,
        "method %s is not a subtype of method %s: they have different names",
        ast_name(sub_id), ast_name(super_id));
    }

    return false;
  }

  // A constructor can only be a subtype of a constructor.
  if(((tsub == TK_NEW) || (tsuper == TK_NEW)) && (tsub != tsuper))
  {
    if(errorf != NULL)
      ast_error_frame(errorf, sub,
        "only a constructor can be a subtype of a constructor");

    return false;
  }

  // Must have the same number of type parameters.
  if(ast_childcount(sub_typeparams) != ast_childcount(super_typeparams))
  {
    if(errorf != NULL)
      ast_error_frame(errorf, sub,
        "methods have a different number of type parameters");

    return false;
  }

  // Must have the same number of parameters.
  if(ast_childcount(sub_params) != ast_childcount(super_params))
  {
    if(errorf != NULL)
      ast_error_frame(errorf, sub,
        "methods have a different number of parameters");

    return false;
  }

  ast_t* r_sub = sub;

  if(ast_id(super_typeparams) != TK_NONE)
  {
    // Reify sub with the type parameters of super.
    BUILD(typeargs, super_typeparams, NODE(TK_TYPEARGS));
    ast_t* super_typeparam = ast_child(super_typeparams);

    while(super_typeparam != NULL)
    {
      AST_GET_CHILDREN(super_typeparam, super_id, super_constraint);

      BUILD(typearg, super_typeparam,
        NODE(TK_TYPEPARAMREF, TREE(super_id) NONE NONE));

      ast_t* def = ast_get(super_typeparam, ast_name(super_id), NULL);
      ast_setdata(typearg, def);
      typeparam_set_cap(typearg);
      ast_append(typeargs, typearg);

      super_typeparam = ast_sibling(super_typeparam);
    }

    r_sub = reify(sub, sub_typeparams, typeargs, opt, true);
    ast_free_unattached(typeargs);
  }

  bool ok = is_reified_fun_sub_fun(r_sub, super, errorf, opt);

  if(r_sub != sub)
    ast_free_unattached(r_sub);

  return ok;
}

static bool is_x_sub_isect(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  // T1 <: T2
  // T1 <: T3
  // ---
  // T1 <: (T2 & T3)
  for(ast_t* child = ast_child(super);
    child != NULL;
    child = ast_sibling(child))
  {
    if(!is_subtype(sub, child, errorf, opt))
    {
      if(errorf != NULL)
      {
        ast_error_frame(errorf, sub,
          "%s is not a subtype of every element of %s",
          ast_print_type(sub), ast_print_type(super));
      }

      return false;
    }
  }

  return true;
}

static bool is_x_sub_union(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  // TODO: a tuple of unions may be a subtype of a union of tuples without
  // being a subtype of any one element.

  // T1 <: T2 or T1 <: T3
  // ---
  // T1 <: (T2 | T3)
  for(ast_t* child = ast_child(super);
    child != NULL;
    child = ast_sibling(child))
  {
    if(is_subtype(sub, child, NULL, opt))
      return true;
  }

  if(errorf != NULL)
  {
    for(ast_t* child = ast_child(super);
      child != NULL;
      child = ast_sibling(child))
    {
      is_subtype(sub, child, errorf, opt);
    }

    ast_error_frame(errorf, sub, "%s is not a subtype of any element of %s",
      ast_print_type(sub), ast_print_type(super));
  }

  return false;
}

static bool is_union_sub_x(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  // T1 <: T3
  // T2 <: T3
  // ---
  // (T1 | T2) <: T3
  for(ast_t* child = ast_child(sub);
    child != NULL;
    child = ast_sibling(child))
  {
    if(!is_subtype(child, super, errorf, opt))
    {
      if(errorf != NULL)
      {
        ast_error_frame(errorf, child,
          "not every element of %s is a subtype of %s",
          ast_print_type(sub), ast_print_type(super));
      }

      return false;
    }
  }

  return true;
}

static bool is_isect_sub_isect(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  // (T1 & T2) <: T3
  // (T1 & T2) <: T4
  // ---
  // (T1 & T2) <: (T3 & T4)
  ast_t* super_child = ast_child(super);

  while(super_child != NULL)
  {
    if(!is_isect_sub_x(sub, super_child, errorf, opt))
      return false;

    super_child = ast_sibling(super_child);
  }

  return true;
}

static bool is_isect_sub_x(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  switch(ast_id(super))
  {
    case TK_ISECTTYPE:
      // (T1 & T2) <: (T3 & T4)
      return is_isect_sub_isect(sub, super, errorf, opt);

    case TK_NOMINAL:
    {
      ast_t* super_def = (ast_t*)ast_data(super);

      // TODO: can satisfy the interface in aggregate
      // (T1 & T2) <: I k
      if(ast_id(super_def) == TK_INTERFACE)
      {
        // return is_isect_sub_interface(sub, super, errorf);
      }
      break;
    }

    default: {}
  }

  // T1 <: T3 or T2 <: T3
  // ---
  // (T1 & T2) <: T3
  for(ast_t* child = ast_child(sub);
    child != NULL;
    child = ast_sibling(child))
  {
    if(is_subtype(child, super, NULL, opt))
      return true;
  }

  if(errorf != NULL)
  {
    ast_error_frame(errorf, sub, "no element of %s is a subtype of %s",
      ast_print_type(sub), ast_print_type(super));
  }

  return false;
}

static bool is_tuple_sub_tuple(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  // T1 <: T3
  // T2 <: T4
  // ---
  // (T1, T2) <: (T3, T4)
  if(ast_childcount(sub) != ast_childcount(super))
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, sub,
        "%s is not a subtype of %s: they have a different number of elements",
        ast_print_type(sub), ast_print_type(super));
    }

    return false;
  }

  ast_t* sub_child = ast_child(sub);
  ast_t* super_child = ast_child(super);
  bool ret = true;

  while(sub_child != NULL)
  {
    if(!is_subtype(sub_child, super_child, errorf, opt))
      ret = false;

    sub_child = ast_sibling(sub_child);
    super_child = ast_sibling(super_child);
  }

  if(!ret && errorf != NULL)
  {
    ast_error_frame(errorf, sub, "%s is not a pairwise subtype of %s",
      ast_print_type(sub), ast_print_type(super));
  }

  return ret;
}

static bool is_tuple_sub_single(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  (void)opt;
  if(errorf != NULL)
  {
    ast_error_frame(errorf, sub,
      "%s is not a subytpe of %s: the subtype is a tuple",
      ast_print_type(sub), ast_print_type(super));
  }

  return false;
}

static bool is_single_sub_tuple(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  (void)opt;
  if(errorf != NULL)
  {
    ast_error_frame(errorf, sub,
      "%s is not a subytpe of %s: the supertype is a tuple",
      ast_print_type(sub), ast_print_type(super));
  }

  return false;
}

static bool is_tuple_sub_x(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  switch(ast_id(super))
  {
    case TK_UNIONTYPE:
      return is_x_sub_union(sub, super, errorf, opt);

    case TK_ISECTTYPE:
      return is_x_sub_isect(sub, super, errorf, opt);

    case TK_TUPLETYPE:
      return is_tuple_sub_tuple(sub, super, errorf, opt);

    case TK_NOMINAL:
    case TK_TYPEPARAMREF:
    case TK_ARROW:
      return is_tuple_sub_single(sub, super, errorf, opt);

    case TK_FUNTYPE:
    case TK_INFERTYPE:
    case TK_ERRORTYPE:
      return false;

    default: {}
  }

  assert(0);
  return false;
}

static bool is_nominal_sub_entity(ast_t* sub, ast_t* super,
  errorframe_t* errorf, pass_opt_t* opt)
{
  // N = C
  // k <: k'
  // ---
  // N k <: C k'
  ast_t* sub_def = (ast_t*)ast_data(sub);
  ast_t* super_def = (ast_t*)ast_data(super);
  bool ret = true;

  if(sub_def != super_def)
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, sub, "%s is not a subtype of %s",
        ast_print_type(sub), ast_print_type(super));
    }

    return false;
  }

  if(!is_eq_typeargs(sub, super, errorf, opt))
    ret = false;

  if(!is_sub_cap_and_eph(sub, super, errorf, opt))
    ret = false;

  return ret;
}

static bool is_nominal_sub_structural(ast_t* sub, ast_t* super,
  errorframe_t* errorf, pass_opt_t* opt)
{
  // implements(N, I)
  // k <: k'
  // ---
  // N k <: I k
  ast_t* sub_def = (ast_t*)ast_data(sub);
  ast_t* super_def = (ast_t*)ast_data(super);

  // Add an assumption: sub <: super
  if(push_assume(sub, super, opt))
    return true;

  bool ret = true;

  ast_t* sub_typeargs = ast_childidx(sub, 2);
  ast_t* sub_typeparams = ast_childidx(sub_def, 1);

  ast_t* super_typeargs = ast_childidx(super, 2);
  ast_t* super_typeparams = ast_childidx(super_def, 1);

  ast_t* super_members = ast_childidx(super_def, 4);
  ast_t* super_member = ast_child(super_members);

  while(super_member != NULL)
  {
    ast_t* super_member_id = ast_childidx(super_member, 1);
    ast_t* sub_member = ast_get(sub_def, ast_name(super_member_id), NULL);

    // If we don't provide a method, we aren't a subtype.
    if((sub_member == NULL) || (ast_id(sub_member) != TK_FUN &&
      ast_id(sub_member) != TK_BE && ast_id(sub_member) != TK_NEW))
    {
      if(errorf != NULL)
      {
        ast_error_frame(errorf, sub,
          "%s is not a subtype of %s: it has no method '%s'",
          ast_print_type(sub), ast_print_type(super),
          ast_name(super_member_id));
      }

      ret = false;
      super_member = ast_sibling(super_member);
      continue;
    }

    // Reify the method on the subtype.
    ast_t* r_sub_member = reify_method_def(sub_member, sub_typeparams,
      sub_typeargs, opt);
    assert(r_sub_member != NULL);

    // Reify the method on the supertype.
    ast_t* r_super_member = reify_method_def(super_member, super_typeparams,
      super_typeargs, opt);
    assert(r_super_member != NULL);

    // Check the reified methods.
    bool ok = is_fun_sub_fun(r_sub_member, r_super_member, errorf, opt);
    ast_free_unattached(r_sub_member);
    ast_free_unattached(r_super_member);

    if(!ok)
    {
      ret = false;

      if(errorf != NULL)
      {
        ast_error_frame(errorf, sub_member,
          "%s is not a subtype of %s: "
          "method '%s' has an incompatible signature",
          ast_print_type(sub), ast_print_type(super),
          ast_name(super_member_id));
      }
    }

    super_member = ast_sibling(super_member);
  }

  pop_assume();
  return ret;
}

static bool is_nominal_sub_interface(ast_t* sub, ast_t* super,
  errorframe_t* errorf, pass_opt_t* opt)
{
  bool ret = true;

  // A struct has no descriptor, so can't be a subtype of an interface.
  ast_t* sub_def = (ast_t*)ast_data(sub);

  if(ast_id(sub_def) == TK_STRUCT)
  {
    struct_cant_be_x(sub, super, errorf, "an interface");
    ret = false;
  }

  if(!is_sub_cap_and_eph(sub, super, errorf, opt))
    ret = false;

  if(!is_nominal_sub_structural(sub, super, errorf, opt))
    ret = false;

  return ret;
}

static bool nominal_provides_trait(ast_t* type, ast_t* trait,
  errorframe_t* errorf, pass_opt_t* opt)
{
  // Get our typeparams and typeargs.
  ast_t* def = (ast_t*)ast_data(type);
  AST_GET_CHILDREN(def, id, typeparams, defcap, traits);
  ast_t* typeargs = ast_childidx(type, 2);

  // Get cap and eph of the trait.
  AST_GET_CHILDREN(trait, t_pkg, t_name, t_typeparams, cap, eph);
  token_id tcap = ast_id(cap);
  token_id teph = ast_id(eph);

  // Check traits, depth first.
  ast_t* child = ast_child(traits);

  while(child != NULL)
  {
    // Reify the child with our typeargs.
    ast_t* r_child = reify(child, typeparams, typeargs, opt, true);
    assert(r_child != NULL);

    // Use the cap and ephemerality of the trait.
    ast_t* rr_child = set_cap_and_ephemeral(r_child, tcap, teph);
    bool is_sub = is_subtype(rr_child, trait, NULL, opt);
    ast_free_unattached(rr_child);

    if(r_child != child)
      ast_free_unattached(r_child);

    if(is_sub)
      return true;

    child = ast_sibling(child);
  }

  if(errorf != NULL)
  {
    ast_error_frame(errorf, type, "%s does not implement trait %s",
      ast_print_type(type), ast_print_type(trait));
  }

  return false;
}

static bool is_entity_sub_trait(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  // implements(C, R)
  // k <: k'
  // ---
  // C k <: R k'
  return nominal_provides_trait(sub, super, errorf, opt) &&
    is_sub_cap_and_eph(sub, super, errorf, opt);
}

static bool is_struct_sub_trait(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  if(check_assume(sub, super, opt))
    return true;

  struct_cant_be_x(sub, super, errorf, "a trait");
  return false;
}

static bool is_trait_sub_trait(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  // R = R' or implements(R, R')
  // k <: k'
  // ---
  // R k <: R' k'
  ast_t* sub_def = (ast_t*)ast_data(sub);
  ast_t* super_def = (ast_t*)ast_data(super);
  bool ret = true;

  if(sub_def == super_def)
  {
    if(!is_eq_typeargs(sub, super, errorf, opt))
      ret = false;
  }
  else if(!nominal_provides_trait(sub, super, errorf, opt))
  {
    ret = false;
  }

  if(!is_sub_cap_and_eph(sub, super, errorf, opt))
    ret = false;

  return ret;
}

static bool is_interface_sub_trait(ast_t* sub, ast_t* super,
  errorframe_t* errorf)
{
  if(errorf != NULL)
  {
    ast_error_frame(errorf, sub,
      "%s is not a subtype of %s: an interface can't be a subtype of a trait",
      ast_print_type(sub), ast_print_type(super));
  }

  return false;
}

static bool is_nominal_sub_trait(ast_t* sub, ast_t* super,
  errorframe_t* errorf, pass_opt_t* opt)
{
  // N k <: I k'
  ast_t* sub_def = (ast_t*)ast_data(sub);

  // A struct has no descriptor, so can't be a subtype of a trait.
  switch(ast_id(sub_def))
  {
    case TK_PRIMITIVE:
    case TK_CLASS:
    case TK_ACTOR:
      return is_entity_sub_trait(sub, super, errorf, opt);

    case TK_STRUCT:
      return is_struct_sub_trait(sub, super, errorf, opt);

    case TK_TRAIT:
      return is_trait_sub_trait(sub, super, errorf, opt);

    case TK_INTERFACE:
      return is_interface_sub_trait(sub, super, errorf);

    default: {}
  }

  assert(0);
  return false;
}

static bool is_nominal_sub_nominal(ast_t* sub, ast_t* super,
  errorframe_t* errorf, pass_opt_t* opt)
{
  // N k <: N' k'
  ast_t* super_def = (ast_t*)ast_data(super);

  switch(ast_id(super_def))
  {
    case TK_PRIMITIVE:
    case TK_STRUCT:
    case TK_CLASS:
    case TK_ACTOR:
      return is_nominal_sub_entity(sub, super, errorf, opt);

    case TK_INTERFACE:
      return is_nominal_sub_interface(sub, super, errorf, opt);

    case TK_TRAIT:
      return is_nominal_sub_trait(sub, super, errorf, opt);

    default: {}
  }

  assert(0);
  return false;
}

static bool is_nominal_sub_typeparam(ast_t* sub, ast_t* super,
  errorframe_t* errorf, pass_opt_t* opt)
{
  // N k <: lowerbound(A k')
  // ---
  // N k <: A k'
  ast_t* super_lower = typeparam_lower(super);

  if(super_lower == NULL)
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, sub,
        "%s is not a subtype of %s: the type parameter has no lower bounds",
        ast_print_type(sub), ast_print_type(super));
    }

    return false;
  }

  bool ok = is_subtype(sub, super_lower, errorf, opt);
  ast_free_unattached(super_lower);
  return ok;
}

static bool is_nominal_sub_arrow(ast_t* sub, ast_t* super,
  errorframe_t* errorf, pass_opt_t* opt)
{
  // N k <: lowerbound(T1->T2)
  // ---
  // N k <: T1->T2
  ast_t* super_lower = viewpoint_lower(super);

  if(super_lower == NULL)
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, sub,
        "%s is not a subtype of %s: the supertype has no lower bounds",
        ast_print_type(sub), ast_print_type(super));
    }

    return false;
  }

  bool ok = is_subtype(sub, super_lower, errorf, opt);
  ast_free_unattached(super_lower);
  return ok;
}

static bool is_nominal_sub_x(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  // N k <: T
  switch(ast_id(super))
  {
    case TK_UNIONTYPE:
    {
      ast_t* def = (ast_t*)ast_data(sub);

      if(ast_id(def) == TK_STRUCT)
      {
        struct_cant_be_x(sub, super, errorf, "a union type");
        return false;
      }

      return is_x_sub_union(sub, super, errorf, opt);
    }

    case TK_ISECTTYPE:
    {
      ast_t* def = (ast_t*)ast_data(sub);

      if(ast_id(def) == TK_STRUCT)
      {
        struct_cant_be_x(sub, super, errorf, "an intersection type");
        return false;
      }

      return is_x_sub_isect(sub, super, errorf, opt);
    }

    case TK_TUPLETYPE:
      return is_single_sub_tuple(sub, super, errorf, opt);

    case TK_NOMINAL:
      return is_nominal_sub_nominal(sub, super, errorf, opt);

    case TK_TYPEPARAMREF:
      return is_nominal_sub_typeparam(sub, super, errorf, opt);

    case TK_ARROW:
      return is_nominal_sub_arrow(sub, super, errorf, opt);

    case TK_FUNTYPE:
    case TK_INFERTYPE:
    case TK_ERRORTYPE:
      return false;

    default: {}
  }

  assert(0);
  return false;
}

static bool is_typeparam_sub_typeparam(ast_t* sub, ast_t* super,
  errorframe_t* errorf, pass_opt_t* opt)
{
  // k <: k'
  // ---
  // A k <: A k'
  (void)opt;
  ast_t* sub_def = (ast_t*)ast_data(sub);
  ast_t* super_def = (ast_t*)ast_data(super);

  if(sub_def == super_def)
  {
    // We know the bounds on the rcap is the same, so we have different
    // subtyping rules here.
    ast_t* sub_cap = cap_fetch(sub);
    ast_t* sub_eph = ast_sibling(sub_cap);
    ast_t* super_cap = cap_fetch(super);
    ast_t* super_eph = ast_sibling(super_cap);

    if(!is_cap_sub_cap_bound(ast_id(sub_cap), ast_id(sub_eph),
      ast_id(super_cap), ast_id(super_eph)))
    {
      if(errorf != NULL)
      {
        ast_error_frame(errorf, sub,
          "%s is not a subtype of %s: %s%s is not a subtype of %s%s",
          ast_print_type(sub), ast_print_type(super),
          ast_print_type(sub_cap), ast_print_type(sub_eph),
          ast_print_type(super_cap), ast_print_type(super_eph));
      }

      return false;
    }

    return true;
  }

  if(errorf != NULL)
  {
    ast_error_frame(errorf, sub,
      "%s is not a subtype of %s: they are different type parameters",
      ast_print_type(sub), ast_print_type(super));
  }

  return false;
}

static bool is_typeparam_sub_arrow(ast_t* sub, ast_t* super,
  errorframe_t* errorf, pass_opt_t* opt)
{
  // forall k' in k . A k' <: lowerbound(T1->T2 {A k |-> A k'})
  // ---
  // A k <: T1->T2
  ast_t* r_sub = viewpoint_reifytypeparam(sub, sub);
  ast_t* r_super = viewpoint_reifytypeparam(super, sub);

  if(r_sub != NULL)
  {
    bool ok = is_subtype(r_sub, r_super, errorf, opt);
    ast_free_unattached(r_sub);
    ast_free_unattached(r_super);
    return ok;
  }

  // If there is only a single instantiation, calculate the lower bounds.
  //
  // A k <: lowerbound(T1->T2)
  // ---
  // A k <: T1->T2
  ast_t* super_lower = viewpoint_lower(super);

  if(super_lower == NULL)
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, sub,
        "%s is not a subtype of %s: the supertype has no lower bounds",
        ast_print_type(sub), ast_print_type(super));
    }

    return false;
  }

  bool ok = is_subtype(sub, super_lower, errorf, opt);
  ast_free_unattached(super_lower);
  return ok;
}

static bool is_typeparam_base_sub_x(ast_t* sub, ast_t* super,
  errorframe_t* errorf, pass_opt_t* opt)
{
  switch(ast_id(super))
  {
    case TK_UNIONTYPE:
      return is_x_sub_union(sub, super, errorf, opt);

    case TK_ISECTTYPE:
      return is_x_sub_isect(sub, super, errorf, opt);

    case TK_TUPLETYPE:
    case TK_NOMINAL:
      return false;

    case TK_TYPEPARAMREF:
      return is_typeparam_sub_typeparam(sub, super, errorf, opt);

    case TK_ARROW:
      return is_typeparam_sub_arrow(sub, super, errorf, opt);

    case TK_FUNTYPE:
    case TK_INFERTYPE:
    case TK_ERRORTYPE:
      return false;

    default: {}
  }

  assert(0);
  return false;
}

static bool is_typeparam_sub_x(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  if(is_typeparam_base_sub_x(sub, super, false, opt))
    return true;

  // upperbound(A k) <: T
  // ---
  // A k <: T
  ast_t* sub_upper = typeparam_upper(sub);

  if(sub_upper == NULL)
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, sub,
        "%s is not a subtype of %s: the subtype has no constraint",
        ast_print_type(sub), ast_print_type(super));
    }

    return false;
  }

  bool ok = is_subtype(sub_upper, super, errorf, opt);
  ast_free_unattached(sub_upper);
  return ok;
}

static bool is_arrow_sub_nominal(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  // upperbound(T1->T2) <: N k
  // ---
  // T1->T2 <: N k
  ast_t* sub_upper = viewpoint_upper(sub);

  if(sub_upper == NULL)
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, sub,
        "%s is not a subtype of %s: the subtype has no upper bounds",
        ast_print_type(sub), ast_print_type(super));
    }

    return false;
  }

  bool ok = is_subtype(sub_upper, super, errorf, opt);
  ast_free_unattached(sub_upper);
  return ok;
}

static bool is_arrow_sub_typeparam(ast_t* sub, ast_t* super,
  errorframe_t* errorf, pass_opt_t* opt)
{
  // forall k' in k . T1->T2 {A k |-> A k'} <: A k'
  // ---
  // T1->T2 <: A k
  ast_t* r_sub = viewpoint_reifytypeparam(sub, super);
  ast_t* r_super = viewpoint_reifytypeparam(super, super);

  if(r_sub != NULL)
  {
    bool ok = is_subtype(r_sub, r_super, errorf, opt);
    ast_free_unattached(r_sub);
    ast_free_unattached(r_super);
    return ok;
  }

  // If there is only a single instantiation, treat as a nominal type.
  return is_arrow_sub_nominal(sub, super, errorf, opt);
}

static bool is_arrow_sub_arrow(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  // S = this | A {#read, #send, #share, #any}
  // K = N k | A {iso, trn, ref, val, box, tag} | K->K | (empty)
  // L = S | K
  // T = N k | A k | L->T
  //
  // forall K' in S . K->S->T1 {S |-> K'} <: T2 {S |-> K'}
  // ---
  // K->S->T1 <: T2
  ast_t* r_sub;
  ast_t* r_super;

  if(viewpoint_reifypair(sub, super, &r_sub, &r_super))
  {
    bool ok = is_subtype(r_sub, r_super, errorf, opt);
    ast_free_unattached(r_sub);
    ast_free_unattached(r_super);
    return ok;
  }

  // No elements need reification.
  //
  // upperbound(T1->T2) <: lowerbound(T3->T4)
  // ---
  // T1->T2 <: T3->T4
  ast_t* sub_upper = viewpoint_upper(sub);
  ast_t* super_lower = viewpoint_lower(super);
  bool ok = true;

  if(sub_upper == NULL)
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, sub,
        "%s is not a subtype of %s: the subtype has no upper bounds",
        ast_print_type(sub), ast_print_type(super));
    }

    ok = false;
  }

  if(super_lower == NULL)
  {
    if(errorf != NULL)
    {
      ast_error_frame(errorf, sub,
        "%s is not a subtype of %s: the supertype has no lower bounds",
        ast_print_type(sub), ast_print_type(super));
    }

    ok = false;
  }

  if(ok)
    ok = is_subtype(sub_upper, super_lower, errorf, opt);

  ast_free_unattached(sub_upper);
  ast_free_unattached(super_lower);
  return ok;
}

static bool is_arrow_sub_x(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  switch(ast_id(super))
  {
    case TK_UNIONTYPE:
      return is_x_sub_union(sub, super, errorf, opt);

    case TK_ISECTTYPE:
      return is_x_sub_isect(sub, super, errorf, opt);

    case TK_TUPLETYPE:
      return is_single_sub_tuple(sub, super, errorf, opt);

    case TK_NOMINAL:
      return is_arrow_sub_nominal(sub, super, errorf, opt);

    case TK_TYPEPARAMREF:
      return is_arrow_sub_typeparam(sub, super, errorf, opt);

    case TK_ARROW:
      return is_arrow_sub_arrow(sub, super, errorf, opt);

    case TK_FUNTYPE:
    case TK_INFERTYPE:
    case TK_ERRORTYPE:
      return false;

    default: {}
  }

  assert(0);
  return false;
}

bool is_subtype(ast_t* sub, ast_t* super, errorframe_t* errorf,
  pass_opt_t* opt)
{
  assert(sub != NULL);
  assert(super != NULL);

  if(ast_id(super) == TK_DONTCARETYPE)
    return true;

  switch(ast_id(sub))
  {
    case TK_UNIONTYPE:
      return is_union_sub_x(sub, super, errorf, opt);

    case TK_ISECTTYPE:
      return is_isect_sub_x(sub, super, errorf, opt);

    case TK_TUPLETYPE:
      return is_tuple_sub_x(sub, super, errorf, opt);

    case TK_NOMINAL:
      return is_nominal_sub_x(sub, super, errorf, opt);

    case TK_TYPEPARAMREF:
      return is_typeparam_sub_x(sub, super, errorf, opt);

    case TK_ARROW:
      return is_arrow_sub_x(sub, super, errorf, opt);

    case TK_FUNTYPE:
    case TK_INFERTYPE:
    case TK_ERRORTYPE:
      return false;

    case TK_NEW:
    case TK_BE:
    case TK_FUN:
      return is_fun_sub_fun(sub, super, errorf, opt);

    default: {}
  }

  assert(0);
  return false;
}

bool is_eqtype(ast_t* a, ast_t* b, errorframe_t* errorf, pass_opt_t* opt)
{
  return is_subtype(a, b, errorf, opt) && is_subtype(b, a, errorf, opt);
}

bool is_sub_provides(ast_t* type, ast_t* provides, errorframe_t* errorf,
  pass_opt_t* opt)
{
  assert(ast_id(type) == TK_NOMINAL);
  return is_nominal_sub_structural(type, provides, errorf, opt);
}

bool is_literal(ast_t* type, const char* name)
{
  if(type == NULL)
    return false;

  if(ast_id(type) != TK_NOMINAL)
    return false;

  // Don't have to check the package, since literals are all builtins.
  return !strcmp(ast_name(ast_childidx(type, 1)), name);
}

bool is_pointer(ast_t* type)
{
  return is_literal(type, "Pointer");
}

bool is_maybe(ast_t* type)
{
  return is_literal(type, "MaybePointer");
}

bool is_none(ast_t* type)
{
  return is_literal(type, "None");
}

bool is_env(ast_t* type)
{
  return is_literal(type, "Env");
}

bool is_bool(ast_t* type)
{
  return is_literal(type, "Bool");
}

bool is_float(ast_t* type)
{
  return is_literal(type, "F32") || is_literal(type, "F64");
}

bool is_integer(ast_t* type)
{
  return
    is_literal(type, "I8") ||
    is_literal(type, "I16") ||
    is_literal(type, "I32") ||
    is_literal(type, "I64") ||
    is_literal(type, "I128") ||
    is_literal(type, "ILong") ||
    is_literal(type, "ISize") ||
    is_literal(type, "U8") ||
    is_literal(type, "U16") ||
    is_literal(type, "U32") ||
    is_literal(type, "U64") ||
    is_literal(type, "U128") ||
    is_literal(type, "ULong") ||
    is_literal(type, "USize");
}

bool is_machine_word(ast_t* type)
{
  return is_bool(type) || is_integer(type) || is_float(type);
}

bool is_signed(ast_t* type)
{
  return
    is_literal(type, "I8") ||
    is_literal(type, "I16") ||
    is_literal(type, "I32") ||
    is_literal(type, "I64") ||
    is_literal(type, "I128") ||
    is_literal(type, "ILong") ||
    is_literal(type, "ISize");
}

bool is_constructable(ast_t* type)
{
  if(type == NULL)
    return false;

  switch(ast_id(type))
  {
    case TK_UNIONTYPE:
    case TK_TUPLETYPE:
      return false;

    case TK_ISECTTYPE:
    {
      ast_t* child = ast_child(type);

      while(child != NULL)
      {
        if(is_constructable(child))
          return true;

        child = ast_sibling(child);
      }

      return false;
    }

    case TK_NOMINAL:
    {
      ast_t* def = (ast_t*)ast_data(type);

      switch(ast_id(def))
      {
        case TK_INTERFACE:
        case TK_TRAIT:
        {
          ast_t* members = ast_childidx(def, 4);
          ast_t* member = ast_child(members);

          while(member != NULL)
          {
            if(ast_id(member) == TK_NEW)
              return true;

            member = ast_sibling(member);
          }

          return false;
        }

        case TK_PRIMITIVE:
        case TK_STRUCT:
        case TK_CLASS:
        case TK_ACTOR:
          return true;

        default: {}
      }
      break;
    }

    case TK_TYPEPARAMREF:
      return is_constructable(typeparam_constraint(type));

    case TK_ARROW:
      return is_constructable(ast_childidx(type, 1));

    default: {}
  }

  assert(0);
  return false;
}

bool is_concrete(ast_t* type)
{
  if(type == NULL)
    return false;

  switch(ast_id(type))
  {
    case TK_UNIONTYPE:
    case TK_TUPLETYPE:
      return false;

    case TK_ISECTTYPE:
    {
      ast_t* child = ast_child(type);

      while(child != NULL)
      {
        if(is_concrete(child))
          return true;

        child = ast_sibling(child);
      }

      return false;
    }

    case TK_NOMINAL:
    {
      ast_t* def = (ast_t*)ast_data(type);

      switch(ast_id(def))
      {
        case TK_INTERFACE:
        case TK_TRAIT:
          return false;

        case TK_PRIMITIVE:
        case TK_STRUCT:
        case TK_CLASS:
        case TK_ACTOR:
          return true;

        default: {}
      }
      break;
    }

    case TK_TYPEPARAMREF:
      return is_constructable(typeparam_constraint(type));

    case TK_ARROW:
      return is_concrete(ast_childidx(type, 1));

    default: {}
  }

  assert(0);
  return false;
}

bool is_known(ast_t* type)
{
  if(type == NULL)
    return false;

  switch(ast_id(type))
  {
    case TK_UNIONTYPE:
    case TK_TUPLETYPE:
      return false;

    case TK_ISECTTYPE:
    {
      ast_t* child = ast_child(type);

      while(child != NULL)
      {
        if(is_known(child))
          return true;

        child = ast_sibling(child);
      }

      return false;
    }

    case TK_NOMINAL:
    {
      ast_t* def = (ast_t*)ast_data(type);

      switch(ast_id(def))
      {
        case TK_INTERFACE:
        case TK_TRAIT:
          return false;

        case TK_PRIMITIVE:
        case TK_STRUCT:
        case TK_CLASS:
        case TK_ACTOR:
          return true;

        default: {}
      }
      break;
    }

    case TK_ARROW:
      return is_known(ast_childidx(type, 1));

    case TK_TYPEPARAMREF:
      return is_known(typeparam_constraint(type));

    default: {}
  }

  assert(0);
  return false;
}

bool is_entity(ast_t* type, token_id entity)
{
  if(type == NULL)
    return false;

  switch(ast_id(type))
  {
    case TK_TUPLETYPE:
      return false;

    case TK_UNIONTYPE:
    {
      ast_t* child = ast_child(type);

      while(child != NULL)
      {
        if(!is_entity(child, entity))
          return false;

        child = ast_sibling(child);
      }

      return true;
    }

    case TK_ISECTTYPE:
    {
      ast_t* child = ast_child(type);

      while(child != NULL)
      {
        if(is_entity(child, entity))
          return true;

        child = ast_sibling(child);
      }

      return false;
    }

    case TK_NOMINAL:
    {
      ast_t* def = (ast_t*)ast_data(type);
      return ast_id(def) == entity;
    }

    case TK_ARROW:
      return is_entity(ast_childidx(type, 1), entity);

    case TK_TYPEPARAMREF:
      return is_entity(typeparam_constraint(type), entity);

    default: {}
  }

  assert(0);
  return false;
}

bool contains_dontcare(ast_t* ast)
{
  switch(ast_id(ast))
  {
    case TK_DONTCARETYPE:
      return true;

    case TK_TUPLETYPE:
    {
      ast_t* child = ast_child(ast);

      while(child != NULL)
      {
        if(contains_dontcare(child))
          return true;

        child = ast_sibling(child);
      }

      return false;
    }

    default: {}
  }

  return false;
}
