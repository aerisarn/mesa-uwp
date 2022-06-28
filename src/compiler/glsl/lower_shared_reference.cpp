/*
 * Copyright (c) 2015 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file lower_shared_reference.cpp
 *
 * IR lower pass to replace dereferences of compute shader shared variables
 * with intrinsic function calls.
 *
 * This relieves drivers of the responsibility of allocating space for the
 * shared variables in the shared memory region.
 */

#include "ir.h"
#include "ir_rvalue_visitor.h"
#include "ir_builder.h"
#include "linker.h"
#include "main/macros.h"
#include "util/list.h"
#include "main/consts_exts.h"
#include "main/shader_types.h"
#include "glsl_parser_extras.h"

using namespace ir_builder;

namespace {

struct var_offset {
   struct list_head node;
   const ir_variable *var;
   unsigned offset;
};

static inline int
writemask_for_size(unsigned n)
{
   return ((1 << n) - 1);
}

class lower_shared_reference_visitor : public ir_rvalue_enter_visitor  {
public:

   lower_shared_reference_visitor(struct gl_linked_shader *shader)
      : buffer_access_type(shared_load_access),
      list_ctx(ralloc_context(NULL)), shader(shader), shared_size(0u),
      progress(false)
   {
      list_inithead(&var_offsets);
   }

   ~lower_shared_reference_visitor()
   {
      ralloc_free(list_ctx);
   }

   enum {
      shared_load_access,
      shared_store_access,
      shared_atomic_access,
   } buffer_access_type;

   void emit_access(void *mem_ctx, bool is_write, ir_dereference *deref,
                    ir_variable *base_offset, unsigned int deref_offset,
                    bool row_major, const glsl_type *matrix_type,
                    enum glsl_interface_packing packing,
                    unsigned int write_mask);

   bool is_dereferenced_thing_row_major(const ir_rvalue *deref);

   void setup_buffer_access(void *mem_ctx, ir_rvalue *deref,
                            ir_rvalue **offset, unsigned *const_offset,
                            bool *row_major,
                            const glsl_type **matrix_type,
                            const glsl_struct_field **struct_field,
                            enum glsl_interface_packing packing);

   void insert_buffer_access(void *mem_ctx, ir_dereference *deref,
                             const glsl_type *type, ir_rvalue *offset,
                             unsigned mask, int channel);

   void handle_rvalue(ir_rvalue **rvalue);
   ir_visitor_status visit_enter(ir_assignment *ir);
   void handle_assignment(ir_assignment *ir);

   ir_call *lower_shared_atomic_intrinsic(ir_call *ir);
   ir_call *check_for_shared_atomic_intrinsic(ir_call *ir);
   ir_visitor_status visit_enter(ir_call *ir);

   unsigned get_shared_offset(const ir_variable *);

   ir_call *shared_load(void *mem_ctx, const struct glsl_type *type,
                        ir_rvalue *offset);
   ir_call *shared_store(void *mem_ctx, ir_rvalue *deref, ir_rvalue *offset,
                         unsigned write_mask);

   void *list_ctx;
   struct gl_linked_shader *shader;
   struct list_head var_offsets;
   unsigned shared_size;
   bool progress;
};

/**
 * Takes a deref and recursively calls itself to break the deref down to the
 * point that the reads or writes generated are contiguous scalars or vectors.
 */
void
lower_shared_reference_visitor::emit_access(void *mem_ctx,
                                            bool is_write,
                                            ir_dereference *deref,
                                            ir_variable *base_offset,
                                            unsigned int deref_offset,
                                            bool row_major,
                                            const glsl_type *matrix_type,
                                            enum glsl_interface_packing packing,
                                            unsigned int write_mask)
{
   if (deref->type->is_struct()) {
      unsigned int field_offset = 0;

      for (unsigned i = 0; i < deref->type->length; i++) {
         const struct glsl_struct_field *field =
            &deref->type->fields.structure[i];
         ir_dereference *field_deref =
            new(mem_ctx) ir_dereference_record(deref->clone(mem_ctx, NULL),
                                               field->name);

         unsigned field_align;
         if (packing == GLSL_INTERFACE_PACKING_STD430)
            field_align = field->type->std430_base_alignment(row_major);
         else
            field_align = field->type->std140_base_alignment(row_major);
         field_offset = glsl_align(field_offset, field_align);

         emit_access(mem_ctx, is_write, field_deref, base_offset,
                     deref_offset + field_offset,
                     row_major, NULL, packing,
                     writemask_for_size(field_deref->type->vector_elements));

         if (packing == GLSL_INTERFACE_PACKING_STD430)
            field_offset += field->type->std430_size(row_major);
         else
            field_offset += field->type->std140_size(row_major);
      }
      return;
   }

   if (deref->type->is_array()) {
      unsigned array_stride = packing == GLSL_INTERFACE_PACKING_STD430 ?
         deref->type->fields.array->std430_array_stride(row_major) :
         glsl_align(deref->type->fields.array->std140_size(row_major), 16);

      for (unsigned i = 0; i < deref->type->length; i++) {
         ir_constant *element = new(mem_ctx) ir_constant(i);
         ir_dereference *element_deref =
            new(mem_ctx) ir_dereference_array(deref->clone(mem_ctx, NULL),
                                              element);
         emit_access(mem_ctx, is_write, element_deref, base_offset,
                     deref_offset + i * array_stride,
                     row_major, NULL, packing,
                     writemask_for_size(element_deref->type->vector_elements));
      }
      return;
   }

   if (deref->type->is_matrix()) {
      for (unsigned i = 0; i < deref->type->matrix_columns; i++) {
         ir_constant *col = new(mem_ctx) ir_constant(i);
         ir_dereference *col_deref =
            new(mem_ctx) ir_dereference_array(deref->clone(mem_ctx, NULL), col);

         /* For a row-major matrix, the next column starts at the next
          * element.  Otherwise it is offset by the matrix stride.
          */
         const unsigned size_mul = row_major
            ? (deref->type->is_double() ? 8 : 4)
            : link_calculate_matrix_stride(deref->type, row_major, packing);

         emit_access(mem_ctx, is_write, col_deref, base_offset,
                     deref_offset + i * size_mul,
                     row_major, deref->type, packing,
                     writemask_for_size(col_deref->type->vector_elements));
      }
      return;
   }

   assert(deref->type->is_scalar() || deref->type->is_vector());

   if (!row_major) {
      ir_rvalue *offset =
         add(base_offset, new(mem_ctx) ir_constant(deref_offset));
      unsigned mask =
         is_write ? write_mask : (1 << deref->type->vector_elements) - 1;
      insert_buffer_access(mem_ctx, deref, deref->type, offset, mask, -1);
   } else {
      /* We're dereffing a column out of a row-major matrix, so we
       * gather the vector from each stored row.
       */
      assert(deref->type->is_float() || deref->type->is_double());
      assert(matrix_type != NULL);

      const unsigned matrix_stride =
         link_calculate_matrix_stride(matrix_type, row_major, packing);

      const glsl_type *deref_type = deref->type->get_scalar_type();

      for (unsigned i = 0; i < deref->type->vector_elements; i++) {
         ir_rvalue *chan_offset =
            add(base_offset,
                new(mem_ctx) ir_constant(deref_offset + i * matrix_stride));
         if (!is_write || ((1U << i) & write_mask))
            insert_buffer_access(mem_ctx, deref, deref_type, chan_offset,
                                 (1U << i), i);
      }
   }
}

/**
 * Determine if a thing being dereferenced is row-major
 *
 * There is some trickery here.
 *
 * If the thing being dereferenced is a member of uniform block \b without an
 * instance name, then the name of the \c ir_variable is the field name of an
 * interface type.  If this field is row-major, then the thing referenced is
 * row-major.
 *
 * If the thing being dereferenced is a member of uniform block \b with an
 * instance name, then the last dereference in the tree will be an
 * \c ir_dereference_record.  If that record field is row-major, then the
 * thing referenced is row-major.
 */
bool
lower_shared_reference_visitor::is_dereferenced_thing_row_major(const ir_rvalue *deref)
{
   bool matrix = false;
   const ir_rvalue *ir = deref;

   while (true) {
      matrix = matrix || ir->type->without_array()->is_matrix();

      switch (ir->ir_type) {
      case ir_type_dereference_array: {
         const ir_dereference_array *const array_deref =
            (const ir_dereference_array *) ir;

         ir = array_deref->array;
         break;
      }

      case ir_type_dereference_record: {
         const ir_dereference_record *const record_deref =
            (const ir_dereference_record *) ir;

         ir = record_deref->record;

         const int idx = record_deref->field_idx;
         assert(idx >= 0);

         const enum glsl_matrix_layout matrix_layout =
            glsl_matrix_layout(ir->type->fields.structure[idx].matrix_layout);

         switch (matrix_layout) {
         case GLSL_MATRIX_LAYOUT_INHERITED:
            break;
         case GLSL_MATRIX_LAYOUT_COLUMN_MAJOR:
            return false;
         case GLSL_MATRIX_LAYOUT_ROW_MAJOR:
            return matrix || deref->type->without_array()->is_struct();
         }

         break;
      }

      case ir_type_dereference_variable: {
         const ir_dereference_variable *const var_deref =
            (const ir_dereference_variable *) ir;

         const enum glsl_matrix_layout matrix_layout =
            glsl_matrix_layout(var_deref->var->data.matrix_layout);

         switch (matrix_layout) {
         case GLSL_MATRIX_LAYOUT_INHERITED: {
            /* For interface block matrix variables we handle inherited
             * layouts at HIR generation time, but we don't do that for shared
             * variables, which are always column-major
             */
            ASSERTED ir_variable *var = deref->variable_referenced();
            assert((var->is_in_buffer_block() && !matrix) ||
                   var->data.mode == ir_var_shader_shared);
            return false;
         }
         case GLSL_MATRIX_LAYOUT_COLUMN_MAJOR:
            return false;
         case GLSL_MATRIX_LAYOUT_ROW_MAJOR:
            return matrix || deref->type->without_array()->is_struct();
         }

         unreachable("invalid matrix layout");
         break;
      }

      default:
         return false;
      }
   }

   /* The tree must have ended with a dereference that wasn't an
    * ir_dereference_variable.  That is invalid, and it should be impossible.
    */
   unreachable("invalid dereference tree");
   return false;
}

/**
 * This function initializes various values that will be used later by
 * emit_access when actually emitting loads or stores.
 *
 * Note: const_offset is an input as well as an output, clients must
 * initialize it to the offset of the variable in the underlying block, and
 * this function will adjust it by adding the constant offset of the member
 * being accessed into that variable.
 */
void
lower_shared_reference_visitor::setup_buffer_access(void *mem_ctx,
                                                    ir_rvalue *deref,
                                                    ir_rvalue **offset,
                                                    unsigned *const_offset,
                                                    bool *row_major,
                                                    const glsl_type **matrix_type,
                                                    const glsl_struct_field **struct_field,
                                                    enum glsl_interface_packing packing)
{
   *offset = new(mem_ctx) ir_constant(0u);
   *row_major = is_dereferenced_thing_row_major(deref);
   *matrix_type = NULL;

   /* Calculate the offset to the start of the region of the UBO
    * dereferenced by *rvalue.  This may be a variable offset if an
    * array dereference has a variable index.
    */
   while (deref) {
      switch (deref->ir_type) {
      case ir_type_dereference_variable: {
         deref = NULL;
         break;
      }

      case ir_type_dereference_array: {
         ir_dereference_array *deref_array = (ir_dereference_array *) deref;
         unsigned array_stride;
         if (deref_array->array->type->is_vector()) {
            /* We get this when storing or loading a component out of a vector
             * with a non-constant index. This happens for v[i] = f where v is
             * a vector (or m[i][j] = f where m is a matrix). If we don't
             * lower that here, it gets turned into v = vector_insert(v, i,
             * f), which loads the entire vector, modifies one component and
             * then write the entire thing back.  That breaks if another
             * thread or SIMD channel is modifying the same vector.
             */
            array_stride = 4;
            if (deref_array->array->type->is_64bit())
               array_stride *= 2;
         } else if (deref_array->array->type->is_matrix() && *row_major) {
            /* When loading a vector out of a row major matrix, the
             * step between the columns (vectors) is the size of a
             * float, while the step between the rows (elements of a
             * vector) is handled below in emit_ubo_loads.
             */
            array_stride = 4;
            if (deref_array->array->type->is_64bit())
               array_stride *= 2;
            *matrix_type = deref_array->array->type;
         } else if (deref_array->type->without_array()->is_interface()) {
            /* We're processing an array dereference of an interface instance
             * array. The thing being dereferenced *must* be a variable
             * dereference because interfaces cannot be embedded in other
             * types. In terms of calculating the offsets for the lowering
             * pass, we don't care about the array index. All elements of an
             * interface instance array will have the same offsets relative to
             * the base of the block that backs them.
             */
            deref = deref_array->array->as_dereference();
            break;
         } else {
            /* Whether or not the field is row-major (because it might be a
             * bvec2 or something) does not affect the array itself. We need
             * to know whether an array element in its entirety is row-major.
             */
            const bool array_row_major =
               is_dereferenced_thing_row_major(deref_array);

            /* The array type will give the correct interface packing
             * information
             */
            if (packing == GLSL_INTERFACE_PACKING_STD430) {
               array_stride = deref_array->type->std430_array_stride(array_row_major);
            } else {
               array_stride = deref_array->type->std140_size(array_row_major);
               array_stride = glsl_align(array_stride, 16);
            }
         }

         ir_rvalue *array_index = deref_array->array_index;
         if (array_index->type->base_type == GLSL_TYPE_INT)
            array_index = i2u(array_index);

         ir_constant *const_index =
            array_index->constant_expression_value(mem_ctx, NULL);
         if (const_index) {
            *const_offset += array_stride * const_index->value.u[0];
         } else {
            *offset = add(*offset,
                          mul(array_index,
                              new(mem_ctx) ir_constant(array_stride)));
         }
         deref = deref_array->array->as_dereference();
         break;
      }

      case ir_type_dereference_record: {
         ir_dereference_record *deref_record = (ir_dereference_record *) deref;
         const glsl_type *struct_type = deref_record->record->type;
         unsigned intra_struct_offset = 0;

         for (unsigned int i = 0; i < struct_type->length; i++) {
            const glsl_type *type = struct_type->fields.structure[i].type;

            ir_dereference_record *field_deref = new(mem_ctx)
               ir_dereference_record(deref_record->record,
                                     struct_type->fields.structure[i].name);
            const bool field_row_major =
               is_dereferenced_thing_row_major(field_deref);

            ralloc_free(field_deref);

            unsigned field_align = 0;

            if (packing == GLSL_INTERFACE_PACKING_STD430)
               field_align = type->std430_base_alignment(field_row_major);
            else
               field_align = type->std140_base_alignment(field_row_major);

            if (struct_type->fields.structure[i].offset != -1) {
               intra_struct_offset = struct_type->fields.structure[i].offset;
            }

            intra_struct_offset = glsl_align(intra_struct_offset, field_align);

            assert(deref_record->field_idx >= 0);
            if (i == (unsigned) deref_record->field_idx) {
               if (struct_field)
                  *struct_field = &struct_type->fields.structure[i];
               break;
            }

            if (packing == GLSL_INTERFACE_PACKING_STD430)
               intra_struct_offset += type->std430_size(field_row_major);
            else
               intra_struct_offset += type->std140_size(field_row_major);

            /* If the field just examined was itself a structure, apply rule
             * #9:
             *
             *     "The structure may have padding at the end; the base offset
             *     of the member following the sub-structure is rounded up to
             *     the next multiple of the base alignment of the structure."
             */
            if (type->without_array()->is_struct()) {
               intra_struct_offset = glsl_align(intra_struct_offset,
                                                field_align);

            }
         }

         *const_offset += intra_struct_offset;
         deref = deref_record->record->as_dereference();
         break;
      }

      case ir_type_swizzle: {
         ir_swizzle *deref_swizzle = (ir_swizzle *) deref;

         assert(deref_swizzle->mask.num_components == 1);

         *const_offset += deref_swizzle->mask.x * sizeof(int);
         deref = deref_swizzle->val->as_dereference();
         break;
      }

      default:
         assert(!"not reached");
         deref = NULL;
         break;
      }
   }
}

unsigned
lower_shared_reference_visitor::get_shared_offset(const ir_variable *var)
{
   list_for_each_entry(var_offset, var_entry, &var_offsets, node) {
      if (var_entry->var == var)
         return var_entry->offset;
   }

   struct var_offset *new_entry = rzalloc(list_ctx, struct var_offset);
   list_add(&new_entry->node, &var_offsets);
   new_entry->var = var;

   unsigned var_align = var->type->std430_base_alignment(false);
   new_entry->offset = glsl_align(shared_size, var_align);

   unsigned var_size = var->type->std430_size(false);
   shared_size = new_entry->offset + var_size;

   return new_entry->offset;
}

void
lower_shared_reference_visitor::handle_rvalue(ir_rvalue **rvalue)
{
   if (!*rvalue)
      return;

   ir_dereference *deref = (*rvalue)->as_dereference();
   if (!deref)
      return;

   ir_variable *var = deref->variable_referenced();
   if (!var || var->data.mode != ir_var_shader_shared)
      return;

   buffer_access_type = shared_load_access;

   void *mem_ctx = ralloc_parent(shader->ir);

   ir_rvalue *offset = NULL;
   unsigned const_offset = get_shared_offset(var);
   bool row_major;
   const glsl_type *matrix_type;
   assert(var->get_interface_type() == NULL);
   const enum glsl_interface_packing packing = GLSL_INTERFACE_PACKING_STD430;

   setup_buffer_access(mem_ctx, deref,
                       &offset, &const_offset,
                       &row_major, &matrix_type, NULL, packing);

   /* Now that we've calculated the offset to the start of the
    * dereference, walk over the type and emit loads into a temporary.
    */
   const glsl_type *type = (*rvalue)->type;
   ir_variable *load_var = new(mem_ctx) ir_variable(type,
                                                    "shared_load_temp",
                                                    ir_var_temporary);
   base_ir->insert_before(load_var);

   ir_variable *load_offset = new(mem_ctx) ir_variable(glsl_type::uint_type,
                                                       "shared_load_temp_offset",
                                                       ir_var_temporary);
   base_ir->insert_before(load_offset);
   base_ir->insert_before(assign(load_offset, offset));

   deref = new(mem_ctx) ir_dereference_variable(load_var);

   emit_access(mem_ctx, false, deref, load_offset, const_offset, row_major,
               matrix_type, packing, 0);

   *rvalue = deref;

   progress = true;
}

void
lower_shared_reference_visitor::handle_assignment(ir_assignment *ir)
{
   if (!ir || !ir->lhs)
      return;

   ir_rvalue *rvalue = ir->lhs->as_rvalue();
   if (!rvalue)
      return;

   ir_dereference *deref = ir->lhs->as_dereference();
   if (!deref)
      return;

   ir_variable *var = ir->lhs->variable_referenced();
   if (!var || var->data.mode != ir_var_shader_shared)
      return;

   buffer_access_type = shared_store_access;

   /* We have a write to a shared variable, so declare a temporary and rewrite
    * the assignment so that the temporary is the LHS.
    */
   void *mem_ctx = ralloc_parent(shader->ir);

   const glsl_type *type = rvalue->type;
   ir_variable *store_var = new(mem_ctx) ir_variable(type,
                                                     "shared_store_temp",
                                                     ir_var_temporary);
   base_ir->insert_before(store_var);
   ir->lhs = new(mem_ctx) ir_dereference_variable(store_var);

   ir_rvalue *offset = NULL;
   unsigned const_offset = get_shared_offset(var);
   bool row_major;
   const glsl_type *matrix_type;
   assert(var->get_interface_type() == NULL);
   const enum glsl_interface_packing packing = GLSL_INTERFACE_PACKING_STD430;

   setup_buffer_access(mem_ctx, deref,
                       &offset, &const_offset,
                       &row_major, &matrix_type, NULL, packing);

   deref = new(mem_ctx) ir_dereference_variable(store_var);

   ir_variable *store_offset = new(mem_ctx) ir_variable(glsl_type::uint_type,
                                                        "shared_store_temp_offset",
                                                        ir_var_temporary);
   base_ir->insert_before(store_offset);
   base_ir->insert_before(assign(store_offset, offset));

   /* Now we have to write the value assigned to the temporary back to memory */
   emit_access(mem_ctx, true, deref, store_offset, const_offset, row_major,
               matrix_type, packing, ir->write_mask);

   progress = true;
}

ir_visitor_status
lower_shared_reference_visitor::visit_enter(ir_assignment *ir)
{
   handle_assignment(ir);
   return rvalue_visit(ir);
}

void
lower_shared_reference_visitor::insert_buffer_access(void *mem_ctx,
                                                     ir_dereference *deref,
                                                     const glsl_type *type,
                                                     ir_rvalue *offset,
                                                     unsigned mask,
                                                     int /* channel */)
{
   if (buffer_access_type == shared_store_access) {
      ir_call *store = shared_store(mem_ctx, deref, offset, mask);
      base_ir->insert_after(store);
   } else {
      ir_call *load = shared_load(mem_ctx, type, offset);
      base_ir->insert_before(load);
      ir_rvalue *value = load->return_deref->as_rvalue()->clone(mem_ctx, NULL);
      base_ir->insert_before(assign(deref->clone(mem_ctx, NULL),
                                    value));
   }
}

static bool
compute_shader_enabled(const _mesa_glsl_parse_state *state)
{
   return state->stage == MESA_SHADER_COMPUTE;
}

ir_call *
lower_shared_reference_visitor::shared_store(void *mem_ctx,
                                             ir_rvalue *deref,
                                             ir_rvalue *offset,
                                             unsigned write_mask)
{
   exec_list sig_params;

   ir_variable *offset_ref = new(mem_ctx)
      ir_variable(glsl_type::uint_type, "offset" , ir_var_function_in);
   sig_params.push_tail(offset_ref);

   ir_variable *val_ref = new(mem_ctx)
      ir_variable(deref->type, "value" , ir_var_function_in);
   sig_params.push_tail(val_ref);

   ir_variable *writemask_ref = new(mem_ctx)
      ir_variable(glsl_type::uint_type, "write_mask" , ir_var_function_in);
   sig_params.push_tail(writemask_ref);

   ir_function_signature *sig = new(mem_ctx)
      ir_function_signature(glsl_type::void_type, compute_shader_enabled);
   assert(sig);
   sig->replace_parameters(&sig_params);
   sig->intrinsic_id = ir_intrinsic_shared_store;

   ir_function *f = new(mem_ctx) ir_function("__intrinsic_store_shared");
   f->add_signature(sig);

   exec_list call_params;
   call_params.push_tail(offset->clone(mem_ctx, NULL));
   call_params.push_tail(deref->clone(mem_ctx, NULL));
   call_params.push_tail(new(mem_ctx) ir_constant(write_mask));
   return new(mem_ctx) ir_call(sig, NULL, &call_params);
}

ir_call *
lower_shared_reference_visitor::shared_load(void *mem_ctx,
                                            const struct glsl_type *type,
                                            ir_rvalue *offset)
{
   exec_list sig_params;

   ir_variable *offset_ref = new(mem_ctx)
      ir_variable(glsl_type::uint_type, "offset_ref" , ir_var_function_in);
   sig_params.push_tail(offset_ref);

   ir_function_signature *sig =
      new(mem_ctx) ir_function_signature(type, compute_shader_enabled);
   assert(sig);
   sig->replace_parameters(&sig_params);
   sig->intrinsic_id = ir_intrinsic_shared_load;

   ir_function *f = new(mem_ctx) ir_function("__intrinsic_load_shared");
   f->add_signature(sig);

   ir_variable *result = new(mem_ctx)
      ir_variable(type, "shared_load_result", ir_var_temporary);
   base_ir->insert_before(result);
   ir_dereference_variable *deref_result = new(mem_ctx)
      ir_dereference_variable(result);

   exec_list call_params;
   call_params.push_tail(offset->clone(mem_ctx, NULL));

   return new(mem_ctx) ir_call(sig, deref_result, &call_params);
}

/* Lowers the intrinsic call to a new internal intrinsic that swaps the access
 * to the shared variable in the first parameter by an offset. This involves
 * creating the new internal intrinsic (i.e. the new function signature).
 */
ir_call *
lower_shared_reference_visitor::lower_shared_atomic_intrinsic(ir_call *ir)
{
   /* Shared atomics usually have 2 parameters, the shared variable and an
    * integer argument. The exception is CompSwap, that has an additional
    * integer parameter.
    */
   int param_count = ir->actual_parameters.length();
   assert(param_count == 2 || param_count == 3);

   /* First argument must be a scalar integer shared variable */
   exec_node *param = ir->actual_parameters.get_head();
   ir_instruction *inst = (ir_instruction *) param;
   assert(inst->ir_type == ir_type_dereference_variable ||
          inst->ir_type == ir_type_dereference_array ||
          inst->ir_type == ir_type_dereference_record ||
          inst->ir_type == ir_type_swizzle);

   ir_rvalue *deref = (ir_rvalue *) inst;
   assert(deref->type->is_scalar() &&
          (deref->type->is_integer_32_64() || deref->type->is_float()));

   ir_variable *var = deref->variable_referenced();
   assert(var);

   /* Compute the offset to the start if the dereference
    */
   void *mem_ctx = ralloc_parent(shader->ir);

   ir_rvalue *offset = NULL;
   unsigned const_offset = get_shared_offset(var);
   bool row_major;
   const glsl_type *matrix_type;
   assert(var->get_interface_type() == NULL);
   const enum glsl_interface_packing packing = GLSL_INTERFACE_PACKING_STD430;
   buffer_access_type = shared_atomic_access;

   setup_buffer_access(mem_ctx, deref,
                       &offset, &const_offset,
                       &row_major, &matrix_type, NULL, packing);

   assert(offset);
   assert(!row_major);
   assert(matrix_type == NULL);

   ir_rvalue *deref_offset =
      add(offset, new(mem_ctx) ir_constant(const_offset));

   /* Create the new internal function signature that will take an offset
    * instead of a shared variable
    */
   exec_list sig_params;
   ir_variable *sig_param = new(mem_ctx)
      ir_variable(glsl_type::uint_type, "offset" , ir_var_function_in);
   sig_params.push_tail(sig_param);

   const glsl_type *type = deref->type->get_scalar_type();
   sig_param = new(mem_ctx)
         ir_variable(type, "data1", ir_var_function_in);
   sig_params.push_tail(sig_param);

   if (param_count == 3) {
      sig_param = new(mem_ctx)
            ir_variable(type, "data2", ir_var_function_in);
      sig_params.push_tail(sig_param);
   }

   ir_function_signature *sig =
      new(mem_ctx) ir_function_signature(deref->type,
                                         compute_shader_enabled);
   assert(sig);
   sig->replace_parameters(&sig_params);

   assert(ir->callee->intrinsic_id >= ir_intrinsic_generic_load);
   assert(ir->callee->intrinsic_id <= ir_intrinsic_generic_atomic_comp_swap);
   sig->intrinsic_id = MAP_INTRINSIC_TO_TYPE(ir->callee->intrinsic_id, shared);

   char func_name[64];
   sprintf(func_name, "%s_shared", ir->callee_name());
   ir_function *f = new(mem_ctx) ir_function(func_name);
   f->add_signature(sig);

   /* Now, create the call to the internal intrinsic */
   exec_list call_params;
   call_params.push_tail(deref_offset);
   param = ir->actual_parameters.get_head()->get_next();
   ir_rvalue *param_as_rvalue = ((ir_instruction *) param)->as_rvalue();
   call_params.push_tail(param_as_rvalue->clone(mem_ctx, NULL));
   if (param_count == 3) {
      param = param->get_next();
      param_as_rvalue = ((ir_instruction *) param)->as_rvalue();
      call_params.push_tail(param_as_rvalue->clone(mem_ctx, NULL));
   }
   ir_dereference_variable *return_deref =
      ir->return_deref->clone(mem_ctx, NULL);
   return new(mem_ctx) ir_call(sig, return_deref, &call_params);
}

ir_call *
lower_shared_reference_visitor::check_for_shared_atomic_intrinsic(ir_call *ir)
{
   exec_list& params = ir->actual_parameters;

   if (params.length() < 2 || params.length() > 3)
      return ir;

   ir_rvalue *rvalue =
      ((ir_instruction *) params.get_head())->as_rvalue();
   if (!rvalue)
      return ir;

   ir_variable *var = rvalue->variable_referenced();
   if (!var || var->data.mode != ir_var_shader_shared)
      return ir;

   const enum ir_intrinsic_id id = ir->callee->intrinsic_id;
   if (id == ir_intrinsic_generic_atomic_add ||
       id == ir_intrinsic_generic_atomic_min ||
       id == ir_intrinsic_generic_atomic_max ||
       id == ir_intrinsic_generic_atomic_and ||
       id == ir_intrinsic_generic_atomic_or ||
       id == ir_intrinsic_generic_atomic_xor ||
       id == ir_intrinsic_generic_atomic_exchange ||
       id == ir_intrinsic_generic_atomic_comp_swap) {
      return lower_shared_atomic_intrinsic(ir);
   }

   return ir;
}

ir_visitor_status
lower_shared_reference_visitor::visit_enter(ir_call *ir)
{
   ir_call *new_ir = check_for_shared_atomic_intrinsic(ir);
   if (new_ir != ir) {
      progress = true;
      base_ir->replace_with(new_ir);
      return visit_continue_with_parent;
   }

   return rvalue_visit(ir);
}

} /* unnamed namespace */

void
lower_shared_reference(const struct gl_constants *consts,
                       struct gl_shader_program *prog,
                       struct gl_linked_shader *shader)
{
   if (shader->Stage != MESA_SHADER_COMPUTE)
      return;

   lower_shared_reference_visitor v(shader);

   /* Loop over the instructions lowering references, because we take a deref
    * of an shared variable array using a shared variable dereference as the
    * index will produce a collection of instructions all of which have cloned
    * shared variable dereferences for that array index.
    */
   do {
      v.progress = false;
      visit_list_elements(&v, shader->ir);
   } while (v.progress);

   prog->Comp.SharedSize = v.shared_size;

   /* Section 19.1 (Compute Shader Variables) of the OpenGL 4.5 (Core Profile)
    * specification says:
    *
    *   "There is a limit to the total size of all variables declared as
    *    shared in a single program object. This limit, expressed in units of
    *    basic machine units, may be queried as the value of
    *    MAX_COMPUTE_SHARED_MEMORY_SIZE."
    */
   if (prog->Comp.SharedSize > consts->MaxComputeSharedMemorySize) {
      linker_error(prog, "Too much shared memory used (%u/%u)\n",
                   prog->Comp.SharedSize,
                   consts->MaxComputeSharedMemorySize);
   }
}
