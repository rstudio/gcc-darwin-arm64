/* Builtins implementation for RISC-V 'V' Extension for GNU compiler.
   Copyright (C) 2022-2023 Free Software Foundation, Inc.
   Contributed by Ju-Zhe Zhong (juzhe.zhong@rivai.ai), RiVAI Technologies Ltd.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GCC is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

#define IN_TARGET_CODE 1

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "tree.h"
#include "rtl.h"
#include "tm_p.h"
#include "memmodel.h"
#include "insn-codes.h"
#include "optabs.h"
#include "recog.h"
#include "diagnostic.h"
#include "expr.h"
#include "function.h"
#include "fold-const.h"
#include "gimplify.h"
#include "explow.h"
#include "stor-layout.h"
#include "alias.h"
#include "langhooks.h"
#include "stringpool.h"
#include "attribs.h"
#include "targhooks.h"
#include "regs.h"
#include "emit-rtl.h"
#include "riscv-vector-builtins.h"
#include "riscv-vector-builtins-shapes.h"
#include "riscv-vector-builtins-bases.h"

using namespace riscv_vector;

namespace riscv_vector {

/* Static information about each vector type.  */
struct vector_type_info
{
  /* The name of the type as declared by riscv_vector.h
     which is recommend to use. For example: 'vint32m1_t'.  */
  const char *name;

  /* ABI name of vector type. The type is always available
     under this name, even when riscv_vector.h isn't included.
     For example:  '__rvv_int32m1_t'.  */
  const char *abi_name;

  /* The C++ mangling of ABI_NAME.  */
  const char *mangled_name;
};

/* Describes a function decl.  */
class GTY (()) registered_function
{
public:
  function_instance GTY ((skip)) instance;

  /* The decl itself.  */
  tree GTY ((skip)) decl;
};

/* Hash traits for registered_function.  */
struct registered_function_hasher : nofree_ptr_hash<registered_function>
{
  typedef function_instance compare_type;

  static hashval_t hash (value_type);
  static bool equal (value_type, const compare_type &);
};

/* Static information about each RVV type.  */
static CONSTEXPR const vector_type_info vector_types[] = {
#define DEF_RVV_TYPE(NAME, NCHARS, ABI_NAME, ARGS...)                          \
  {#NAME, #ABI_NAME, "u" #NCHARS #ABI_NAME},
#include "riscv-vector-builtins.def"
};

/* Static information about operand suffix for each RVV type.  */
const char *const operand_suffixes[NUM_OP_TYPES] = {
  "", /* OP_TYPE_none.  */
#define DEF_RVV_OP_TYPE(NAME) "_" # NAME,
#include "riscv-vector-builtins.def"
};

/* Static information about type suffix for each RVV type.  */
const rvv_builtin_suffixes type_suffixes[NUM_VECTOR_TYPES + 1] = {
#define DEF_RVV_TYPE(NAME, NCHARS, ABI_NAME, SCALAR_TYPE, VECTOR_MODE,         \
		     VECTOR_MODE_MIN_VLEN_32, VECTOR_SUFFIX, SCALAR_SUFFIX,    \
		     VSETVL_SUFFIX, MASK_TYPE)                                 \
  {#VECTOR_SUFFIX, #SCALAR_SUFFIX, #VSETVL_SUFFIX},
#include "riscv-vector-builtins.def"
};

/* Mask type for each RVV type.  */
const vector_type_index mask_types[NUM_VECTOR_TYPES + 1] = {
#define DEF_RVV_TYPE(NAME, NCHARS, ABI_NAME, SCALAR_TYPE, VECTOR_MODE,         \
		     VECTOR_MODE_MIN_VLEN_32, VECTOR_SUFFIX, SCALAR_SUFFIX,    \
		     VSETVL_SUFFIX, MASK_TYPE)                                 \
  VECTOR_TYPE_##MASK_TYPE,
#include "riscv-vector-builtins.def"
};

/* Static information about predication suffix for each RVV type.  */
const char *const predication_suffixes[NUM_PRED_TYPES] = {
  "", /* PRED_TYPE_none.  */
#define DEF_RVV_PRED_TYPE(NAME) "_" # NAME,
#include "riscv-vector-builtins.def"
};

/* A list of all signed integer will be registered for intrinsic functions.  */
static const rvv_type_info i_ops[] = {
#define DEF_RVV_I_OPS(TYPE, REQUIRE) {VECTOR_TYPE_##TYPE, REQUIRE},
#include "riscv-vector-builtins-types.def"
  {NUM_VECTOR_TYPES, 0}};

/* A list of all integer will be registered for intrinsic functions.  */
static const rvv_type_info iu_ops[] = {
#define DEF_RVV_I_OPS(TYPE, REQUIRE) {VECTOR_TYPE_##TYPE, REQUIRE},
#define DEF_RVV_U_OPS(TYPE, REQUIRE) {VECTOR_TYPE_##TYPE, REQUIRE},
#include "riscv-vector-builtins-types.def"
  {NUM_VECTOR_TYPES, 0}};

/* A list of all types will be registered for intrinsic functions.  */
static const rvv_type_info all_ops[] = {
#define DEF_RVV_I_OPS(TYPE, REQUIRE) {VECTOR_TYPE_##TYPE, REQUIRE},
#define DEF_RVV_U_OPS(TYPE, REQUIRE) {VECTOR_TYPE_##TYPE, REQUIRE},
#define DEF_RVV_F_OPS(TYPE, REQUIRE) {VECTOR_TYPE_##TYPE, REQUIRE},
#include "riscv-vector-builtins-types.def"
  {NUM_VECTOR_TYPES, 0}};

/* A list of all bool will be registered for intrinsic functions.  */
static const rvv_type_info b_ops[] = {
#define DEF_RVV_B_OPS(TYPE, REQUIRE) {VECTOR_TYPE_##TYPE, REQUIRE},
#include "riscv-vector-builtins-types.def"
  {NUM_VECTOR_TYPES, 0}};

static CONSTEXPR const rvv_arg_type_info rvv_arg_type_info_end
  = rvv_arg_type_info (NUM_BASE_TYPES);

/* A list of args for size_t func (void) function.  */
static CONSTEXPR const rvv_arg_type_info void_args[]
  = {rvv_arg_type_info (RVV_BASE_void), rvv_arg_type_info_end};

/* A list of args for size_t func (size_t) function.  */
static CONSTEXPR const rvv_arg_type_info size_args[]
  = {rvv_arg_type_info (RVV_BASE_size), rvv_arg_type_info_end};

/* A list of args for vector_type func (const scalar_type *) function.  */
static CONSTEXPR const rvv_arg_type_info scalar_const_ptr_args[]
  = {rvv_arg_type_info (RVV_BASE_scalar_const_ptr), rvv_arg_type_info_end};

/* A list of args for void func (scalar_type *, vector_type) function.  */
static CONSTEXPR const rvv_arg_type_info scalar_ptr_args[]
  = {rvv_arg_type_info (RVV_BASE_scalar_ptr),
     rvv_arg_type_info (RVV_BASE_vector), rvv_arg_type_info_end};

/* A list of args for vector_type func (const scalar_type *, ptrdiff_t)
 * function.  */
static CONSTEXPR const rvv_arg_type_info scalar_const_ptr_ptrdiff_args[]
  = {rvv_arg_type_info (RVV_BASE_scalar_const_ptr),
     rvv_arg_type_info (RVV_BASE_ptrdiff), rvv_arg_type_info_end};

/* A list of args for void func (scalar_type *, ptrdiff_t, vector_type)
 * function.  */
static CONSTEXPR const rvv_arg_type_info scalar_ptr_ptrdiff_args[]
  = {rvv_arg_type_info (RVV_BASE_scalar_ptr),
     rvv_arg_type_info (RVV_BASE_ptrdiff), rvv_arg_type_info (RVV_BASE_vector),
     rvv_arg_type_info_end};

/* A list of args for vector_type func (const scalar_type *, uint8_index_type)
 * function.  */
static CONSTEXPR const rvv_arg_type_info scalar_const_ptr_uint8_index_args[]
  = {rvv_arg_type_info (RVV_BASE_scalar_const_ptr),
     rvv_arg_type_info (RVV_BASE_uint8_index), rvv_arg_type_info_end};

/* A list of args for vector_type func (const scalar_type *, uint16_index_type)
 * function.  */
static CONSTEXPR const rvv_arg_type_info scalar_const_ptr_uint16_index_args[]
  = {rvv_arg_type_info (RVV_BASE_scalar_const_ptr),
     rvv_arg_type_info (RVV_BASE_uint16_index), rvv_arg_type_info_end};

/* A list of args for vector_type func (const scalar_type *, uint32_index_type)
 * function.  */
static CONSTEXPR const rvv_arg_type_info scalar_const_ptr_uint32_index_args[]
  = {rvv_arg_type_info (RVV_BASE_scalar_const_ptr),
     rvv_arg_type_info (RVV_BASE_uint32_index), rvv_arg_type_info_end};

/* A list of args for vector_type func (const scalar_type *, uint64_index_type)
 * function.  */
static CONSTEXPR const rvv_arg_type_info scalar_const_ptr_uint64_index_args[]
  = {rvv_arg_type_info (RVV_BASE_scalar_const_ptr),
     rvv_arg_type_info (RVV_BASE_uint64_index), rvv_arg_type_info_end};

/* A list of args for void func (scalar_type *, uint8_index_type, vector_type)
 * function.  */
static CONSTEXPR const rvv_arg_type_info scalar_ptr_uint8_index_args[]
  = {rvv_arg_type_info (RVV_BASE_scalar_ptr),
     rvv_arg_type_info (RVV_BASE_uint8_index),
     rvv_arg_type_info (RVV_BASE_vector), rvv_arg_type_info_end};

/* A list of args for void func (scalar_type *, uint16_index_type, vector_type)
 * function.  */
static CONSTEXPR const rvv_arg_type_info scalar_ptr_uint16_index_args[]
  = {rvv_arg_type_info (RVV_BASE_scalar_ptr),
     rvv_arg_type_info (RVV_BASE_uint16_index),
     rvv_arg_type_info (RVV_BASE_vector), rvv_arg_type_info_end};

/* A list of args for void func (scalar_type *, uint32_index_type, vector_type)
 * function.  */
static CONSTEXPR const rvv_arg_type_info scalar_ptr_uint32_index_args[]
  = {rvv_arg_type_info (RVV_BASE_scalar_ptr),
     rvv_arg_type_info (RVV_BASE_uint32_index),
     rvv_arg_type_info (RVV_BASE_vector), rvv_arg_type_info_end};

/* A list of args for void func (scalar_type *, uint64_index_type, vector_type)
 * function.  */
static CONSTEXPR const rvv_arg_type_info scalar_ptr_uint64_index_args[]
  = {rvv_arg_type_info (RVV_BASE_scalar_ptr),
     rvv_arg_type_info (RVV_BASE_uint64_index),
     rvv_arg_type_info (RVV_BASE_vector), rvv_arg_type_info_end};

/* A list of args for vector_type func (vector_type, vector_type) function.  */
static CONSTEXPR const rvv_arg_type_info vv_args[]
  = {rvv_arg_type_info (RVV_BASE_vector), rvv_arg_type_info (RVV_BASE_vector),
     rvv_arg_type_info_end};

/* A list of args for vector_type func (vector_type, shift_type) function.  */
static CONSTEXPR const rvv_arg_type_info shift_vv_args[]
  = {rvv_arg_type_info (RVV_BASE_vector),
     rvv_arg_type_info (RVV_BASE_shift_vector), rvv_arg_type_info_end};

/* A list of none preds that will be registered for intrinsic functions.  */
static CONSTEXPR const predication_type_index none_preds[]
  = {PRED_TYPE_none, NUM_PRED_TYPES};

/* vop/vop_m/vop_tu/vop_tum/vop_tumu/vop_mu will be registered.  */
static CONSTEXPR const predication_type_index full_preds[]
  = {PRED_TYPE_none, PRED_TYPE_m,  PRED_TYPE_tu,  PRED_TYPE_tum,
     PRED_TYPE_tumu, PRED_TYPE_mu, NUM_PRED_TYPES};

/* vop/vop_m will be registered.  */
static CONSTEXPR const predication_type_index none_m_preds[]
  = {PRED_TYPE_none, PRED_TYPE_m, NUM_PRED_TYPES};

/* A static operand information for size_t func (void) function registration. */
static CONSTEXPR const rvv_op_info i_none_size_void_ops
  = {i_ops,				/* Types */
     OP_TYPE_none,			/* Suffix */
     rvv_arg_type_info (RVV_BASE_size), /* Return type */
     void_args /* Args */};

/* A static operand information for size_t func (size_t) function registration.
 */
static CONSTEXPR const rvv_op_info i_none_size_size_ops
  = {i_ops,				/* Types */
     OP_TYPE_none,			/* Suffix */
     rvv_arg_type_info (RVV_BASE_size), /* Return type */
     size_args /* Args */};

/* A static operand information for vector_type func (const scalar_type *)
 * function registration. */
static CONSTEXPR const rvv_op_info all_v_scalar_const_ptr_ops
  = {all_ops,				  /* Types */
     OP_TYPE_v,				  /* Suffix */
     rvv_arg_type_info (RVV_BASE_vector), /* Return type */
     scalar_const_ptr_args /* Args */};

/* A static operand information for void func (scalar_type *, vector_type)
 * function registration. */
static CONSTEXPR const rvv_op_info all_v_scalar_ptr_ops
  = {all_ops,				/* Types */
     OP_TYPE_v,				/* Suffix */
     rvv_arg_type_info (RVV_BASE_void), /* Return type */
     scalar_ptr_args /* Args */};

/* A static operand information for vector_type func (const scalar_type *)
 * function registration. */
static CONSTEXPR const rvv_op_info b_v_scalar_const_ptr_ops
  = {b_ops,				  /* Types */
     OP_TYPE_v,				  /* Suffix */
     rvv_arg_type_info (RVV_BASE_vector), /* Return type */
     scalar_const_ptr_args /* Args */};

/* A static operand information for void func (scalar_type *, vector_type)
 * function registration. */
static CONSTEXPR const rvv_op_info b_v_scalar_ptr_ops
  = {b_ops,				/* Types */
     OP_TYPE_v,				/* Suffix */
     rvv_arg_type_info (RVV_BASE_void), /* Return type */
     scalar_ptr_args /* Args */};

/* A static operand information for vector_type func (const scalar_type *,
 * ptrdiff_t) function registration. */
static CONSTEXPR const rvv_op_info all_v_scalar_const_ptr_ptrdiff_ops
  = {all_ops,				  /* Types */
     OP_TYPE_v,				  /* Suffix */
     rvv_arg_type_info (RVV_BASE_vector), /* Return type */
     scalar_const_ptr_ptrdiff_args /* Args */};

/* A static operand information for vector_type func (const scalar_type *,
 * uint8_index_type) function registration. */
static CONSTEXPR const rvv_op_info all_v_scalar_const_ptr_uint8_index_ops
  = {all_ops,				  /* Types */
     OP_TYPE_v,				  /* Suffix */
     rvv_arg_type_info (RVV_BASE_vector), /* Return type */
     scalar_const_ptr_uint8_index_args /* Args */};

/* A static operand information for vector_type func (const scalar_type *,
 * uint16_index_type) function registration. */
static CONSTEXPR const rvv_op_info all_v_scalar_const_ptr_uint16_index_ops
  = {all_ops,				  /* Types */
     OP_TYPE_v,				  /* Suffix */
     rvv_arg_type_info (RVV_BASE_vector), /* Return type */
     scalar_const_ptr_uint16_index_args /* Args */};

/* A static operand information for vector_type func (const scalar_type *,
 * uint32_index_type) function registration. */
static CONSTEXPR const rvv_op_info all_v_scalar_const_ptr_uint32_index_ops
  = {all_ops,				  /* Types */
     OP_TYPE_v,				  /* Suffix */
     rvv_arg_type_info (RVV_BASE_vector), /* Return type */
     scalar_const_ptr_uint32_index_args /* Args */};

/* A static operand information for vector_type func (const scalar_type *,
 * uint64_index_type) function registration. */
static CONSTEXPR const rvv_op_info all_v_scalar_const_ptr_uint64_index_ops
  = {all_ops,				  /* Types */
     OP_TYPE_v,				  /* Suffix */
     rvv_arg_type_info (RVV_BASE_vector), /* Return type */
     scalar_const_ptr_uint64_index_args /* Args */};

/* A static operand information for void func (scalar_type *, ptrdiff_t,
 * vector_type) function registration. */
static CONSTEXPR const rvv_op_info all_v_scalar_ptr_ptrdiff_ops
  = {all_ops,				/* Types */
     OP_TYPE_v,				/* Suffix */
     rvv_arg_type_info (RVV_BASE_void), /* Return type */
     scalar_ptr_ptrdiff_args /* Args */};

/* A static operand information for void func (scalar_type *, uint8_index_type,
 * vector_type) function registration. */
static CONSTEXPR const rvv_op_info all_v_scalar_ptr_uint8_index_ops
  = {all_ops,				/* Types */
     OP_TYPE_v,				/* Suffix */
     rvv_arg_type_info (RVV_BASE_void), /* Return type */
     scalar_ptr_uint8_index_args /* Args */};

/* A static operand information for void func (scalar_type *, uint16_index_type,
 * vector_type) function registration. */
static CONSTEXPR const rvv_op_info all_v_scalar_ptr_uint16_index_ops
  = {all_ops,				/* Types */
     OP_TYPE_v,				/* Suffix */
     rvv_arg_type_info (RVV_BASE_void), /* Return type */
     scalar_ptr_uint16_index_args /* Args */};

/* A static operand information for void func (scalar_type *, uint32_index_type,
 * vector_type) function registration. */
static CONSTEXPR const rvv_op_info all_v_scalar_ptr_uint32_index_ops
  = {all_ops,				/* Types */
     OP_TYPE_v,				/* Suffix */
     rvv_arg_type_info (RVV_BASE_void), /* Return type */
     scalar_ptr_uint32_index_args /* Args */};

/* A static operand information for void func (scalar_type *, uint64_index_type,
 * vector_type) function registration. */
static CONSTEXPR const rvv_op_info all_v_scalar_ptr_uint64_index_ops
  = {all_ops,				/* Types */
     OP_TYPE_v,				/* Suffix */
     rvv_arg_type_info (RVV_BASE_void), /* Return type */
     scalar_ptr_uint64_index_args /* Args */};

/* A static operand information for vector_type func (vector_type, vector_type)
 * function registration. */
static CONSTEXPR const rvv_op_info iu_vvv_ops
  = {iu_ops,				/* Types */
     OP_TYPE_vv,			/* Suffix */
     rvv_arg_type_info (RVV_BASE_vector), /* Return type */
     vv_args /* Args */};

/* A static operand information for vector_type func (vector_type, shift_type)
 * function registration. */
static CONSTEXPR const rvv_op_info iu_shift_vvv_ops
  = {iu_ops,				/* Types */
     OP_TYPE_vv,			/* Suffix */
     rvv_arg_type_info (RVV_BASE_vector), /* Return type */
     shift_vv_args /* Args */};

/* A list of all RVV intrinsic functions.  */
static function_group_info function_groups[] = {
#define DEF_RVV_FUNCTION(NAME, SHAPE, PREDS, OPS_INFO)                         \
  {#NAME, &bases::NAME, &shapes::SHAPE, PREDS, OPS_INFO},
#include "riscv-vector-builtins-functions.def"
};

/* The RVV types, with their built-in
   "__rvv..._t" name.  Allow an index of NUM_VECTOR_TYPES, which always
   yields a null tree.  */
static GTY (()) tree abi_vector_types[NUM_VECTOR_TYPES + 1];

/* Same, but with the riscv_vector.h "v..._t" name.  */
extern GTY (()) rvv_builtin_types_t builtin_types[NUM_VECTOR_TYPES + 1];
rvv_builtin_types_t builtin_types[NUM_VECTOR_TYPES + 1];

/* The list of all registered function decls, indexed by code.  */
static GTY (()) vec<registered_function *, va_gc> *registered_functions;

/* All registered function decls, hashed on the function_instance
   that they implement.  This is used for looking up implementations of
   overloaded functions.  */
static hash_table<registered_function_hasher> *function_table;

/* RAII class for enabling enough RVV features to define the built-in
   types and implement the riscv_vector.h pragma.

   Note: According to 'TYPE_MODE' macro implementation, we need set
   have_regs_of_mode[mode] to be true if we want to get the exact mode
   from 'TYPE_MODE'. However, have_regs_of_mode has not been set yet in
   targetm.init_builtins (). We need rvv_switcher to set have_regs_of_mode
   before targetm.init_builtins () and recover back have_regs_of_mode
   after targetm.init_builtins ().  */
class rvv_switcher
{
public:
  rvv_switcher ();
  ~rvv_switcher ();

private:
  bool m_old_have_regs_of_mode[MAX_MACHINE_MODE];
};

rvv_switcher::rvv_switcher ()
{
  /* Set have_regs_of_mode before targetm.init_builtins ().  */
  memcpy (m_old_have_regs_of_mode, have_regs_of_mode,
	  sizeof (have_regs_of_mode));
  for (int i = 0; i < NUM_MACHINE_MODES; ++i)
    if (riscv_v_ext_vector_mode_p ((machine_mode) i))
      have_regs_of_mode[i] = true;
}

rvv_switcher::~rvv_switcher ()
{
  /* Recover back have_regs_of_mode.  */
  memcpy (have_regs_of_mode, m_old_have_regs_of_mode,
	  sizeof (have_regs_of_mode));
}

/* Add attribute NAME to ATTRS.  */
static tree
add_attribute (const char *name, tree attrs)
{
  return tree_cons (get_identifier (name), NULL_TREE, attrs);
}

/* Add type attributes to builtin type tree, currently only the mangled name. */
static void
add_vector_type_attribute (tree type, const char *mangled_name)
{
  tree mangled_name_tree = get_identifier (mangled_name);
  tree value = tree_cons (NULL_TREE, mangled_name_tree, NULL_TREE);
  TYPE_ATTRIBUTES (type)
    = tree_cons (get_identifier ("RVV type"), value, TYPE_ATTRIBUTES (type));
}

/* Force TYPE to be a sizeless type.  */
static void
make_type_sizeless (tree type)
{
  TYPE_ATTRIBUTES (type) = tree_cons (get_identifier ("RVV sizeless type"),
				      NULL_TREE, TYPE_ATTRIBUTES (type));
}

/* Return true if TYPE is a sizeless type.  */
static bool
sizeless_type_p (const_tree type)
{
  if (type == error_mark_node)
    return NULL_TREE;
  return lookup_attribute ("RVV sizeless type", TYPE_ATTRIBUTES (type));
}

/* If TYPE is an ABI-defined RVV type, return its attribute descriptor,
   otherwise return null.  */
static tree
lookup_vector_type_attribute (const_tree type)
{
  if (type == error_mark_node)
    return NULL_TREE;
  return lookup_attribute ("RVV type", TYPE_ATTRIBUTES (type));
}

/* Return a representation of "const T *".  */
static tree
build_const_pointer (tree t)
{
  return build_pointer_type (build_qualified_type (t, TYPE_QUAL_CONST));
}

/* Helper function for register a single built-in RVV ABI type.  */
static void
register_builtin_type (vector_type_index type, tree eltype, machine_mode mode)
{
  builtin_types[type].scalar = eltype;
  builtin_types[type].scalar_ptr = build_pointer_type (eltype);
  builtin_types[type].scalar_const_ptr = build_const_pointer (eltype);
  if (!riscv_v_ext_vector_mode_p (mode))
    return;

  tree vectype = build_vector_type_for_mode (eltype, mode);
  gcc_assert (VECTOR_MODE_P (TYPE_MODE (vectype)) && TYPE_MODE (vectype) == mode
	      && TYPE_MODE_RAW (vectype) == mode && TYPE_ALIGN (vectype) <= 128
	      && known_eq (tree_to_poly_uint64 (TYPE_SIZE (vectype)),
			   GET_MODE_BITSIZE (mode)));
  vectype = build_distinct_type_copy (vectype);
  gcc_assert (vectype == TYPE_MAIN_VARIANT (vectype));
  SET_TYPE_STRUCTURAL_EQUALITY (vectype);
  TYPE_ARTIFICIAL (vectype) = 1;
  TYPE_INDIVISIBLE_P (vectype) = 1;
  add_vector_type_attribute (vectype, vector_types[type].mangled_name);
  make_type_sizeless (vectype);
  abi_vector_types[type] = vectype;
  lang_hooks.types.register_builtin_type (vectype, vector_types[type].abi_name);
}

/* Register the built-in RVV ABI types, such as __rvv_int32m1_t.  */
static void
register_builtin_types ()
{
  /* Get type node from get_typenode_from_name to prevent we have different type
     node define in different target libraries, e.g. int32_t defined as
     `long` in RV32/newlib-stdint, but `int` for RV32/glibc-stdint.h.
     NOTE: uint[16|32|64]_type_node already defined in tree.h.  */
  tree int8_type_node = get_typenode_from_name (INT8_TYPE);
  tree uint8_type_node = get_typenode_from_name (UINT8_TYPE);
  tree int16_type_node = get_typenode_from_name (INT16_TYPE);
  tree int32_type_node = get_typenode_from_name (INT32_TYPE);
  tree int64_type_node = get_typenode_from_name (INT64_TYPE);

  machine_mode mode;
#define DEF_RVV_TYPE(NAME, NCHARS, ABI_NAME, SCALAR_TYPE, VECTOR_MODE,         \
		     VECTOR_MODE_MIN_VLEN_32, ARGS...)                         \
  mode = TARGET_MIN_VLEN > 32 ? VECTOR_MODE##mode                              \
			      : VECTOR_MODE_MIN_VLEN_32##mode;                 \
  register_builtin_type (VECTOR_TYPE_##NAME, SCALAR_TYPE##_type_node, mode);
#include "riscv-vector-builtins.def"
}

/* Register vector type TYPE under its risv_vector.h name.  */
static void
register_vector_type (vector_type_index type)
{
  tree vectype = abi_vector_types[type];

  /* When vectype is NULL, the corresponding builtin type
     is disabled according to '-march'.  */
  if (!vectype)
    return;
  tree id = get_identifier (vector_types[type].name);
  tree decl = build_decl (input_location, TYPE_DECL, id, vectype);
  decl = lang_hooks.decls.pushdecl (decl);

  /* Record the new RVV type if pushdecl succeeded without error.  Use
     the ABI type otherwise, so that the type we record at least has the
     right form, even if it doesn't have the right name.  This should give
     better error recovery behavior than installing error_mark_node or
     installing an incorrect type.  */
  if (decl && TREE_CODE (decl) == TYPE_DECL
      && TREE_TYPE (decl) != error_mark_node
      && TYPE_MAIN_VARIANT (TREE_TYPE (decl)) == vectype)
    vectype = TREE_TYPE (decl);

  builtin_types[type].vector = vectype;
  builtin_types[type].vector_ptr = build_pointer_type (vectype);
}

/* Return true if the type has required_extensions.  */
static bool
required_extensions_p (enum rvv_base_type type)
{
  return type == RVV_BASE_vector || type == RVV_BASE_uint8_index
	 || type == RVV_BASE_uint16_index || type == RVV_BASE_uint32_index
	 || type == RVV_BASE_uint64_index;
}

/* Check whether all the RVV_REQUIRE_* values in REQUIRED_EXTENSIONS are
   enabled.  */
static bool
check_required_extensions (const function_instance &instance)
{
  rvv_type_info type_info = instance.type;
  uint64_t required_extensions = type_info.required_extensions;
  const rvv_op_info *op_info = instance.op_info;
  tree type = builtin_types[type_info.index].vector;
  for (unsigned i = 0; op_info->args[i].base_type != NUM_BASE_TYPES; ++i)
    {
      if (!required_extensions_p (op_info->args[i].base_type))
	continue;

      enum vector_type_index vector_type
	= op_info->args[i].get_base_vector_type (type);
      if (vector_type == NUM_VECTOR_TYPES)
	continue;
      required_extensions |= op_info->types[vector_type].required_extensions;

      /* According to RVV ISA, EEW=64 index of indexed loads/stores require
	 XLEN = 64.  */
      if (op_info->args[i].base_type == RVV_BASE_uint64_index)
	required_extensions |= RVV_REQUIRE_RV64BIT;
    }

  uint64_t riscv_isa_flags = 0;

  if (TARGET_VECTOR_ELEN_FP_32)
    riscv_isa_flags |= RVV_REQUIRE_ELEN_FP_32;
  if (TARGET_VECTOR_ELEN_FP_64)
    riscv_isa_flags |= RVV_REQUIRE_ELEN_FP_64;
  if (TARGET_MIN_VLEN > 32)
    riscv_isa_flags |= RVV_REQUIRE_ZVE64;
  if (TARGET_64BIT)
    riscv_isa_flags |= RVV_REQUIRE_RV64BIT;

  uint64_t missing_extensions = required_extensions & ~riscv_isa_flags;
  if (missing_extensions != 0)
    return false;
  return true;
}

/* Return true if predication is using a real mask operand.  */
static bool
use_real_mask_p (enum predication_type_index pred)
{
  return pred == PRED_TYPE_m || pred == PRED_TYPE_tum || pred == PRED_TYPE_tumu
	 || pred == PRED_TYPE_mu;
}

/* Return true if predication is using a real merge operand.  */
static bool
use_real_merge_p (enum predication_type_index pred)
{
  return pred == PRED_TYPE_tu || pred == PRED_TYPE_tum || pred == PRED_TYPE_tumu
	 || pred == PRED_TYPE_mu;
}

/* Get TAIL policy for predication. If predication indicates TU, return the TU.
   Otherwise, return the prefer default configuration.  */
static rtx
get_tail_policy_for_pred (enum predication_type_index pred)
{
  if (pred == PRED_TYPE_tu || pred == PRED_TYPE_tum || pred == PRED_TYPE_tumu)
    return gen_int_mode (TAIL_UNDISTURBED, Pmode);
  return gen_int_mode (get_prefer_tail_policy (), Pmode);
}

/* Get MASK policy for predication. If predication indicates MU, return the MU.
   Otherwise, return the prefer default configuration.  */
static rtx
get_mask_policy_for_pred (enum predication_type_index pred)
{
  if (pred == PRED_TYPE_tumu || pred == PRED_TYPE_mu)
    return gen_int_mode (MASK_UNDISTURBED, Pmode);
  return gen_int_mode (get_prefer_mask_policy (), Pmode);
}

vector_type_index
rvv_arg_type_info::get_base_vector_type (tree type) const
{
  if (!type)
    return NUM_VECTOR_TYPES;
  poly_int64 nunits = GET_MODE_NUNITS (TYPE_MODE (type));
  machine_mode inner_mode;
  bool unsigned_p = TYPE_UNSIGNED (type);
  switch (base_type)
    {
    case RVV_BASE_uint8_index:
      inner_mode = E_QImode;
      unsigned_p = true;
      break;
    case RVV_BASE_uint16_index:
      inner_mode = E_HImode;
      unsigned_p = true;
      break;
    case RVV_BASE_uint32_index:
      inner_mode = E_SImode;
      unsigned_p = true;
      break;
    case RVV_BASE_uint64_index:
      inner_mode = E_DImode;
      unsigned_p = true;
      break;
    case RVV_BASE_shift_vector:
      inner_mode = GET_MODE_INNER (TYPE_MODE (type));
      unsigned_p = true;
      break;
    default:
      return NUM_VECTOR_TYPES;
    }

  opt_machine_mode mode
    = get_vector_mode (as_a<scalar_mode> (inner_mode), nunits);

  if (!mode.exists ())
    return NUM_VECTOR_TYPES;
  for (unsigned int i = 0; i < NUM_VECTOR_TYPES + 1; i++)
    {
      tree vector_type = builtin_types[i].vector;
      if (!vector_type)
	continue;

      if (TYPE_UNSIGNED (vector_type) != unsigned_p)
	continue;

      if (TYPE_MODE (vector_type) == mode.require ())
	return (enum vector_type_index) i;
    }
  return NUM_VECTOR_TYPES;
}

tree
rvv_arg_type_info::get_tree_type (vector_type_index type_idx) const
{
  switch (base_type)
    {
    case RVV_BASE_vector:
      return builtin_types[type_idx].vector;
    case RVV_BASE_scalar:
      return builtin_types[type_idx].scalar;
    case RVV_BASE_vector_ptr:
      return builtin_types[type_idx].vector_ptr;
    case RVV_BASE_scalar_ptr:
      /* According to the latest rvv-intrinsic-doc, it defines vsm.v intrinsic:
	 __riscv_vsm (uint8_t *base, vbool1_t value, size_t vl).  */
      if (type_idx >= VECTOR_TYPE_vbool64_t && type_idx <= VECTOR_TYPE_vbool1_t)
	return builtin_types[VECTOR_TYPE_vuint8mf8_t].scalar_ptr;
      else
	return builtin_types[type_idx].scalar_ptr;
    case RVV_BASE_scalar_const_ptr:
      /* According to the latest rvv-intrinsic-doc, it defines vlm.v intrinsic:
	 __riscv_vlm_v_b1 (const uint8_t *base, size_t vl).  */
      if (type_idx >= VECTOR_TYPE_vbool64_t && type_idx <= VECTOR_TYPE_vbool1_t)
	return builtin_types[VECTOR_TYPE_vuint8mf8_t].scalar_const_ptr;
      else
	return builtin_types[type_idx].scalar_const_ptr;
    case RVV_BASE_void:
      return void_type_node;
    case RVV_BASE_size:
      return size_type_node;
    case RVV_BASE_ptrdiff:
      return ptrdiff_type_node;
    case RVV_BASE_unsigned_long:
      return long_unsigned_type_node;
    case RVV_BASE_long:
      return long_integer_type_node;
    case RVV_BASE_uint8_index:
    case RVV_BASE_uint16_index:
    case RVV_BASE_uint32_index:
    case RVV_BASE_uint64_index:
    case RVV_BASE_shift_vector:
      if (get_base_vector_type (builtin_types[type_idx].vector)
	  != NUM_VECTOR_TYPES)
	return builtin_types[get_base_vector_type (
			       builtin_types[type_idx].vector)].vector;
      break;
    default:
      gcc_unreachable ();
    }
  /* Return NULL_TREE if the type we don't want to register.  */
  return NULL_TREE;
}

function_instance::function_instance (const char *base_name_in,
				      const function_base *base_in,
				      const function_shape *shape_in,
				      rvv_type_info type_in,
				      predication_type_index pred_in,
				      const rvv_op_info *op_info_in)
  : base_name (base_name_in), base (base_in), shape (shape_in), type (type_in),
    pred (pred_in), op_info (op_info_in)
{
}

bool
function_instance::operator== (const function_instance &other) const
{
  for (unsigned int i = 0; op_info->args[i].base_type != NUM_BASE_TYPES; ++i)
    if (op_info->args[i].base_type != other.op_info->args[i].base_type)
      return false;
  return (base == other.base && shape == other.shape
	  && type.index == other.type.index && op_info->op == other.op_info->op
	  && pred == other.pred
	  && op_info->ret.base_type == other.op_info->ret.base_type);
}

bool
function_instance::any_type_float_p () const
{
  if (FLOAT_MODE_P (TYPE_MODE (get_return_type ())))
    return true;

  for (int i = 0; op_info->args[i].base_type != NUM_BASE_TYPES; ++i)
    if (FLOAT_MODE_P (TYPE_MODE (get_arg_type (i))))
      return true;

  return false;
}

tree
function_instance::get_return_type () const
{
  return op_info->ret.get_tree_type (type.index);
}

tree
function_instance::get_arg_type (unsigned opno) const
{
  return op_info->args[opno].get_tree_type (type.index);
}

/* Return a hash code for a function_instance.  */
hashval_t
function_instance::hash () const
{
  inchash::hash h;
  /* BASE uniquely determines BASE_NAME, so we don't need to hash both.  */
  h.add_ptr (base);
  h.add_ptr (shape);
  h.add_int (type.index);
  h.add_int (op_info->op);
  h.add_int (pred);
  h.add_int (op_info->ret.base_type);
  for (unsigned int i = 0; op_info->args[i].base_type != NUM_BASE_TYPES; ++i)
    h.add_int (op_info->args[i].base_type);
  return h.end ();
}

/* Return a set of CP_* flags that describe what the function could do,
   taking the command-line flags into account.  */
unsigned int
function_instance::call_properties () const
{
  unsigned int flags = base->call_properties (*this);

  /* -fno-trapping-math means that we can assume any FP exceptions
     are not user-visible.  */
  if (!flag_trapping_math)
    flags &= ~CP_RAISE_FP_EXCEPTIONS;

  return flags;
}

/* Return true if calls to the function could read some form of
   global state.  */
bool
function_instance::reads_global_state_p () const
{
  unsigned int flags = call_properties ();

  /* Preserve any dependence on rounding mode, flush to zero mode, etc.
     There is currently no way of turning this off; in particular,
     -fno-rounding-math (which is the default) means that we should make
     the usual assumptions about rounding mode, which for intrinsics means
     acting as the instructions do.  */
  if (flags & CP_READ_FPCR)
    return true;

  /* Handle direct reads of global state.  */
  return flags & (CP_READ_MEMORY | CP_READ_CSR);
}

/* Return true if calls to the function could modify some form of
   global state.  */
bool
function_instance::modifies_global_state_p () const
{
  unsigned int flags = call_properties ();

  /* Preserve any exception state written back to the FPCR,
     unless -fno-trapping-math says this is unnecessary.  */
  if (flags & CP_RAISE_FP_EXCEPTIONS)
    return true;

  /* Handle direct modifications of global state.  */
  return flags & (CP_WRITE_MEMORY | CP_WRITE_CSR);
}

/* Return true if calls to the function could raise a signal.  */
bool
function_instance::could_trap_p () const
{
  unsigned int flags = call_properties ();

  /* Handle functions that could raise SIGFPE.  */
  if (flags & CP_RAISE_FP_EXCEPTIONS)
    return true;

  /* Handle functions that could raise SIGBUS or SIGSEGV.  */
  if (flags & (CP_READ_MEMORY | CP_WRITE_MEMORY))
    return true;

  return false;
}

function_builder::function_builder ()
{
  m_direct_overloads = lang_GNU_CXX ();
  gcc_obstack_init (&m_string_obstack);
}

function_builder::~function_builder ()
{
  obstack_free (&m_string_obstack, NULL);
}

/* Allocate arguments of the function.  */
void
function_builder::allocate_argument_types (const function_instance &instance,
					   vec<tree> &argument_types) const
{
  for (unsigned int i = 0;
       instance.op_info->args[i].base_type != NUM_BASE_TYPES; ++i)
    argument_types.quick_push (
      instance.op_info->args[i].get_tree_type (instance.type.index));
}

/* Apply predication into argument_types.  */
void
function_builder::apply_predication (const function_instance &instance,
				     tree return_type,
				     vec<tree> &argument_types) const
{
  /* These predication types need to apply merge type.  */
  if (instance.pred == PRED_TYPE_tu || instance.pred == PRED_TYPE_tum
      || instance.pred == PRED_TYPE_tumu || instance.pred == PRED_TYPE_mu)
    argument_types.quick_insert (0, return_type);

  /* These predication types need to apply mask type.  */
  tree mask_type = builtin_types[mask_types[instance.type.index]].vector;
  if (instance.pred == PRED_TYPE_m || instance.pred == PRED_TYPE_tum
      || instance.pred == PRED_TYPE_tumu || instance.pred == PRED_TYPE_mu)
    argument_types.quick_insert (0, mask_type);

  /* check if vl parameter need  */
  if (instance.base->apply_vl_p ())
    argument_types.quick_push (size_type_node);
}

/* Register all the functions in GROUP.  */
void
function_builder::register_function_group (const function_group_info &group)
{
  (*group.shape)->build (*this, group);
}

/* Add NAME to the end of the function name being built.  */
void
function_builder::append_name (const char *name)
{
  obstack_grow (&m_string_obstack, name, strlen (name));
}

/* Add "__riscv_" and "name".  */
void
function_builder::append_base_name (const char *name)
{
  append_name ("__riscv_");
  append_name (name);
}

/* Add SEW into function name.  */
void
function_builder::append_sew (int sew)
{
  switch (sew)
    {
    case 8:
      append_name ("8");
      break;
    case 16:
      append_name ("16");
      break;
    case 32:
      append_name ("32");
      break;
    case 64:
      append_name ("64");
      break;
    default:
      gcc_unreachable ();
    }
}

/* Zero-terminate and complete the function name being built.  */
char *
function_builder::finish_name ()
{
  obstack_1grow (&m_string_obstack, 0);
  return (char *) obstack_finish (&m_string_obstack);
}

/* Return the appropriate function attributes for INSTANCE.  */
tree
function_builder::get_attributes (const function_instance &instance)
{
  tree attrs = NULL_TREE;

  if (!instance.modifies_global_state_p ())
    {
      if (instance.reads_global_state_p ())
	attrs = add_attribute ("pure", attrs);
      else
	attrs = add_attribute ("const", attrs);
    }

  if (!flag_non_call_exceptions || !instance.could_trap_p ())
    attrs = add_attribute ("nothrow", attrs);

  return add_attribute ("leaf", attrs);
}

/* Add a function called NAME with type FNTYPE and attributes ATTRS.
   INSTANCE describes what the function does.  */
registered_function &
function_builder::add_function (const function_instance &instance,
				const char *name, tree fntype, tree attrs,
				bool placeholder_p)
{
  unsigned int code = vec_safe_length (registered_functions);
  code = (code << RISCV_BUILTIN_SHIFT) + RISCV_BUILTIN_VECTOR;

  /* We need to be able to generate placeholders to enusre that we have a
     consistent numbering scheme for function codes between the C and C++
     frontends, so that everything ties up in LTO.

     Currently, tree-streamer-in.c:unpack_ts_function_decl_value_fields
     validates that tree nodes returned by TARGET_BUILTIN_DECL are non-NULL and
     some node other than error_mark_node. This is a holdover from when builtin
     decls were streamed by code rather than by value.

     Ultimately, we should be able to remove this validation of BUILT_IN_MD
     nodes and remove the target hook. For now, however, we need to appease the
     validation and return a non-NULL, non-error_mark_node node, so we
     arbitrarily choose integer_zero_node.  */
  tree decl = placeholder_p
		? integer_zero_node
		: simulate_builtin_function_decl (input_location, name, fntype,
						  code, NULL, attrs);

  registered_function &rfn = *ggc_alloc<registered_function> ();
  rfn.instance = instance;
  rfn.decl = decl;
  vec_safe_push (registered_functions, &rfn);

  return rfn;
}

/* Add a built-in function for INSTANCE, with the argument types given
   by ARGUMENT_TYPES and the return type given by RETURN_TYPE. NAME is
   the "full" name for C function. OVERLOAD_NAME is the "short" name for
   C++ overloaded function. OVERLOAD_NAME can be nullptr because some
   instance doesn't have C++ overloaded function.  */
void
function_builder::add_unique_function (const function_instance &instance,
				       const function_shape *shape,
				       tree return_type,
				       vec<tree> &argument_types)
{
  /* Do not add this function if it is invalid.  */
  if (!check_required_extensions (instance))
    return;

  /* Add the function under its full (unique) name.  */
  char *name = shape->get_name (*this, instance, false);
  tree fntype
    = build_function_type_array (return_type, argument_types.length (),
				 argument_types.address ());
  tree attrs = get_attributes (instance);
  registered_function &rfn
    = add_function (instance, name, fntype, attrs, false);

  /* Enter the function into the hash table.  */
  hashval_t hash = instance.hash ();
  registered_function **rfn_slot
    = function_table->find_slot_with_hash (instance, hash, INSERT);
  gcc_assert (!*rfn_slot);
  *rfn_slot = &rfn;

  /* Also add the function under its overloaded alias, if we want
     a separate decl for each instance of an overloaded function.  */
  char *overload_name = shape->get_name (*this, instance, true);
  if (overload_name)
    {
      /* Attribute lists shouldn't be shared.  */
      tree attrs = get_attributes (instance);
      bool placeholder_p = !m_direct_overloads;
      add_function (instance, overload_name, fntype, attrs, placeholder_p);
    }
  obstack_free (&m_string_obstack, name);
}

function_call_info::function_call_info (location_t location_in,
					const function_instance &instance_in,
					tree fndecl_in)
  : function_instance (instance_in), location (location_in), fndecl (fndecl_in)
{}

function_expander::function_expander (const function_instance &instance,
				      tree fndecl_in, tree exp_in,
				      rtx target_in)
  : function_call_info (EXPR_LOCATION (exp_in), instance, fndecl_in),
    exp (exp_in), target (target_in), opno (0)
{
  if (!function_returns_void_p ())
    create_output_operand (&m_ops[opno++], target, TYPE_MODE (TREE_TYPE (exp)));
}

/* Take argument ARGNO from EXP's argument list and convert it into
   an expand operand.  Store the operand in *M_OPS.  */
void
function_expander::add_input_operand (unsigned argno)
{
  tree arg = CALL_EXPR_ARG (exp, argno);
  rtx x = expand_normal (arg);
  add_input_operand (TYPE_MODE (TREE_TYPE (arg)), x);
}

/* Since we may normalize vop/vop_tu/vop_m/vop_tumu.. into a single patter.
   We add a undef for the intrinsics that don't need a real merge.  */
void
function_expander::add_vundef_operand (machine_mode mode)
{
  rtx vundef = gen_rtx_UNSPEC (mode, gen_rtvec (1, const0_rtx), UNSPEC_VUNDEF);
  add_input_operand (mode, vundef);
}

/* Add a memory operand with mode MODE and address ADDR.  */
void
function_expander::add_mem_operand (machine_mode mode, unsigned argno)
{
  gcc_assert (VECTOR_MODE_P (mode));
  rtx addr = expand_normal (CALL_EXPR_ARG (exp, argno));
  rtx mem = gen_rtx_MEM (mode, memory_address (mode, addr));
  /* The memory is only guaranteed to be element-aligned.  */
  set_mem_align (mem, GET_MODE_ALIGNMENT (GET_MODE_INNER (mode)));
  add_fixed_operand (mem);
}

/* Implement the call using instruction ICODE, with a 1:1 mapping between
   arguments and input operands.  */
rtx
function_expander::use_exact_insn (insn_code icode)
{
  machine_mode mode = TYPE_MODE (TREE_TYPE (exp));
  tree mask_type = builtin_types[mask_types[type.index]].vector;
  machine_mode mask_mode = TYPE_MODE (mask_type);

  /* Record the offset to get the argument.  */
  int arg_offset = 0;

  if (use_real_mask_p (pred))
    add_input_operand (arg_offset++);
  else
    add_all_one_mask_operand (mask_mode);

  /* Store operation doesn't have merge operand.  */
  if (!function_returns_void_p ())
    {
      if (use_real_merge_p (pred))
	add_input_operand (arg_offset++);
      else
	add_vundef_operand (mode);
    }

  for (int argno = arg_offset; argno < call_expr_nargs (exp); argno++)
    add_input_operand (argno);

  if (base->apply_tail_policy_p ())
    add_input_operand (Pmode, get_tail_policy_for_pred (pred));
  if (base->apply_mask_policy_p ())
    add_input_operand (Pmode, get_mask_policy_for_pred (pred));

  add_input_operand (Pmode, get_avl_type_rtx (avl_type::NONVLMAX));
  return generate_insn (icode);
}

/* Use contiguous load INSN.  */
rtx
function_expander::use_contiguous_load_insn (insn_code icode)
{
  gcc_assert (call_expr_nargs (exp) > 0);
  machine_mode mode = TYPE_MODE (TREE_TYPE (exp));
  tree mask_type = builtin_types[mask_types[type.index]].vector;
  machine_mode mask_mode = TYPE_MODE (mask_type);

  /* Record the offset to get the argument.  */
  int arg_offset = 0;

  if (use_real_mask_p (pred))
    add_input_operand (arg_offset++);
  else
    add_all_one_mask_operand (mask_mode);

  if (use_real_merge_p (pred))
    add_input_operand (arg_offset++);
  else
    add_vundef_operand (mode);

  add_mem_operand (mode, arg_offset++);

  for (int argno = arg_offset; argno < call_expr_nargs (exp); argno++)
    add_input_operand (argno);

  if (GET_MODE_CLASS (mode) != MODE_VECTOR_BOOL)
    {
      add_input_operand (Pmode, get_tail_policy_for_pred (pred));
      add_input_operand (Pmode, get_mask_policy_for_pred (pred));
    }

  if (opno != insn_data[icode].n_generator_args)
    add_input_operand (Pmode, get_avl_type_rtx (avl_type::NONVLMAX));

  return generate_insn (icode);
}

/* Use contiguous store INSN.  */
rtx
function_expander::use_contiguous_store_insn (insn_code icode)
{
  gcc_assert (call_expr_nargs (exp) > 0);
  machine_mode mode = TYPE_MODE (builtin_types[type.index].vector);
  tree mask_type = builtin_types[mask_types[type.index]].vector;
  machine_mode mask_mode = TYPE_MODE (mask_type);

  /* Record the offset to get the argument.  */
  int arg_offset = 0;

  add_mem_operand (mode, use_real_mask_p (pred) ? 1 : 0);

  if (use_real_mask_p (pred))
    add_input_operand (arg_offset++);
  else
    add_all_one_mask_operand (mask_mode);

  arg_offset++;
  for (int argno = arg_offset; argno < call_expr_nargs (exp); argno++)
    add_input_operand (argno);

  return generate_insn (icode);
}

/* Generate instruction ICODE, given that its operands have already
   been added to M_OPS.  Return the value of the first operand.  */
rtx
function_expander::generate_insn (insn_code icode)
{
  gcc_assert (opno == insn_data[icode].n_generator_args);
  if (!maybe_expand_insn (icode, opno, m_ops))
    {
      error ("invalid argument to built-in function");
      return NULL_RTX;
    }
  return function_returns_void_p () ? const0_rtx : m_ops[0].value;
}

inline hashval_t
registered_function_hasher::hash (value_type value)
{
  return value->instance.hash ();
}

inline bool
registered_function_hasher::equal (value_type value, const compare_type &key)
{
  return value->instance == key;
}

/* If TYPE is a built-in type defined by the RVV ABI, return the mangled name,
   otherwise return NULL.  */
const char *
mangle_builtin_type (const_tree type)
{
  if (TYPE_NAME (type) && TREE_CODE (TYPE_NAME (type)) == TYPE_DECL)
    type = TREE_TYPE (TYPE_NAME (type));
  if (tree attr = lookup_vector_type_attribute (type))
    if (tree id = TREE_VALUE (chain_index (0, TREE_VALUE (attr))))
      return IDENTIFIER_POINTER (id);
  return NULL;
}

/* Initialize all compiler built-ins related to RVV that should be
   defined at start-up.  */
void
init_builtins ()
{
  rvv_switcher rvv;
  if (!TARGET_VECTOR)
    return;
  register_builtin_types ();
  if (in_lto_p)
    handle_pragma_vector ();
}

/* Implement TARGET_VERIFY_TYPE_CONTEXT for RVV types.  */
bool
verify_type_context (location_t loc, type_context_kind context, const_tree type,
		     bool silent_p)
{
  if (!sizeless_type_p (type))
    return true;

  switch (context)
    {
    case TCTX_SIZEOF:
    case TCTX_STATIC_STORAGE:
      if (!silent_p)
	error_at (loc, "RVV type %qT does not have a fixed size", type);

      return false;

    case TCTX_ALIGNOF:
      if (!silent_p)
	error_at (loc, "RVV type %qT does not have a defined alignment", type);

      return false;

    case TCTX_THREAD_STORAGE:
      if (!silent_p)
	error_at (loc,
		  "variables of type %qT cannot have thread-local"
		  " storage duration",
		  type);

      return false;

    case TCTX_POINTER_ARITH:
      if (!silent_p)
	error_at (loc, "arithmetic on pointer to RVV type %qT", type);

      return false;

    case TCTX_FIELD:
      if (silent_p)
	;
      else if (lang_GNU_CXX ())
	error_at (loc, "member variables cannot have RVV type %qT", type);
      else
	error_at (loc, "fields cannot have RVV type %qT", type);

      return false;

    case TCTX_ARRAY_ELEMENT:
      if (!silent_p)
	error_at (loc, "array elements cannot have RVV type %qT", type);

      return false;

    case TCTX_ALLOCATION:
      if (!silent_p)
	error_at (loc, "cannot allocate objects with RVV type %qT", type);

      return false;

    case TCTX_DEALLOCATION:
      if (!silent_p)
	error_at (loc, "cannot delete objects with RVV type %qT", type);

      return false;

    case TCTX_EXCEPTIONS:
      if (!silent_p)
	error_at (loc, "cannot throw or catch RVV type %qT", type);

      return false;

    case TCTX_CAPTURE_BY_COPY:
      if (!silent_p)
	error_at (loc, "capture by copy of RVV type %qT", type);

      return false;
    }

  gcc_unreachable ();
}

/* Implement #pragma riscv intrinsic vector.  */
void
handle_pragma_vector ()
{
  if (function_table)
    {
      error ("duplicate definition of %qs", "riscv_vector.h");
      return;
    }
  rvv_switcher rvv;

  /* Define the vector and tuple types.  */
  for (unsigned int type_i = 0; type_i < NUM_VECTOR_TYPES; ++type_i)
    register_vector_type ((enum vector_type_index) type_i);

  /* Define the functions.  */
  function_table = new hash_table<registered_function_hasher> (1023);
  function_builder builder;
  for (unsigned int i = 0; i < ARRAY_SIZE (function_groups); ++i)
    builder.register_function_group (function_groups[i]);
}

/* Return the function decl with RVV function subcode CODE, or error_mark_node
   if no such function exists.  */
tree
builtin_decl (unsigned int code, bool)
{
  if (code >= vec_safe_length (registered_functions))
    return error_mark_node;

  return (*registered_functions)[code]->decl;
}

/* Expand a call to the RVV function with subcode CODE.  EXP is the call
   expression and TARGET is the preferred location for the result.
   Return the value of the lhs.  */
rtx
expand_builtin (unsigned int code, tree exp, rtx target)
{
  registered_function &rfn = *(*registered_functions)[code];
  return function_expander (rfn.instance, rfn.decl, exp, target).expand ();
}

} // end namespace riscv_vector

inline void
gt_ggc_mx (function_instance *)
{}

inline void
gt_pch_nx (function_instance *)
{}

inline void
gt_pch_nx (function_instance *, gt_pointer_operator, void *)
{}

#include "gt-riscv-vector-builtins.h"
