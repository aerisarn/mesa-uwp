/*
 * Copyright 2023 Valve Corporation
 * Copyright 2023 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "compiler/nir/nir_builder.h"

static bool
get_atomic_op(const nir_intrinsic_instr *atomic, nir_intrinsic_op *out_intr,
              nir_atomic_op *out_op)
{
#define CASE(prefix, suffix, atom, op, swap)                                   \
   case nir_intrinsic_##prefix##_atomic_##atom##suffix:                        \
      *out_op = nir_atomic_op_##op;                                            \
      *out_intr = swap ? nir_intrinsic_##prefix##_atomic_swap##suffix :        \
                         nir_intrinsic_##prefix##_atomic##suffix;              \
      return true;

#define CASES_NO_IMAGE(atom, op, swap)                                         \
   CASE(deref,, atom, op, swap)                                                \
   CASE(ssbo,, atom, op, swap)                                                 \
   CASE(shared,, atom, op, swap)                                               \
   CASE(task_payload,, atom, op, swap)                                         \
   CASE(global,, atom, op, swap)                                               \
   CASE(global, _2x32, atom, op, swap)                                         \
   CASE(global, _amd, atom, op, swap)

#define CASES_IMAGE(atom, op, swap)                                            \
   CASE(image,, atom, op, swap)                                                \
   CASE(image_deref,, atom, op, swap)                                          \
   CASE(bindless_image,, atom, op, swap)

#define CASES(atom, op, swap)                                                  \
   CASES_NO_IMAGE(atom, op, swap)                                              \
   CASES_IMAGE(atom, op, swap)                                                 \

   switch (atomic->intrinsic) {
      CASES(add, iadd, false);
      CASES(imin, imin, false);
      CASES(umin, umin, false);
      CASES(imax, imax, false);
      CASES(umax, umax, false);
      CASES(and, iand, false);
      CASES(or, ior, false);
      CASES(xor, ixor, false);
      CASES(exchange, xchg, false);
      CASES(fadd, fadd, false);
      CASES(fmin, fmin, false);
      CASES(fmax, fmax, false);
      CASES(comp_swap, cmpxchg, true);
      CASES_NO_IMAGE(fcomp_swap, fcmpxchg, true);
      CASES_IMAGE(inc_wrap, inc_wrap, false);
      CASES_IMAGE(dec_wrap, dec_wrap, false);
   default:
      return false;
   }

#undef CASE
#undef CASES_NO_IMAGE
#undef CASES
}

static bool
pass(nir_builder *b, nir_instr *instr, UNUSED void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   nir_intrinsic_op intr_op;
   nir_atomic_op op;
   if (!get_atomic_op(intr, &intr_op, &op))
      return false;

   intr->intrinsic = intr_op;
   nir_intrinsic_set_atomic_op(intr, op);
   return false;
}

bool
nir_lower_legacy_atomics(nir_shader *shader)
{
   return nir_shader_instructions_pass(
      shader, pass, nir_metadata_block_index | nir_metadata_dominance, NULL);
}
