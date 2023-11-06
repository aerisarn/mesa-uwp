/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vn_ring.h"

#include "vn_cs.h"
#include "vn_instance.h"
#include "vn_renderer.h"

static uint32_t
vn_ring_load_head(const struct vn_ring *ring)
{
   /* the renderer is expected to store the head with memory_order_release,
    * forming a release-acquire ordering
    */
   return atomic_load_explicit(ring->shared.head, memory_order_acquire);
}

static void
vn_ring_store_tail(struct vn_ring *ring)
{
   /* the renderer is expected to load the tail with memory_order_acquire,
    * forming a release-acquire ordering
    */
   return atomic_store_explicit(ring->shared.tail, ring->cur,
                                memory_order_release);
}

uint32_t
vn_ring_load_status(const struct vn_ring *ring)
{
   /* must be called and ordered after vn_ring_store_tail for idle status */
   return atomic_load_explicit(ring->shared.status, memory_order_seq_cst);
}

void
vn_ring_unset_status_bits(struct vn_ring *ring, uint32_t mask)
{
   atomic_fetch_and_explicit(ring->shared.status, ~mask,
                             memory_order_seq_cst);
}

static void
vn_ring_write_buffer(struct vn_ring *ring, const void *data, uint32_t size)
{
   assert(ring->cur + size - vn_ring_load_head(ring) <= ring->buffer_size);

   const uint32_t offset = ring->cur & ring->buffer_mask;
   if (offset + size <= ring->buffer_size) {
      memcpy(ring->shared.buffer + offset, data, size);
   } else {
      const uint32_t s = ring->buffer_size - offset;
      memcpy(ring->shared.buffer + offset, data, s);
      memcpy(ring->shared.buffer, data + s, size - s);
   }

   ring->cur += size;
}

static bool
vn_ring_ge_seqno(const struct vn_ring *ring, uint32_t a, uint32_t b)
{
   /* this can return false negative when not called fast enough (e.g., when
    * called once every couple hours), but following calls with larger a's
    * will correct itself
    *
    * TODO use real seqnos?
    */
   if (a >= b)
      return ring->cur >= a || ring->cur < b;
   else
      return ring->cur >= a && ring->cur < b;
}

static void
vn_ring_retire_submits(struct vn_ring *ring, uint32_t seqno)
{
   struct vn_renderer *renderer = ring->instance->renderer;
   list_for_each_entry_safe(struct vn_ring_submit, submit, &ring->submits,
                            head) {
      if (!vn_ring_ge_seqno(ring, seqno, submit->seqno))
         break;

      for (uint32_t i = 0; i < submit->shmem_count; i++)
         vn_renderer_shmem_unref(renderer, submit->shmems[i]);

      list_move_to(&submit->head, &ring->free_submits);
   }
}

bool
vn_ring_get_seqno_status(struct vn_ring *ring, uint32_t seqno)
{
   return vn_ring_ge_seqno(ring, vn_ring_load_head(ring), seqno);
}

void
vn_ring_wait_seqno(struct vn_ring *ring, uint32_t seqno)
{
   /* A renderer wait incurs several hops and the renderer might poll
    * repeatedly anyway.  Let's just poll here.
    */
   struct vn_relax_state relax_state =
      vn_relax_init(ring->instance, "ring seqno");
   do {
      if (vn_ring_get_seqno_status(ring, seqno)) {
         vn_relax_fini(&relax_state);
         return;
      }
      vn_relax(&relax_state);
   } while (true);
}

static bool
vn_ring_has_space(const struct vn_ring *ring,
                  uint32_t size,
                  uint32_t *out_head)
{
   const uint32_t head = vn_ring_load_head(ring);
   if (likely(ring->cur + size - head <= ring->buffer_size)) {
      *out_head = head;
      return true;
   }

   return false;
}

static uint32_t
vn_ring_wait_space(struct vn_ring *ring, uint32_t size)
{
   assert(size <= ring->buffer_size);

   uint32_t head;
   if (likely(vn_ring_has_space(ring, size, &head)))
      return head;

   {
      VN_TRACE_FUNC();

      /* see the reasoning in vn_ring_wait_seqno */
      struct vn_relax_state relax_state =
         vn_relax_init(ring->instance, "ring space");
      do {
         vn_relax(&relax_state);
         if (vn_ring_has_space(ring, size, &head)) {
            vn_relax_fini(&relax_state);
            return head;
         }
      } while (true);
   }
}

void
vn_ring_get_layout(size_t buf_size,
                   size_t extra_size,
                   struct vn_ring_layout *layout)
{
   /* this can be changed/extended quite freely */
   struct layout {
      alignas(64) uint32_t head;
      alignas(64) uint32_t tail;
      alignas(64) uint32_t status;

      alignas(64) uint8_t buffer[];
   };

   assert(buf_size && util_is_power_of_two_or_zero(buf_size));

   layout->head_offset = offsetof(struct layout, head);
   layout->tail_offset = offsetof(struct layout, tail);
   layout->status_offset = offsetof(struct layout, status);

   layout->buffer_offset = offsetof(struct layout, buffer);
   layout->buffer_size = buf_size;

   layout->extra_offset = layout->buffer_offset + layout->buffer_size;
   layout->extra_size = extra_size;

   layout->shmem_size = layout->extra_offset + layout->extra_size;
}

struct vn_ring *
vn_ring_create(struct vn_instance *instance,
               const struct vn_ring_layout *layout)
{
   VN_TRACE_FUNC();

   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;

   struct vn_ring *ring = vk_zalloc(alloc, sizeof(*ring), VN_DEFAULT_ALIGN,
                                    VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!ring)
      return NULL;

   ring->shmem =
      vn_renderer_shmem_create(instance->renderer, layout->shmem_size);
   if (!ring->shmem) {
      if (VN_DEBUG(INIT))
         vn_log(instance, "failed to allocate/map ring shmem");
      vk_free(alloc, ring);
      return NULL;
   }

   void *shared = ring->shmem->mmap_ptr;
   memset(shared, 0, layout->shmem_size);

   ring->instance = instance;

   assert(layout->buffer_size &&
          util_is_power_of_two_or_zero(layout->buffer_size));
   ring->buffer_size = layout->buffer_size;
   ring->buffer_mask = ring->buffer_size - 1;

   ring->shared.head = shared + layout->head_offset;
   ring->shared.tail = shared + layout->tail_offset;
   ring->shared.status = shared + layout->status_offset;
   ring->shared.buffer = shared + layout->buffer_offset;
   ring->shared.extra = shared + layout->extra_offset;

   list_inithead(&ring->submits);
   list_inithead(&ring->free_submits);

   return ring;
}

void
vn_ring_destroy(struct vn_ring *ring)
{
   VN_TRACE_FUNC();

   const VkAllocationCallbacks *alloc = &ring->instance->base.base.alloc;

   vn_ring_retire_submits(ring, ring->cur);
   assert(list_is_empty(&ring->submits));

   list_for_each_entry_safe(struct vn_ring_submit, submit,
                            &ring->free_submits, head)
      vk_free(alloc, submit);

   vk_free(alloc, ring);
}

struct vn_ring_submit *
vn_ring_get_submit(struct vn_ring *ring, uint32_t shmem_count)
{
   const VkAllocationCallbacks *alloc = &ring->instance->base.base.alloc;
   const uint32_t min_shmem_count = 2;
   struct vn_ring_submit *submit;

   /* TODO this could be simplified if we could omit shmem_count */
   if (shmem_count <= min_shmem_count &&
       !list_is_empty(&ring->free_submits)) {
      submit =
         list_first_entry(&ring->free_submits, struct vn_ring_submit, head);
      list_del(&submit->head);
   } else {
      const size_t submit_size = offsetof(
         struct vn_ring_submit, shmems[MAX2(shmem_count, min_shmem_count)]);
      submit = vk_alloc(alloc, submit_size, VN_DEFAULT_ALIGN,
                        VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   }

   return submit;
}

bool
vn_ring_submit(struct vn_ring *ring,
               struct vn_ring_submit *submit,
               const struct vn_cs_encoder *cs,
               uint32_t *seqno)
{
   /* write cs to the ring */
   assert(!vn_cs_encoder_is_empty(cs));

   /* avoid -Wmaybe-unitialized */
   uint32_t cur_seqno = 0;

   for (uint32_t i = 0; i < cs->buffer_count; i++) {
      const struct vn_cs_encoder_buffer *buf = &cs->buffers[i];
      cur_seqno = vn_ring_wait_space(ring, buf->committed_size);
      vn_ring_write_buffer(ring, buf->base, buf->committed_size);
   }

   vn_ring_store_tail(ring);
   const VkRingStatusFlagsMESA status = vn_ring_load_status(ring);
   if (status & VK_RING_STATUS_FATAL_BIT_MESA) {
      vn_log(NULL, "vn_ring_submit abort on fatal");
      abort();
   }

   vn_ring_retire_submits(ring, cur_seqno);

   submit->seqno = ring->cur;
   list_addtail(&submit->head, &ring->submits);

   *seqno = submit->seqno;

   /* notify renderer to wake up ring if idle */
   return status & VK_RING_STATUS_IDLE_BIT_MESA;
}
