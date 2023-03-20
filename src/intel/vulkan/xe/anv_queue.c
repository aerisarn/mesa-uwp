/*
 * Copyright © 2023 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "xe/anv_queue.h"

#include "anv_private.h"

#include "common/xe/intel_engine.h"
#include "common/intel_gem.h"

#include "drm-uapi/xe_drm.h"

VkResult
anv_xe_create_engine(struct anv_device *device,
                     struct anv_queue *queue,
                     const VkDeviceQueueCreateInfo *pCreateInfo)
{
   struct anv_physical_device *physical = device->physical;
   struct anv_queue_family *queue_family =
      &physical->queue.families[pCreateInfo->queueFamilyIndex];
   const struct intel_query_engine_info *engines = physical->engine_info;
   struct drm_xe_engine_class_instance *instances;

   instances = vk_alloc(&device->vk.alloc,
                        sizeof(*instances) * queue_family->queueCount, 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!instances)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   /* Build a list of all compatible HW engines */
   uint32_t count = 0;
   for (uint32_t i = 0; i < engines->num_engines; i++) {
      const struct intel_engine_class_instance engine = engines->engines[i];
      if (engine.engine_class != queue_family->engine_class)
         continue;

      instances[count].engine_class = intel_engine_class_to_xe(engine.engine_class);
      instances[count].engine_instance = engine.engine_instance;
      /* TODO: handle gt_id, MTL and newer platforms will have media engines
       * in a separated gt
       */
      instances[count++].gt_id = 0;
   }

   assert(device->vm_id != 0);
   /* TODO: drm_xe_engine_set_property XE_ENGINE_PROPERTY_PRIORITY */
   struct drm_xe_engine_create create = {
         /* Allows KMD to pick one of those engines for the submission queue */
         .instances = (uintptr_t)instances,
         .vm_id = device->vm_id,
         .width = 1,
         .num_placements = count,
   };
   int ret = intel_ioctl(device->fd, DRM_IOCTL_XE_ENGINE_CREATE, &create);
   vk_free(&device->vk.alloc, instances);
   if (ret)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "Unable to create engine");

   queue->engine_id = create.engine_id;
   return VK_SUCCESS;
}

void
anv_xe_destroy_engine(struct anv_device *device, struct anv_queue *queue)
{
   struct drm_xe_engine_destroy destroy = {
      .engine_id = queue->engine_id,
   };
   intel_ioctl(device->fd, DRM_IOCTL_XE_ENGINE_DESTROY, &destroy);
}
