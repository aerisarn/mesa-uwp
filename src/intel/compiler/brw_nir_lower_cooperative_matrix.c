/*
 * Copyright 2023 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

/**
 * \file brw_nir_lower_cooperative_matrix.c
 * Lower cooperative matrix to subgroup operations.
 */

#include "brw_nir.h"

struct lower_cmat_state {
   nir_shader *shader;

   struct hash_table *slice_coop_types;

   struct hash_table *vars_to_slice;

   unsigned subgroup_size;
};

static void
print_coop_types(struct lower_cmat_state *state)
{
   fprintf(stderr, "--- Slices to Cooperative Matrix type table\n");
   hash_table_foreach(state->slice_coop_types, e) {
      nir_variable *var = (void *)e->key;
      const struct glsl_type *t = e->data;
      fprintf(stderr, "%p: %s -> %s\n", var, var->name, glsl_get_type_name(t));
   }
   fprintf(stderr, "\n\n");
}

static const struct glsl_type *
get_coop_type_for_slice(struct lower_cmat_state *state, nir_deref_instr *deref)
{
   nir_variable *var = nir_deref_instr_get_variable(deref);
   struct hash_entry *entry = _mesa_hash_table_search(state->slice_coop_types, var);

   assert(entry != NULL);

   return entry->data;
}

static bool
lower_cmat_filter(const nir_instr *instr, const void *_state)
{
   if (instr->type == nir_instr_type_deref) {
      nir_deref_instr *deref = nir_instr_as_deref(instr);
      return glsl_type_is_cmat(deref->type);
   }

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   switch (intrin->intrinsic) {
   case nir_intrinsic_cmat_construct:
   case nir_intrinsic_cmat_load:
   case nir_intrinsic_cmat_store:
   case nir_intrinsic_cmat_length:
   case nir_intrinsic_cmat_muladd:
   case nir_intrinsic_cmat_unary_op:
   case nir_intrinsic_cmat_binary_op:
   case nir_intrinsic_cmat_scalar_op:
   case nir_intrinsic_cmat_bitcast:
   case nir_intrinsic_cmat_insert:
   case nir_intrinsic_cmat_extract:
   case nir_intrinsic_cmat_copy:
      return true;

   default:
      return false;
   }
}

/**
 * Get number of matrix elements packed in each component of the slice.
 */
static unsigned
get_packing_factor(const struct glsl_cmat_description desc,
                   const struct glsl_type *slice_type)
{
   const struct glsl_type *slice_element_type = glsl_without_array(slice_type);

   assert(!glsl_type_is_cmat(slice_type));

   assert(glsl_get_bit_size(slice_element_type) >= glsl_base_type_get_bit_size(desc.element_type));
   assert(glsl_get_bit_size(slice_element_type) % glsl_base_type_get_bit_size(desc.element_type) == 0);

   return glsl_get_bit_size(slice_element_type) / glsl_base_type_get_bit_size(desc.element_type);
}

static const struct glsl_type *
get_slice_type_from_desc(const struct lower_cmat_state *state,
                         const struct glsl_cmat_description desc)
{
   enum glsl_base_type base_type;

   /* Number of matrix elements stored by each subgroup invocation. If the
    * data is packed, the slice size will be less than this.
    */
   const unsigned elements_per_invocation =
      (desc.rows * desc.cols) / state->subgroup_size;

   assert(elements_per_invocation > 0);

   const unsigned element_bits = 32;
   const unsigned bits = glsl_base_type_get_bit_size(desc.element_type);
   unsigned packing_factor = MIN2(elements_per_invocation,
                                  element_bits / bits);

   /* Adjust the packing factor so that each row of the matrix fills and
    * entire GRF.
    */
   const unsigned actual_cols = desc.use != GLSL_CMAT_USE_B ? desc.cols : desc.rows;
   while ((actual_cols / packing_factor) < 8) {
      assert(packing_factor > 1);
      packing_factor /= 2;
   }

   switch (desc.element_type) {
   case GLSL_TYPE_FLOAT:
      base_type = GLSL_TYPE_FLOAT;
      break;
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_FLOAT16:
   case GLSL_TYPE_UINT8:
   case GLSL_TYPE_UINT16:
      base_type = glsl_get_base_type(glsl_uintN_t_type(packing_factor * bits));
      break;
   case GLSL_TYPE_INT:
   case GLSL_TYPE_INT8:
   case GLSL_TYPE_INT16:
      base_type = glsl_get_base_type(glsl_intN_t_type(packing_factor * bits));
      break;
   default:
      unreachable("Invalid cooperative matrix element type.");
   }

   unsigned len = elements_per_invocation / packing_factor;

   /* Supported matrix sizes are designed to fill either 4 or 8 SIMD8
    * registers. That means:
    *
    *          4 regsiters   8 registers
    * SIMD32     len = 1       len = 2
    * SIMD16     len = 2       len = 4
    * SIMD8      len = 4       len = 8
    *
    * If configurations are added that result in other values of len, at the
    * very least this assertion will need to be updated. The only value of len
    * that makes sense to add would be 16, and that would be a lot of
    * registers.
    */
   assert(len == 1 || len == 2 || len == 4 || len == 8);

   const struct glsl_type *slice_type = glsl_vector_type(base_type, len);

   assert(packing_factor == get_packing_factor(desc, slice_type));

   return slice_type;
}

static const struct glsl_type *
get_slice_type(const struct lower_cmat_state *state,
               const struct glsl_type *type)
{
   if (glsl_type_is_array(type)) {
      const struct glsl_type *slice_type =
         get_slice_type(state, glsl_get_array_element(type));

      return glsl_array_type(slice_type, glsl_array_size(type), 0);
   }

   assert(glsl_type_is_cmat(type));

   return get_slice_type_from_desc(state,
                                   *glsl_get_cmat_description(type));
}

static nir_deref_instr *
create_local_slice(struct lower_cmat_state *state, nir_builder *b,
                   const struct glsl_type *mat_type, const char *name)
{
   const struct glsl_type *slice_type = get_slice_type(state, mat_type);
   nir_variable *slice_var = nir_local_variable_create(b->impl, slice_type, name);
   _mesa_hash_table_insert(state->slice_coop_types, slice_var, (void *)mat_type);
   return nir_build_deref_var(b, slice_var);
}

static void
lower_cmat_load_store(nir_builder *b, nir_intrinsic_instr *intrin,
                      struct lower_cmat_state *state)
{
   const bool load = intrin->intrinsic == nir_intrinsic_cmat_load;
   const unsigned mat_src = load ? 0 : 1;
   const unsigned ptr_src = load ? 1 : 0;

   /* TODO: Column major. */
   assert(nir_intrinsic_matrix_layout(intrin) == GLSL_MATRIX_LAYOUT_ROW_MAJOR);

   nir_deref_instr *slice = nir_src_as_deref(intrin->src[mat_src]);
   const struct glsl_type *mat_type = get_coop_type_for_slice(state, slice);
   const struct glsl_cmat_description *desc = glsl_get_cmat_description(mat_type);

   /* TODO: Dynamic stride. */
   assert(nir_src_is_const(intrin->src[2]));

   nir_def *results[NIR_MAX_VEC_COMPONENTS];
   const unsigned num_components = glsl_get_vector_elements(slice->type);

   nir_deref_instr *pointer = nir_src_as_deref(intrin->src[ptr_src]);

   const unsigned stride = nir_src_as_uint(intrin->src[2]);

   const struct glsl_type *element_type =
      glsl_get_array_element(slice->type);

   const struct glsl_type *pointer_type =
      glsl_array_type(element_type, MAX2(desc->rows, desc->cols) * stride,
                      glsl_get_bit_size(element_type) / 8);

   pointer = nir_build_deref_cast(b, &pointer->def, pointer->modes, pointer_type,
                                  glsl_get_bit_size(element_type) / 8);

   for (unsigned i = 0; i < num_components; i++) {

      nir_def *offset = nir_imul_imm(b, nir_load_subgroup_invocation(b),
                                         stride);
      nir_deref_instr *memory_deref =
         nir_build_deref_array(b, pointer,
                               nir_i2iN(b, nir_iadd_imm(b, offset, i),
                                        pointer->def.bit_size));

      if (load) {
         results[i] = nir_load_deref(b, memory_deref);
      } else {
         nir_def *src = nir_channel(b, nir_load_deref(b, slice), i);
         nir_store_deref(b, memory_deref, src, 0x1);
      }
   }

   if (load)
      nir_store_deref(b, slice, nir_vec(b, results, num_components),
                      nir_component_mask(num_components));
}

static void
lower_cmat_unary_op(nir_builder *b, nir_intrinsic_instr *intrin,
                    struct lower_cmat_state *state)
{
   nir_deref_instr *dst_slice = nir_src_as_deref(intrin->src[0]);
   nir_deref_instr *src_slice = nir_src_as_deref(intrin->src[1]);
   nir_def *results[NIR_MAX_VEC_COMPONENTS];
   const unsigned num_components = glsl_get_vector_elements(dst_slice->type);

   const struct glsl_type *dst_mat_type =
      get_coop_type_for_slice(state, dst_slice);
   const struct glsl_type *src_mat_type =
      get_coop_type_for_slice(state, src_slice);

   const struct glsl_cmat_description dst_desc =
      *glsl_get_cmat_description(dst_mat_type);

   const struct glsl_cmat_description src_desc =
      *glsl_get_cmat_description(src_mat_type);

   const unsigned dst_bits = glsl_base_type_bit_size(dst_desc.element_type);
   const unsigned src_bits = glsl_base_type_bit_size(src_desc.element_type);

   /* The type of the returned slice may be different from the type of the
    * input slice.
    */
   const unsigned dst_packing_factor =
      get_packing_factor(dst_desc, dst_slice->type);

   const unsigned src_packing_factor =
      get_packing_factor(src_desc, src_slice->type);

   const nir_op op = nir_intrinsic_alu_op(intrin);

   /* There are three possible cases:
    *
    * 1. dst_packing_factor == src_packing_factor. This is the common case,
    *    and handling it is straightforward.
    *
    * 2. dst_packing_factor > src_packing_factor. This occurs when converting a
    *    float32_t matrix slice to a packed float16_t slice. Loop over the size
    *    of the destination slice, but read multiple entries from the source
    *    slice on each iteration.
    *
    * 3. dst_packing_factor < src_packing_factor. This occurs when converting a
    *    packed int8_t matrix slice to an int32_t slice. Loop over the size of
    *    the source slice, but write multiple entries to the destination slice
    *    on each iteration.
    *
    * Handle all cases by iterating over the total (non-packed) number of
    * elements in the slice. When dst_packing_factor values have been
    * calculated, store them.
    */
   assert((dst_packing_factor * glsl_get_vector_elements(dst_slice->type)) ==
          (src_packing_factor * glsl_get_vector_elements(src_slice->type)));

   /* Stores at most dst_packing_factor partial results. */
   nir_def *v[4];
   assert(dst_packing_factor <= 4);

   for (unsigned i = 0; i < num_components * dst_packing_factor; i++) {
      const unsigned dst_chan_index = i % dst_packing_factor;
      const unsigned src_chan_index = i % src_packing_factor;
      const unsigned dst_index = i / dst_packing_factor;
      const unsigned src_index = i / src_packing_factor;

      nir_def *src =
         nir_channel(b,
                     nir_unpack_bits(b,
                                     nir_channel(b,
                                                 nir_load_deref(b, src_slice),
                                                 src_index),
                                     src_bits),
                     src_chan_index);

      v[dst_chan_index] = nir_build_alu1(b, op, src);

      if (dst_chan_index == (dst_packing_factor - 1)) {
         results[dst_index] =
            nir_pack_bits(b, nir_vec(b, v, dst_packing_factor),
                          dst_packing_factor * dst_bits);
      }
   }

   nir_store_deref(b, dst_slice, nir_vec(b, results, num_components),
                   nir_component_mask(num_components));
}

static void
lower_cmat_binary_op(nir_builder *b, nir_intrinsic_instr *intrin,
                     struct lower_cmat_state *state)
{
   nir_deref_instr *dst_slice = nir_src_as_deref(intrin->src[0]);
   nir_deref_instr *src_a_slice = nir_src_as_deref(intrin->src[1]);
   nir_deref_instr *src_b_slice = nir_src_as_deref(intrin->src[2]);

   nir_def *src_a = nir_load_deref(b, src_a_slice);
   nir_def *src_b = nir_load_deref(b, src_b_slice);
   nir_def *results[NIR_MAX_VEC_COMPONENTS];
   const unsigned num_components = glsl_get_vector_elements(dst_slice->type);

   const struct glsl_type *dst_mat_type = get_coop_type_for_slice(state, dst_slice);
   ASSERTED const struct glsl_type *src_a_mat_type = get_coop_type_for_slice(state, src_a_slice);
   ASSERTED const struct glsl_type *src_b_mat_type = get_coop_type_for_slice(state, src_b_slice);

   const struct glsl_cmat_description desc =
      *glsl_get_cmat_description(dst_mat_type);

   assert(dst_mat_type == src_a_mat_type);
   assert(dst_mat_type == src_b_mat_type);

   const unsigned bits = glsl_base_type_bit_size(desc.element_type);
   const unsigned packing_factor = get_packing_factor(desc, dst_slice->type);

   for (unsigned i = 0; i < num_components; i++) {
      nir_def *val_a = nir_channel(b, src_a, i);
      nir_def *val_b = nir_channel(b, src_b, i);

      results[i] =
         nir_pack_bits(b, nir_build_alu2(b, nir_intrinsic_alu_op(intrin),
                                         nir_unpack_bits(b, val_a, bits),
                                         nir_unpack_bits(b, val_b, bits)),
                       packing_factor * bits);
   }

   nir_store_deref(b, dst_slice, nir_vec(b, results, num_components),
                   nir_component_mask(num_components));
}

static void
lower_cmat_scalar_op(nir_builder *b, nir_intrinsic_instr *intrin,
                     struct lower_cmat_state *state)
{
   nir_deref_instr *dst_slice = nir_src_as_deref(intrin->src[0]);
   nir_deref_instr *src_slice = nir_src_as_deref(intrin->src[1]);
   nir_def *scalar = intrin->src[2].ssa;

   nir_def *src = nir_load_deref(b, src_slice);
   nir_def *results[NIR_MAX_VEC_COMPONENTS];
   const unsigned num_components = glsl_get_vector_elements(dst_slice->type);

   ASSERTED const struct glsl_type *dst_mat_type = get_coop_type_for_slice(state, dst_slice);
   ASSERTED const struct glsl_type *src_mat_type = get_coop_type_for_slice(state, src_slice);
   assert(dst_mat_type == src_mat_type);

   for (unsigned i = 0; i < num_components; i++) {
      nir_def *val = nir_channel(b, src, i);

      results[i] = nir_build_alu2(b, nir_intrinsic_alu_op(intrin), val, scalar);
   }

   nir_store_deref(b, dst_slice, nir_vec(b, results, num_components),
                   nir_component_mask(num_components));
}

static nir_deref_instr *
lower_cmat_deref(nir_builder *b, nir_deref_instr *deref,
                 struct lower_cmat_state *state)
{
   nir_deref_instr *parent = nir_deref_instr_parent(deref);
   if (parent) {
      assert(deref->deref_type == nir_deref_type_array);
      parent = lower_cmat_deref(b, parent, state);
      return nir_build_deref_array(b, parent, deref->arr.index.ssa);
   } else {
      assert(deref->deref_type == nir_deref_type_var);
      assert(deref->var);
      assert(glsl_type_is_cmat(glsl_without_array(deref->var->type)));

      struct hash_entry *entry = _mesa_hash_table_search(state->vars_to_slice, deref->var);
      assert(entry);
      return nir_build_deref_var(b, (nir_variable *)entry->data);
   }
}

static nir_def *
lower_cmat_instr(nir_builder *b, nir_instr *instr, void *_state)
{
   struct lower_cmat_state *state = _state;

   if (instr->type == nir_instr_type_deref) {
      nir_deref_instr *deref = lower_cmat_deref(b, nir_instr_as_deref(instr), state);
      return &deref->def;
   }

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   switch (intrin->intrinsic) {
   case nir_intrinsic_cmat_load:
   case nir_intrinsic_cmat_store:
      lower_cmat_load_store(b, intrin, state);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;

   case nir_intrinsic_cmat_construct: {
      nir_deref_instr *slice = nir_src_as_deref(intrin->src[0]);
      nir_def *src = intrin->src[1].ssa;

      const struct glsl_type *mat_type = get_coop_type_for_slice(state, slice);
      const struct glsl_cmat_description desc =
         *glsl_get_cmat_description(mat_type);
      const unsigned packing_factor = get_packing_factor(desc, slice->type);

      if (packing_factor > 1) {
         src = nir_pack_bits(b, nir_replicate(b, src, packing_factor),
                             packing_factor * glsl_base_type_get_bit_size(desc.element_type));
      }

      const unsigned num_components = glsl_get_vector_elements(slice->type);

      nir_store_deref(b, slice, nir_replicate(b, src, num_components),
                      nir_component_mask(num_components));
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   case nir_intrinsic_cmat_unary_op:
      lower_cmat_unary_op(b, intrin, state);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;

   case nir_intrinsic_cmat_binary_op:
      lower_cmat_binary_op(b, intrin, state);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;

   case nir_intrinsic_cmat_scalar_op:
      lower_cmat_scalar_op(b, intrin, state);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;

   case nir_intrinsic_cmat_length: {
      const struct glsl_cmat_description desc = nir_intrinsic_cmat_desc(intrin);
      const struct glsl_type *mat_type = glsl_cmat_type(&desc);
      const struct glsl_type *slice_type = get_slice_type(state, mat_type);
      return nir_imm_intN_t(b, (get_packing_factor(desc, slice_type) *
                                glsl_get_vector_elements(slice_type)), 32);
   }

   case nir_intrinsic_cmat_muladd:
      /* FINISHME. */
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;

   case nir_intrinsic_cmat_bitcast:
      /* FINISHME. */
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;

   case nir_intrinsic_cmat_copy:
      nir_copy_deref(b,
                     nir_src_as_deref(intrin->src[0]),
                     nir_src_as_deref(intrin->src[1]));
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;

   case nir_intrinsic_cmat_insert: {
      nir_deref_instr *dst_slice = nir_src_as_deref(intrin->src[0]);
      nir_def *scalar = intrin->src[1].ssa;
      nir_deref_instr *src_slice = nir_src_as_deref(intrin->src[2]);
      nir_def *dst_index = intrin->src[3].ssa;

      const struct glsl_type *dst_mat_type = get_coop_type_for_slice(state, dst_slice);
      ASSERTED const struct glsl_type *src_mat_type = get_coop_type_for_slice(state, src_slice);
      assert(dst_mat_type == src_mat_type);

      const struct glsl_cmat_description desc =
         *glsl_get_cmat_description(dst_mat_type);

      const unsigned bits = glsl_base_type_bit_size(desc.element_type);
      const unsigned packing_factor = get_packing_factor(desc, dst_slice->type);
      const unsigned num_components = glsl_get_vector_elements(dst_slice->type);

      nir_def *slice_index = nir_udiv_imm(b, dst_index, packing_factor);
      nir_def *vector_index = nir_umod_imm(b, dst_index, packing_factor);
      nir_def *results[NIR_MAX_VEC_COMPONENTS];

      for (unsigned i = 0; i < num_components; i++) {
         nir_def *val = nir_channel(b, nir_load_deref(b, src_slice), i);
         nir_def *insert;

         if (packing_factor == 1) {
            insert = scalar;
         } else {
            nir_def *unpacked = nir_unpack_bits(b, val, bits);
            nir_def *v = nir_vector_insert(b, unpacked, scalar, vector_index);

            insert = nir_pack_bits(b, v, bits * packing_factor);
         }

         results[i] = nir_bcsel(b, nir_ieq_imm(b, slice_index, i), insert, val);
      }

      nir_store_deref(b, dst_slice, nir_vec(b, results, num_components),
                      nir_component_mask(num_components));

      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   case nir_intrinsic_cmat_extract: {
      nir_deref_instr *slice = nir_src_as_deref(intrin->src[0]);
      const struct glsl_type *mat_type = get_coop_type_for_slice(state, slice);
      nir_def *index = intrin->src[1].ssa;

      const struct glsl_cmat_description desc =
         *glsl_get_cmat_description(mat_type);

      const unsigned bits = glsl_base_type_bit_size(desc.element_type);
      const unsigned packing_factor = get_packing_factor(desc, slice->type);

      nir_def *src =
         nir_vector_extract(b, nir_load_deref(b, slice),
                            nir_udiv_imm(b, index, packing_factor));

      if (packing_factor == 1) {
         return src;
      } else {
         return nir_vector_extract(b,
                                   nir_unpack_bits(b, src, bits),
                                   nir_umod_imm(b, index, packing_factor));
      }

      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   default:
      unreachable("invalid cooperative matrix intrinsic");
   }
}

static void
create_slice_var(struct lower_cmat_state *state, nir_variable *var,
                 nir_function_impl *impl)
{
   // TODO: without array
   const struct glsl_type *mat_type = glsl_without_array(var->type);

   assert(glsl_type_is_cmat(mat_type));
   assert((!impl && var->data.mode == nir_var_shader_temp) ||
          ( impl && var->data.mode == nir_var_function_temp));

   const struct glsl_type *slice_type = get_slice_type(state, var->type);
   const char *slice_name = ralloc_asprintf(state->shader, "%s_slice", var->name);
   nir_variable *slice_var = impl ?
      nir_local_variable_create(impl, slice_type, slice_name) :
      nir_variable_create(state->shader, var->data.mode, slice_type, slice_name);

   _mesa_hash_table_insert(state->vars_to_slice, var, slice_var);
   _mesa_hash_table_insert(state->slice_coop_types, slice_var, (void *)mat_type);
}

bool
brw_nir_lower_cmat(nir_shader *shader, unsigned subgroup_size)
{
   void *temp_ctx = ralloc_context(NULL);

   struct lower_cmat_state state = {
      .shader = shader,
      .slice_coop_types = _mesa_pointer_hash_table_create(temp_ctx),
      .vars_to_slice = _mesa_pointer_hash_table_create(temp_ctx),
      .subgroup_size = subgroup_size,
   };

   /* Create a slice array for each variable and add a map from the original
    * variable back to it, so it can be reached during lowering.
    *
    * TODO: Cooperative matrix inside struct?
    */
   nir_foreach_variable_in_shader(var, shader) {
      if (glsl_type_is_cmat(glsl_without_array(var->type)))
         create_slice_var(&state, var, NULL);
   }
   nir_foreach_function(func, shader) {
      nir_foreach_function_temp_variable(var, func->impl) {
         if (glsl_type_is_cmat(glsl_without_array(var->type)))
            create_slice_var(&state, var, func->impl);
      }
   }

   bool progress = nir_shader_lower_instructions(shader,
                                                 lower_cmat_filter,
                                                 lower_cmat_instr,
                                                 &state);

   ralloc_free(temp_ctx);

   return progress;
}
