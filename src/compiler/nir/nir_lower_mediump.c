/*
 * Copyright (C) 2020 Google, Inc.
 * Copyright (C) 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "nir.h"
#include "nir_builder.h"

/**
 * Return the intrinsic if it matches the mask in "modes", else return NULL.
 */
static nir_intrinsic_instr *
get_io_intrinsic(nir_instr *instr, nir_variable_mode modes,
                 nir_variable_mode *out_mode)
{
   if (instr->type != nir_instr_type_intrinsic)
      return NULL;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_input_vertex:
   case nir_intrinsic_load_interpolated_input:
   case nir_intrinsic_load_per_vertex_input:
      *out_mode = nir_var_shader_in;
      return modes & nir_var_shader_in ? intr : NULL;
   case nir_intrinsic_load_output:
   case nir_intrinsic_load_per_vertex_output:
   case nir_intrinsic_store_output:
   case nir_intrinsic_store_per_vertex_output:
      *out_mode = nir_var_shader_out;
      return modes & nir_var_shader_out ? intr : NULL;
   default:
      return NULL;
   }
}

/**
 * Recompute the IO "base" indices from scratch to remove holes or to fix
 * incorrect base values due to changes in IO locations by using IO locations
 * to assign new bases. The mapping from locations to bases becomes
 * monotonically increasing.
 */
bool
nir_recompute_io_bases(nir_shader *nir, nir_variable_mode modes)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   BITSET_DECLARE(inputs, NUM_TOTAL_VARYING_SLOTS);
   BITSET_DECLARE(outputs, NUM_TOTAL_VARYING_SLOTS);
   BITSET_ZERO(inputs);
   BITSET_ZERO(outputs);

   /* Gather the bitmasks of used locations. */
   nir_foreach_block_safe (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         nir_variable_mode mode;
         nir_intrinsic_instr *intr = get_io_intrinsic(instr, modes, &mode);
         if (!intr)
            continue;

         nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
         unsigned num_slots = sem.num_slots;
         if (sem.medium_precision)
            num_slots = (num_slots + sem.high_16bits + 1) / 2;

         if (mode == nir_var_shader_in) {
            for (unsigned i = 0; i < num_slots; i++)
               BITSET_SET(inputs, sem.location + i);
         } else if (!sem.dual_source_blend_index) {
            for (unsigned i = 0; i < num_slots; i++)
               BITSET_SET(outputs, sem.location + i);
         }
      }
   }

   /* Renumber bases. */
   bool changed = false;

   nir_foreach_block_safe (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         nir_variable_mode mode;
         nir_intrinsic_instr *intr = get_io_intrinsic(instr, modes, &mode);
         if (!intr)
            continue;

         nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
         unsigned num_slots = sem.num_slots;
         if (sem.medium_precision)
            num_slots = (num_slots + sem.high_16bits + 1) / 2;

         if (mode == nir_var_shader_in) {
            nir_intrinsic_set_base(intr,
                                   BITSET_PREFIX_SUM(inputs, sem.location));
         } else if (sem.dual_source_blend_index) {
            nir_intrinsic_set_base(intr,
                                   BITSET_PREFIX_SUM(outputs, NUM_TOTAL_VARYING_SLOTS));
         } else {
            nir_intrinsic_set_base(intr,
                                   BITSET_PREFIX_SUM(outputs, sem.location));
         }
         changed = true;
      }
   }

   if (changed) {
      nir_metadata_preserve(impl, nir_metadata_dominance |
                                  nir_metadata_block_index);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   return changed;
}

/**
 * Lower mediump inputs and/or outputs to 16 bits.
 *
 * \param modes            Whether to lower inputs, outputs, or both.
 * \param varying_mask     Determines which varyings to skip (VS inputs,
 *    FS outputs, and patch varyings ignore this mask).
 * \param use_16bit_slots  Remap lowered slots to* VARYING_SLOT_VARn_16BIT.
 */
bool
nir_lower_mediump_io(nir_shader *nir, nir_variable_mode modes,
                     uint64_t varying_mask, bool use_16bit_slots)
{
   bool changed = false;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   assert(impl);

   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block_safe (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         nir_variable_mode mode;
         nir_intrinsic_instr *intr = get_io_intrinsic(instr, modes, &mode);
         if (!intr)
            continue;

         nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
         nir_ssa_def *(*convert)(nir_builder *, nir_ssa_def *);
         bool is_varying = !(nir->info.stage == MESA_SHADER_VERTEX &&
                             mode == nir_var_shader_in) &&
                           !(nir->info.stage == MESA_SHADER_FRAGMENT &&
                             mode == nir_var_shader_out);

         if (!sem.medium_precision ||
             (is_varying && sem.location <= VARYING_SLOT_VAR31 &&
              !(varying_mask & BITFIELD64_BIT(sem.location))))
            continue; /* can't lower */

         if (nir_intrinsic_has_src_type(intr)) {
            /* Stores. */
            nir_alu_type type = nir_intrinsic_src_type(intr);

            switch (type) {
            case nir_type_float32:
               convert = nir_f2fmp;
               break;
            case nir_type_int32:
            case nir_type_uint32:
               convert = nir_i2imp;
               break;
            default:
               continue; /* already lowered? */
            }

            /* Convert the 32-bit store into a 16-bit store. */
            b.cursor = nir_before_instr(&intr->instr);
            nir_instr_rewrite_src_ssa(&intr->instr, &intr->src[0],
                                      convert(&b, intr->src[0].ssa));
            nir_intrinsic_set_src_type(intr, (type & ~32) | 16);
         } else {
            /* Loads. */
            nir_alu_type type = nir_intrinsic_dest_type(intr);

            switch (type) {
            case nir_type_float32:
               convert = nir_f2f32;
               break;
            case nir_type_int32:
               convert = nir_i2i32;
               break;
            case nir_type_uint32:
               convert = nir_u2u32;
               break;
            default:
               continue; /* already lowered? */
            }

            /* Convert the 32-bit load into a 16-bit load. */
            b.cursor = nir_after_instr(&intr->instr);
            intr->dest.ssa.bit_size = 16;
            nir_intrinsic_set_dest_type(intr, (type & ~32) | 16);
            nir_ssa_def *dst = convert(&b, &intr->dest.ssa);
            nir_ssa_def_rewrite_uses_after(&intr->dest.ssa, dst,
                                           dst->parent_instr);
         }

         if (use_16bit_slots && is_varying &&
             sem.location >= VARYING_SLOT_VAR0 &&
             sem.location <= VARYING_SLOT_VAR31) {
            unsigned index = sem.location - VARYING_SLOT_VAR0;

            sem.location = VARYING_SLOT_VAR0_16BIT + index / 2;
            sem.high_16bits = index % 2;
            nir_intrinsic_set_io_semantics(intr, sem);
         }
         changed = true;
      }
   }

   if (changed && use_16bit_slots)
      nir_recompute_io_bases(nir, modes);

   if (changed) {
      nir_metadata_preserve(impl, nir_metadata_dominance |
                                  nir_metadata_block_index);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   return changed;
}

/**
 * Set the mediump precision bit for those shader inputs and outputs that are
 * set in the "modes" mask. Non-generic varyings (that GLES3 doesn't have)
 * are ignored. The "types" mask can be (nir_type_float | nir_type_int), etc.
 */
bool
nir_force_mediump_io(nir_shader *nir, nir_variable_mode modes,
                     nir_alu_type types)
{
   bool changed = false;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   assert(impl);

   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block_safe (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         nir_variable_mode mode;
         nir_intrinsic_instr *intr = get_io_intrinsic(instr, modes, &mode);
         if (!intr)
            continue;

         nir_alu_type type;
         if (nir_intrinsic_has_src_type(intr))
            type = nir_intrinsic_src_type(intr);
         else
            type = nir_intrinsic_dest_type(intr);
         if (!(type & types))
            continue;

         nir_io_semantics sem = nir_intrinsic_io_semantics(intr);

         if (nir->info.stage == MESA_SHADER_FRAGMENT &&
             mode == nir_var_shader_out) {
            /* Only accept FS outputs. */
            if (sem.location < FRAG_RESULT_DATA0 &&
                sem.location != FRAG_RESULT_COLOR)
               continue;
         } else if (nir->info.stage == MESA_SHADER_VERTEX &&
                    mode == nir_var_shader_in) {
            /* Accept all VS inputs. */
         } else {
            /* Only accept generic varyings. */
            if (sem.location < VARYING_SLOT_VAR0 ||
                sem.location > VARYING_SLOT_VAR31)
            continue;
         }

         sem.medium_precision = 1;
         nir_intrinsic_set_io_semantics(intr, sem);
         changed = true;
      }
   }

   if (changed) {
      nir_metadata_preserve(impl, nir_metadata_dominance |
                                  nir_metadata_block_index);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   return changed;
}

/**
 * Remap 16-bit varying slots to the original 32-bit varying slots.
 * This only changes IO semantics and bases.
 */
bool
nir_unpack_16bit_varying_slots(nir_shader *nir, nir_variable_mode modes)
{
   bool changed = false;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   assert(impl);

   nir_foreach_block_safe (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         nir_variable_mode mode;
         nir_intrinsic_instr *intr = get_io_intrinsic(instr, modes, &mode);
         if (!intr)
            continue;

         nir_io_semantics sem = nir_intrinsic_io_semantics(intr);

         if (sem.location < VARYING_SLOT_VAR0_16BIT ||
             sem.location > VARYING_SLOT_VAR15_16BIT)
            continue;

         sem.location = VARYING_SLOT_VAR0 +
                        (sem.location - VARYING_SLOT_VAR0_16BIT) * 2 +
                        sem.high_16bits;
         sem.high_16bits = 0;
         nir_intrinsic_set_io_semantics(intr, sem);
         changed = true;
      }
   }

   if (changed)
      nir_recompute_io_bases(nir, modes);

   if (changed) {
      nir_metadata_preserve(impl, nir_metadata_dominance |
                                  nir_metadata_block_index);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   return changed;
}

static bool
is_n_to_m_conversion(nir_instr *instr, unsigned n, nir_op m)
{
   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(instr);
   return alu->op == m && alu->src[0].src.ssa->bit_size == n;
}

static bool
is_f16_to_f32_conversion(nir_instr *instr)
{
   return is_n_to_m_conversion(instr, 16, nir_op_f2f32);
}

static bool
is_f32_to_f16_conversion(nir_instr *instr)
{
   return is_n_to_m_conversion(instr, 32, nir_op_f2f16) ||
          is_n_to_m_conversion(instr, 32, nir_op_f2f16_rtne) ||
          is_n_to_m_conversion(instr, 32, nir_op_f2fmp);
}

static bool
is_i16_to_i32_conversion(nir_instr *instr)
{
   return is_n_to_m_conversion(instr, 16, nir_op_i2i32);
}

static bool
is_u16_to_u32_conversion(nir_instr *instr)
{
   return is_n_to_m_conversion(instr, 16, nir_op_u2u32);
}

static bool
is_i32_to_i16_conversion(nir_instr *instr)
{
   return is_n_to_m_conversion(instr, 32, nir_op_i2i16) ||
      is_n_to_m_conversion(instr, 32, nir_op_u2u16);
}

static void
replace_with_mov(nir_builder *b, nir_instr *instr, nir_src *src,
                 nir_alu_instr *alu)
{
   nir_ssa_def *mov = nir_mov_alu(b, alu->src[0],
                                  nir_dest_num_components(alu->dest.dest));
   assert(!alu->dest.saturate);
   nir_instr_rewrite_src_ssa(instr, src, mov);
}

/**
 * If texture source operands use f16->f32 conversions or return values are
 * followed by f16->f32 or f32->f16, remove those conversions. This benefits
 * drivers that have texture opcodes that can accept and return 16-bit types.
 *
 * "tex_src_types" is a mask of nir_tex_src_* operands that should be handled.
 * It's always done for the destination.
 *
 * This should be run after late algebraic optimizations.
 * Copy propagation and DCE should be run after this.
 */
bool
nir_fold_16bit_sampler_conversions(nir_shader *nir,
                                   unsigned tex_src_types,
                                   uint32_t sampler_dims)
{
   bool changed = false;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   assert(impl);

   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block_safe (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_tex)
            continue;

         nir_tex_instr *tex = nir_instr_as_tex(instr);
         nir_instr *src;
         nir_alu_instr *src_alu;

         /* Skip sparse residency */
         if (tex->is_sparse)
            continue;

         if ((tex->op == nir_texop_txs ||
              tex->op == nir_texop_query_levels) ||
             !(sampler_dims & BITFIELD_BIT(tex->sampler_dim)))
            continue;

         /* Optimize source operands. */
         for (unsigned i = 0; i < tex->num_srcs; i++) {
            /* Filter out sources that should be ignored. */
            if (!(BITFIELD_BIT(tex->src[i].src_type) & tex_src_types))
               continue;

            src = tex->src[i].src.ssa->parent_instr;
            if (src->type != nir_instr_type_alu)
               continue;

            src_alu = nir_instr_as_alu(src);
            b.cursor = nir_before_instr(src);

            nir_alu_type src_type = nir_tex_instr_src_type(tex, i);

            /* Handle vector sources that are made of scalar instructions. */
            if (nir_op_is_vec(src_alu->op)) {
               /* See if the vector is made of f16->f32 opcodes. */
               unsigned num = nir_dest_num_components(src_alu->dest.dest);
               bool is_f16_to_f32 = src_type == nir_type_float;
               bool is_u16_to_u32 = src_type & (nir_type_int | nir_type_uint);

               for (unsigned comp = 0; comp < num; comp++) {
                  nir_instr *instr = src_alu->src[comp].src.ssa->parent_instr;
                  is_f16_to_f32 &= is_f16_to_f32_conversion(instr);
                  /* Zero-extension (u16) and sign-extension (i16) have
                   * the same behavior here - txf returns 0 if bit 15 is set
                   * because it's out of bounds and the higher bits don't
                   * matter.
                   */
                  is_u16_to_u32 &= is_u16_to_u32_conversion(instr) ||
                                   is_i16_to_i32_conversion(instr);
               }

               if (!is_f16_to_f32 && !is_u16_to_u32)
                  continue;

               nir_alu_instr *new_vec = nir_alu_instr_clone(nir, src_alu);
               nir_instr_insert_after(&src_alu->instr, &new_vec->instr);

               /* Replace conversions with mov. */
               for (unsigned comp = 0; comp < num; comp++) {
                  nir_instr *instr = new_vec->src[comp].src.ssa->parent_instr;
                  replace_with_mov(&b, &new_vec->instr,
                                   &new_vec->src[comp].src,
                                   nir_instr_as_alu(instr));
               }

               new_vec->dest.dest.ssa.bit_size =
                  new_vec->src[0].src.ssa->bit_size;
               nir_instr_rewrite_src_ssa(&tex->instr, &tex->src[i].src,
                                         &new_vec->dest.dest.ssa);
               changed = true;
            } else if ((is_f16_to_f32_conversion(&src_alu->instr) &&
                        src_type == nir_type_float) ||
                       ((is_u16_to_u32_conversion(&src_alu->instr) ||
                         is_i16_to_i32_conversion(&src_alu->instr)) &&
                        src_type & (nir_type_int | nir_type_uint))) {
               /* Handle scalar sources. */
               replace_with_mov(&b, &tex->instr, &tex->src[i].src, src_alu);
               changed = true;
            }
         }

         /* Optimize the destination. */
         bool is_f32_to_f16 = tex->dest_type & nir_type_float;
         /* same behavior for int and uint */
         bool is_i32_to_i16 = tex->dest_type & (nir_type_int | nir_type_uint);

         nir_foreach_use(use, &tex->dest.ssa) {
            is_f32_to_f16 &= is_f32_to_f16_conversion(use->parent_instr);
            is_i32_to_i16 &= is_i32_to_i16_conversion(use->parent_instr);
         }

         if (is_f32_to_f16 || is_i32_to_i16) {
            /* All uses are the same conversions. Replace them with mov. */
            nir_foreach_use(use, &tex->dest.ssa) {
               nir_alu_instr *conv = nir_instr_as_alu(use->parent_instr);
               conv->op = nir_op_mov;
               tex->dest.ssa.bit_size = conv->dest.dest.ssa.bit_size;
               tex->dest_type = (tex->dest_type & (~16 & ~32 & ~64)) |
                                conv->dest.dest.ssa.bit_size;
            }
            changed = true;
         }
      }
   }

   if (changed) {
      nir_metadata_preserve(impl, nir_metadata_dominance |
                                  nir_metadata_block_index);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   return changed;
}

/**
 * Fix types of source operands of texture opcodes according to
 * the constraints by inserting the appropriate conversion opcodes.
 *
 * For example, if the type of derivatives must be equal to texture
 * coordinates and the type of the texture bias must be 32-bit, there
 * will be 2 constraints describing that.
 */
bool
nir_legalize_16bit_sampler_srcs(nir_shader *nir,
                                nir_tex_src_type_constraints constraints)
{
   bool changed = false;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   assert(impl);

   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block_safe (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_tex)
            continue;

         nir_tex_instr *tex = nir_instr_as_tex(instr);
         int8_t map[nir_num_tex_src_types];
         memset(map, -1, sizeof(map));

         /* Create a mapping from src_type to src[i]. */
         for (unsigned i = 0; i < tex->num_srcs; i++)
            map[tex->src[i].src_type] = i;

         /* Legalize src types. */
         for (unsigned i = 0; i < tex->num_srcs; i++) {
            nir_tex_src_type_constraint c = constraints[tex->src[i].src_type];

            if (!c.legalize_type)
               continue;

            /* Determine the required bit size for the src. */
            unsigned bit_size;
            if (c.bit_size) {
               bit_size = c.bit_size;
            } else {
               if (map[c.match_src] == -1)
                  continue; /* e.g. txs */

               bit_size = tex->src[map[c.match_src]].src.ssa->bit_size;
            }

            /* Check if the type is legal. */
            if (bit_size == tex->src[i].src.ssa->bit_size)
               continue;

            /* Fix the bit size. */
            bool is_sint = nir_tex_instr_src_type(tex, i) == nir_type_int;
            bool is_uint = nir_tex_instr_src_type(tex, i) == nir_type_uint;
            nir_ssa_def *(*convert)(nir_builder *, nir_ssa_def *);

            switch (bit_size) {
            case 16:
               convert = is_sint ? nir_i2i16 :
                         is_uint ? nir_u2u16 : nir_f2f16;
               break;
            case 32:
               convert = is_sint ? nir_i2i32 :
                         is_uint ? nir_u2u32 : nir_f2f32;
               break;
            default:
               assert(!"unexpected bit size");
               continue;
            }

            b.cursor = nir_before_instr(&tex->instr);
            nir_ssa_def *conv =
               convert(&b, nir_ssa_for_src(&b, tex->src[i].src,
                                           tex->src[i].src.ssa->num_components));
            nir_instr_rewrite_src_ssa(&tex->instr, &tex->src[i].src, conv);
            changed = true;
         }
      }
   }

   if (changed) {
      nir_metadata_preserve(impl, nir_metadata_dominance |
                                  nir_metadata_block_index);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   return changed;
}

static bool
const_is_f16(nir_ssa_scalar scalar)
{
   double value = nir_ssa_scalar_as_float(scalar);
   return value == _mesa_half_to_float(_mesa_float_to_half(value));
}

static bool
const_is_u16(nir_ssa_scalar scalar)
{
   uint64_t value = nir_ssa_scalar_as_uint(scalar);
   return value == (uint16_t) value;
}

static bool
const_is_i16(nir_ssa_scalar scalar)
{
   int64_t value = nir_ssa_scalar_as_int(scalar);
   return value == (int16_t) value;
}

static bool
fold_16bit_store_data(nir_builder *b, nir_intrinsic_instr *instr)
{
   nir_alu_type src_type = nir_intrinsic_src_type(instr);
   nir_src *data_src = &instr->src[3];

   b->cursor = nir_before_instr(&instr->instr);

   bool fold_f16 = src_type == nir_type_float32;
   bool fold_u16 = src_type == nir_type_uint32;
   bool fold_i16 = src_type == nir_type_int32;

   nir_ssa_scalar comps[NIR_MAX_VEC_COMPONENTS];
   for (unsigned i = 0; i < instr->num_components; i++) {
      comps[i] = nir_ssa_scalar_resolved(data_src->ssa, i);
      if (comps[i].def->parent_instr->type == nir_instr_type_ssa_undef)
         continue;
      else if (nir_ssa_scalar_is_const(comps[i])) {
         fold_f16 &= const_is_f16(comps[i]);
         fold_u16 &= const_is_u16(comps[i]);
         fold_i16 &= const_is_i16(comps[i]);
      } else {
         fold_f16 &= is_f16_to_f32_conversion(comps[i].def->parent_instr);
         fold_u16 &= is_u16_to_u32_conversion(comps[i].def->parent_instr);
         fold_i16 &= is_i16_to_i32_conversion(comps[i].def->parent_instr);
      }
   }

   if (!fold_f16 && !fold_u16 && !fold_i16)
      return false;

   nir_ssa_scalar new_comps[NIR_MAX_VEC_COMPONENTS];
   for (unsigned i = 0; i < instr->num_components; i++) {
      if (comps[i].def->parent_instr->type == nir_instr_type_ssa_undef)
         new_comps[i] = nir_get_ssa_scalar(nir_ssa_undef(b, 1, 16), 0);
      else if (nir_ssa_scalar_is_const(comps[i])) {
         nir_ssa_def *constant;
         if (src_type == nir_type_float32)
            constant = nir_imm_float16(b, nir_ssa_scalar_as_float(comps[i]));
         else
            constant = nir_imm_intN_t(b, nir_ssa_scalar_as_uint(comps[i]), 16);
         new_comps[i] = nir_get_ssa_scalar(constant, 0);
      } else {
         /* conversion instruction */
         new_comps[i] = nir_ssa_scalar_chase_alu_src(comps[i], 0);
      }
   }

   nir_ssa_def *new_vec = nir_vec_scalars(b, new_comps, instr->num_components);

   nir_instr_rewrite_src_ssa(&instr->instr, data_src, new_vec);

   nir_intrinsic_set_src_type(instr, (src_type & ~32) | 16);

   return true;
}

static bool
fold_16bit_load_data(nir_builder *b, nir_intrinsic_instr *instr)
{
   nir_alu_type dest_type = nir_intrinsic_dest_type(instr);

   if (dest_type == nir_type_float32 &&
       nir_has_any_rounding_mode_enabled(b->shader->info.float_controls_execution_mode))
      return false;

   bool is_f32_to_f16 = dest_type == nir_type_float32;
   bool is_i32_to_i16 = dest_type == nir_type_int32 || dest_type == nir_type_uint32;

   nir_foreach_use(use, &instr->dest.ssa) {
      is_f32_to_f16 &= is_f32_to_f16_conversion(use->parent_instr);
      is_i32_to_i16 &= is_i32_to_i16_conversion(use->parent_instr);
   }

   if (!is_f32_to_f16 && !is_i32_to_i16)
      return false;

   /* All uses are the same conversions. Replace them with mov. */
   nir_foreach_use(use, &instr->dest.ssa) {
      nir_alu_instr *conv = nir_instr_as_alu(use->parent_instr);
      conv->op = nir_op_mov;
   }

   instr->dest.ssa.bit_size = 16;
   nir_intrinsic_set_dest_type(instr, (dest_type & ~32) | 16);

   return true;
}

static bool
fold_16bit_image_load_store(nir_builder *b, nir_instr *instr, UNUSED void *unused)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instr);

   bool progress = false;

   switch (intrinsic->intrinsic) {
   case nir_intrinsic_bindless_image_store:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_store:
      progress |= fold_16bit_store_data(b, intrinsic);
      break;
   case nir_intrinsic_bindless_image_load:
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_load:
      progress |= fold_16bit_load_data(b, intrinsic);
      break;
   default:
      break;
   }

   return progress;
}

bool
nir_fold_16bit_image_load_store_conversions(nir_shader *nir)
{
   return nir_shader_instructions_pass(nir,
                                       fold_16bit_image_load_store,
                                       nir_metadata_block_index | nir_metadata_dominance,
                                       NULL);
}
