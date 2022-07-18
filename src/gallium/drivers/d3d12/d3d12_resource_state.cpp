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

#include "d3d12_resource_state.h"

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
