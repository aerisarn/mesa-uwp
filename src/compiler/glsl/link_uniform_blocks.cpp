/*
 * Copyright © 2012 Intel Corporation
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

#include "ir.h"
#include "main/shader_types.h"

static bool
link_uniform_blocks_are_compatible(const gl_uniform_block *a,
                                   const gl_uniform_block *b)
{
   assert(strcmp(a->name.string, b->name.string) == 0);

   /* Page 35 (page 42 of the PDF) in section 4.3.7 of the GLSL 1.50 spec says:
    *
    *    Matched block names within an interface (as defined above) must match
    *    in terms of having the same number of declarations with the same
    *    sequence of types and the same sequence of member names, as well as
    *    having the same member-wise layout qualification....if a matching
    *    block is declared as an array, then the array sizes must also
    *    match... Any mismatch will generate a link error.
    *
    * Arrays are not yet supported, so there is no check for that.
    */
   if (a->NumUniforms != b->NumUniforms)
      return false;

   if (a->_Packing != b->_Packing)
      return false;

   if (a->_RowMajor != b->_RowMajor)
      return false;

   if (a->Binding != b->Binding)
      return false;

   for (unsigned i = 0; i < a->NumUniforms; i++) {
      if (strcmp(a->Uniforms[i].Name, b->Uniforms[i].Name) != 0)
         return false;

      if (a->Uniforms[i].Type != b->Uniforms[i].Type)
         return false;

      if (a->Uniforms[i].RowMajor != b->Uniforms[i].RowMajor)
         return false;
   }

   return true;
}

/**
 * Merges a uniform block into an array of uniform blocks that may or
 * may not already contain a copy of it.
 *
 * Returns the index of the new block in the array.
 */
int
link_cross_validate_uniform_block(void *mem_ctx,
                                  struct gl_uniform_block **linked_blocks,
                                  unsigned int *num_linked_blocks,
                                  struct gl_uniform_block *new_block)
{
   for (unsigned int i = 0; i < *num_linked_blocks; i++) {
      struct gl_uniform_block *old_block = &(*linked_blocks)[i];

      if (strcmp(old_block->name.string, new_block->name.string) == 0)
         return link_uniform_blocks_are_compatible(old_block, new_block)
            ? i : -1;
   }

   *linked_blocks = reralloc(mem_ctx, *linked_blocks,
                             struct gl_uniform_block,
                             *num_linked_blocks + 1);
   int linked_block_index = (*num_linked_blocks)++;
   struct gl_uniform_block *linked_block = &(*linked_blocks)[linked_block_index];

   memcpy(linked_block, new_block, sizeof(*new_block));
   linked_block->Uniforms = ralloc_array(*linked_blocks,
                                         struct gl_uniform_buffer_variable,
                                         linked_block->NumUniforms);

   memcpy(linked_block->Uniforms,
          new_block->Uniforms,
          sizeof(*linked_block->Uniforms) * linked_block->NumUniforms);

   linked_block->name.string = ralloc_strdup(*linked_blocks, linked_block->name.string);
   resource_name_updated(&linked_block->name);

   for (unsigned int i = 0; i < linked_block->NumUniforms; i++) {
      struct gl_uniform_buffer_variable *ubo_var =
         &linked_block->Uniforms[i];

      if (ubo_var->Name == ubo_var->IndexName) {
         ubo_var->Name = ralloc_strdup(*linked_blocks, ubo_var->Name);
         ubo_var->IndexName = ubo_var->Name;
      } else {
         ubo_var->Name = ralloc_strdup(*linked_blocks, ubo_var->Name);
         ubo_var->IndexName = ralloc_strdup(*linked_blocks, ubo_var->IndexName);
      }
   }

   return linked_block_index;
}
