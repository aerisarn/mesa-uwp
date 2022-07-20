/*
 * Copyright © Microsoft Corporation
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "d3d12_bufmgr.h"
#include "d3d12_context.h"
#include "d3d12_format.h"
#include "d3d12_resource_state.h"

#include <assert.h>

#define UNKNOWN_RESOURCE_STATE (D3D12_RESOURCE_STATES) 0x8000u

bool
d3d12_desired_resource_state_init(d3d12_desired_resource_state *state, uint32_t subresource_count)
{
   state->homogenous = true;
   state->num_subresources = subresource_count;
   state->subresource_states = (D3D12_RESOURCE_STATES *)calloc(subresource_count, sizeof(D3D12_RESOURCE_STATES));
   return state->subresource_states != nullptr;
}

void
d3d12_desired_resource_state_cleanup(d3d12_desired_resource_state *state)
{
   free(state->subresource_states);
}

D3D12_RESOURCE_STATES
d3d12_get_desired_subresource_state(const d3d12_desired_resource_state *state, uint32_t subresource_index)
{
   if (state->homogenous)
      subresource_index = 0;
   return state->subresource_states[subresource_index];
}

static void
update_subresource_state(D3D12_RESOURCE_STATES *existing_state, D3D12_RESOURCE_STATES new_state)
{
   if (*existing_state == UNKNOWN_RESOURCE_STATE || new_state == UNKNOWN_RESOURCE_STATE ||
       d3d12_is_write_state(new_state)) {
      *existing_state = new_state;
   } else {
      /* Accumulate read state state bits */
      *existing_state |= new_state;
   }
}

void
d3d12_set_desired_resource_state(d3d12_desired_resource_state *state_obj, D3D12_RESOURCE_STATES state)
{
   state_obj->homogenous = true;
   update_subresource_state(&state_obj->subresource_states[0], state);
}

void
d3d12_set_desired_subresource_state(d3d12_desired_resource_state *state_obj,
                                    uint32_t subresource,
                                    D3D12_RESOURCE_STATES state)
{
   if (state_obj->homogenous && state_obj->num_subresources > 1) {
      for (unsigned i = 1; i < state_obj->num_subresources; ++i) {
         state_obj->subresource_states[i] = state_obj->subresource_states[0];
      }
      state_obj->homogenous = false;
   }

   update_subresource_state(&state_obj->subresource_states[subresource], state);
}

void
d3d12_reset_desired_resource_state(d3d12_desired_resource_state *state_obj)
{
   d3d12_set_desired_resource_state(state_obj, UNKNOWN_RESOURCE_STATE);
}

bool
d3d12_resource_state_init(d3d12_resource_state *state, uint32_t subresource_count, bool simultaneous_access)
{
   state->homogenous = true;
   state->supports_simultaneous_access = simultaneous_access;
   state->num_subresources = subresource_count;
   state->subresource_states = (d3d12_subresource_state *)calloc(subresource_count, sizeof(d3d12_subresource_state));
   return state->subresource_states != nullptr;
}

void
d3d12_resource_state_cleanup(d3d12_resource_state *state)
{
   free(state->subresource_states);
}

const d3d12_subresource_state *
d3d12_get_subresource_state(const d3d12_resource_state *state, uint32_t subresource)
{
   if (state->homogenous)
      subresource = 0;
   return &state->subresource_states[subresource];
}

void
d3d12_set_resource_state(d3d12_resource_state *state_obj, const d3d12_subresource_state *state)
{
   state_obj->homogenous = true;
   state_obj->subresource_states[0] = *state;
}

void
d3d12_set_subresource_state(d3d12_resource_state *state_obj, uint32_t subresource, const d3d12_subresource_state *state)
{
   if (state_obj->homogenous && state_obj->num_subresources > 1) {
      for (unsigned i = 1; i < state_obj->num_subresources; ++i) {
         state_obj->subresource_states[i] = state_obj->subresource_states[0];
      }
      state_obj->homogenous = false;
   }

   state_obj->subresource_states[subresource] = *state;
}

void
d3d12_reset_resource_state(d3d12_resource_state *state)
{
   d3d12_subresource_state subres_state = {};
   d3d12_set_resource_state(state, &subres_state);
}

D3D12_RESOURCE_STATES
d3d12_resource_state_if_promoted(D3D12_RESOURCE_STATES desired_state,
                                 bool simultaneous_access,
                                 const d3d12_subresource_state *current_state)
{
   D3D12_RESOURCE_STATES result = D3D12_RESOURCE_STATE_COMMON;
   const D3D12_RESOURCE_STATES promotable_states = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST;

   if (simultaneous_access ||
       (desired_state & promotable_states) != D3D12_RESOURCE_STATE_COMMON) {
      // If the current state is COMMON...
      if (current_state->state == D3D12_RESOURCE_STATE_COMMON)
         // ...then promotion is allowed
         return desired_state;

      // If the current state is a read state resulting from previous promotion...
      if (current_state->is_promoted &&
          (current_state->state & D3D12_RESOURCE_STATE_GENERIC_READ) != D3D12_RESOURCE_STATE_COMMON)
         // ...then (accumulated) promotion is allowed
         return desired_state | current_state->state;
   }

   return D3D12_RESOURCE_STATE_COMMON;
}

void
d3d12_resource_state_copy(d3d12_resource_state *dest, d3d12_resource_state *src)
{
   assert(dest->num_subresources == src->num_subresources);
   if (src->homogenous)
      d3d12_set_resource_state(dest, &src->subresource_states[0]);
   else {
      dest->homogenous = false;
      for (unsigned i = 0; i < src->num_subresources; ++i)
         dest->subresource_states[i] = src->subresource_states[i];
   }
}

struct d3d12_context_state_table_entry
{
   struct d3d12_desired_resource_state desired;
   struct d3d12_resource_state batch_begin, batch_end;
};

static void
destroy_context_state_table_entry(d3d12_context_state_table_entry *entry)
{
   d3d12_desired_resource_state_cleanup(&entry->desired);
   d3d12_resource_state_cleanup(&entry->batch_begin);
   d3d12_resource_state_cleanup(&entry->batch_end);
   free(entry);
}

void
d3d12_context_state_table_init(struct d3d12_context *ctx)
{
   ctx->bo_state_table = _mesa_hash_table_u64_create(nullptr);
}

void
d3d12_context_state_table_destroy(struct d3d12_context *ctx)
{
   hash_table_foreach(ctx->bo_state_table->table, entry)
      destroy_context_state_table_entry((d3d12_context_state_table_entry *)entry->data);
   _mesa_hash_table_u64_destroy(ctx->bo_state_table);
}

static unsigned
get_subresource_count(const D3D12_RESOURCE_DESC *desc)
{
   unsigned array_size = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc->DepthOrArraySize;
   return desc->MipLevels * array_size * d3d12_non_opaque_plane_count(desc->Format);
}

static void
init_state_table_entry(d3d12_context_state_table_entry *bo_state, d3d12_bo *bo)
{
   /* Default parameters for bos for suballocated buffers */
   unsigned subresource_count = 1;
   bool supports_simultaneous_access = true;
   if (bo->res) {
      D3D12_RESOURCE_DESC desc = GetDesc(bo->res);
      subresource_count = get_subresource_count(&desc);
      supports_simultaneous_access = d3d12_resource_supports_simultaneous_access(&desc);
   }

   d3d12_desired_resource_state_init(&bo_state->desired, subresource_count);
   d3d12_resource_state_init(&bo_state->batch_end, subresource_count, supports_simultaneous_access);

   /* We'll never need state fixups for simultaneous access resources, so don't bother initializing this second state */
   if (!supports_simultaneous_access)
      d3d12_resource_state_init(&bo_state->batch_begin, subresource_count, supports_simultaneous_access);
}

static d3d12_context_state_table_entry *
find_or_create_state_entry(struct hash_table_u64 *table, d3d12_bo *bo)
{
   d3d12_context_state_table_entry *bo_state =
      (d3d12_context_state_table_entry *) _mesa_hash_table_u64_search(table, bo->unique_id);
   if (!bo_state) {
      bo_state = CALLOC_STRUCT(d3d12_context_state_table_entry);
      init_state_table_entry(bo_state, bo);
      _mesa_hash_table_u64_insert(table, bo->unique_id, bo_state);
   }
   return bo_state;
}

void
d3d12_context_state_resolve_submission(struct d3d12_context *ctx, struct d3d12_batch *batch)
{
   util_dynarray_foreach(&ctx->recently_destroyed_bos, uint64_t, id) {
      void *data = _mesa_hash_table_u64_search(ctx->bo_state_table, *id);
      if (data)
         destroy_context_state_table_entry((d3d12_context_state_table_entry *)data);
      _mesa_hash_table_u64_remove(ctx->bo_state_table, *id);
   }

   util_dynarray_clear(&ctx->recently_destroyed_bos);

   hash_table_foreach(batch->bos, bo_entry) {
      d3d12_bo *bo = (d3d12_bo *)bo_entry->key;
      d3d12_context_state_table_entry *bo_state = find_or_create_state_entry(ctx->bo_state_table, bo);
      if (!bo_state->batch_end.supports_simultaneous_access) {
         d3d12_resource_state_copy(&bo_state->batch_begin, &bo_state->batch_end);
      } else {
         d3d12_reset_resource_state(&bo_state->batch_end);
      }
   }
}
