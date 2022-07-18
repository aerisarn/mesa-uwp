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

#ifndef D3D12RESOURCESTATE_H
#define D3D12RESOURCESTATE_H

#include <vector>
#include <assert.h>

#include "util/list.h"

#include "d3d12_common.h"
#include "d3d12_resource_state.h"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

#define RESOURCE_STATE_VALID_BITS 0x2f3fff
#define RESOURCE_STATE_VALID_INTERNAL_BITS 0x2fffff

//---------------------------------------------------------------------------------------------------------------------------------
    
//==================================================================================================================================
// TransitionableResourceState
// A base class that transitionable resources should inherit from.
//==================================================================================================================================
struct TransitionableResourceState
{
   struct list_head m_TransitionListEntry;
   struct d3d12_desired_resource_state m_DesiredState;
   struct d3d12_resource_state m_currentState;

   TransitionableResourceState(ID3D12Resource *pResource, UINT TotalSubresources, bool SupportsSimultaneousAccess) :
      m_TotalSubresources(TotalSubresources),
      m_pResource(pResource)
   {
      list_inithead(&m_TransitionListEntry);
      d3d12_desired_resource_state_init(&m_DesiredState, TotalSubresources);
      d3d12_resource_state_init(&m_currentState, TotalSubresources, SupportsSimultaneousAccess);
   }

   ~TransitionableResourceState()
   {
      d3d12_desired_resource_state_cleanup(&m_DesiredState);
      if (IsTransitionPending())
      {
         list_del(&m_TransitionListEntry);
      }
   }

   bool IsTransitionPending() const { return !list_is_empty(&m_TransitionListEntry); }

   UINT NumSubresources() { return m_TotalSubresources; }

   inline ID3D12Resource* GetD3D12Resource() const { return m_pResource; }

private:
   unsigned m_TotalSubresources;

   ID3D12Resource* m_pResource;
};

//==================================================================================================================================
// ResourceStateManager
// The main business logic for handling resource transitions, including multi-queue sync and shared/exclusive state changes.
//
// Requesting a resource to transition simply updates destination state, and ensures it's in a list to be processed later.
//
// When processing ApplyAllResourceTransitions, we build up sets of vectors.
// There's a source one for each command list type, and a single one for the dest because we are applying
// the resource transitions for a single operation.
// There's also a vector for "tentative" barriers, which are merged into the destination vector if
// no flushing occurs as a result of submitting the final barrier operation.
// 99% of the time, there will only be the source being populated, but sometimes there will be a destination as well.
// If the source and dest of a transition require different types, we put a (source->COMMON) in the approriate source vector,
// and a (COMMON->dest) in the destination vector.
//
// Once all resources are processed, we:
// 1. Submit all source barriers, except ones belonging to the destination queue.
// 2. Flush all source command lists, except ones belonging to the destination queue.
// 3. Determine if the destination queue is going to be flushed.
//    If so: Submit source barriers on that command list first, then flush it.
//    If not: Accumulate source, dest, and tentative barriers so they can be sent to D3D12 in a single API call.
// 4. Insert waits on the destination queue - deferred waits, and waits for work on other queues.
// 5. Insert destination barriers.
//
// Only once all of this has been done do we update the "current" state of resources,
// because this is the only way that we know whether or not the destination queue has been flushed,
// and therefore, we can get the correct fence values to store in the subresources.
//==================================================================================================================================
class ResourceStateManager
{
protected:

   struct list_head m_TransitionListHead;

   std::vector<D3D12_RESOURCE_BARRIER> m_vResourceBarriers;
   bool m_IsImplicitDispatch;

public:
   ResourceStateManager();

   ~ResourceStateManager()
   {
      // All resources should be gone by this point, and each resource ensures it is no longer in this list.
      assert(list_is_empty(&m_TransitionListHead));
   }

   // Call the D3D12 APIs to perform the resource barriers, command list submission, and command queue sync
   // that was determined by previous calls to ProcessTransitioningResource.
   void SubmitResourceTransitions(ID3D12GraphicsCommandList *pCommandList);

   // Transition the entire resource to a particular destination state on a particular command list.
   void TransitionResource(TransitionableResourceState* pResource,
                           D3D12_RESOURCE_STATES State);
   // Transition a single subresource to a particular destination state.
   void TransitionSubresource(TransitionableResourceState* pResource,
                              UINT SubresourceIndex,
                              D3D12_RESOURCE_STATES State);

   // Submit all barriers and queue sync.
   void ApplyAllResourceTransitions(ID3D12GraphicsCommandList *pCommandList, UINT64 ExecutionId, bool IsImplicitDispatch);

private:
   // These methods set the destination state of the resource/subresources and ensure it's in the transition list.
   void TransitionResource(TransitionableResourceState& Resource,
                           D3D12_RESOURCE_STATES State);
   void TransitionSubresource(TransitionableResourceState& Resource,
                              UINT SubresourceIndex,
                              D3D12_RESOURCE_STATES State);

   // Clear out any state from previous iterations.
   void ApplyResourceTransitionsPreamble(bool IsImplicitDispatch);

   // What to do with the resource, in the context of the transition list, after processing it.
   enum class TransitionResult
   {
      // There are no more pending transitions that may be processed at a later time (i.e. draw time),
      // so remove it from the pending transition list.
      Remove,
      // There are more transitions to be done, so keep it in the list.
      Keep
   };

   // For every entry in the transition list, call a routine.
   // This routine must return a TransitionResult which indicates what to do with the list.
   template <typename TFunc>
   void ForEachTransitioningResource(TFunc&& func)
   {
      list_for_each_entry_safe(TransitionableResourceState, pResource, &m_TransitionListHead, m_TransitionListEntry)
      {
            func(*pResource);
            list_delinit(&pResource->m_TransitionListEntry);
      }
   }

   // Updates vectors with the operations that should be applied to the requested resource.
   // May update the destination state of the resource.
   void ProcessTransitioningResource(ID3D12Resource* pTransitioningResource,
                                     TransitionableResourceState& TransitionableResourceState,
                                     d3d12_resource_state *CurrentState,
                                     UINT NumTotalSubresources,
                                     UINT64 ExecutionId);

private:
   // Helpers
   static bool TransitionRequired(D3D12_RESOURCE_STATES CurrentState, D3D12_RESOURCE_STATES& DestinationState);
   void AddCurrentStateUpdate(TransitionableResourceState& Resource,
                              d3d12_resource_state *CurrentState,
                              UINT SubresourceIndex,
                              const d3d12_subresource_state *NewLogicalState);
   void ProcessTransitioningSubresourceExplicit(d3d12_resource_state *CurrentState,
                                                UINT i,
                                                D3D12_RESOURCE_STATES after,
                                                TransitionableResourceState& TransitionableResourceState,
                                                D3D12_RESOURCE_BARRIER& TransitionDesc,
                                                UINT64 ExecutionId);
};

#endif // D3D12RESOURCESTATEH
