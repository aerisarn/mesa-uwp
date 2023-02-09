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
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "xe/intel_device_info.h"

#include "common/intel_gem.h"
#include "dev/intel_device_info.h"

#include "util/log.h"

#include "drm-uapi/xe_drm.h"

static void *
xe_query_alloc_fetch(int fd, uint32_t query_id, int32_t *len)
{
   struct drm_xe_device_query query = {
      .query = query_id,
   };
   if (intel_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query))
      return NULL;

   void *data = calloc(1, query.size);
   if (!data)
      return NULL;

   query.data = (uintptr_t)data;
   if (intel_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query))
      goto data_query_failed;

   if (len)
      *len = query.size;
   return data;

data_query_failed:
   free(data);
   return NULL;
}

static bool
xe_query_config(int fd, struct intel_device_info *devinfo)
{
   struct drm_xe_query_config *config;
   config = xe_query_alloc_fetch(fd, DRM_XE_DEVICE_QUERY_CONFIG, NULL);
   if (!config)
      return false;

   if (config->info[XE_QUERY_CONFIG_FLAGS] & XE_QUERY_CONFIG_FLAGS_HAS_VRAM)
      devinfo->has_local_mem = true;

   devinfo->revision = (config->info[XE_QUERY_CONFIG_REV_AND_DEVICE_ID] >> 16) & 0xFFFF;
   devinfo->gtt_size = 1ull << config->info[XE_QUERY_CONFIG_VA_BITS];

   free(config);
   return true;
}

bool
intel_device_info_xe_query_regions(int fd, struct intel_device_info *devinfo,
                                   bool update)
{
   struct drm_xe_query_mem_usage *regions;
   regions = xe_query_alloc_fetch(fd, DRM_XE_DEVICE_QUERY_MEM_USAGE, NULL);
   if (!regions)
      return false;

   for (int i = 0; i < regions->num_regions; i++) {
      struct drm_xe_query_mem_region *region = &regions->regions[i];

      switch (region->mem_class) {
      case XE_MEM_REGION_CLASS_SYSMEM: {
         if (!update) {
            devinfo->mem.sram.mem.klass = region->mem_class;
            devinfo->mem.sram.mem.instance = region->instance;
            devinfo->mem.sram.mappable.size = region->total_size;
         } else {
            assert(devinfo->mem.sram.mem.klass == region->mem_class);
            assert(devinfo->mem.sram.mem.instance == region->instance);
            assert(devinfo->mem.sram.mappable.size == region->total_size);
         }
         devinfo->mem.sram.mappable.free = region->total_size - region->used;
         break;
      }
      case XE_MEM_REGION_CLASS_VRAM: {
         if (!update) {
            devinfo->mem.vram.mem.klass = region->mem_class;
            devinfo->mem.vram.mem.instance = region->instance;
            devinfo->mem.vram.mappable.size = region->total_size;
         } else {
            assert(devinfo->mem.vram.mem.klass == region->mem_class);
            assert(devinfo->mem.vram.mem.instance == region->instance);
            assert(devinfo->mem.vram.mappable.size == region->total_size);
         }
         devinfo->mem.vram.mappable.free = region->total_size - region->used;
         break;
      }
      default:
         mesa_loge("Unhandled Xe memory class");
         break;
      }
   }

   devinfo->mem.use_class_instance = true;
   free(regions);
   return true;
}

static bool
xe_query_gts(int fd, struct intel_device_info *devinfo)
{
   struct drm_xe_query_gts *gts;
   gts = xe_query_alloc_fetch(fd, DRM_XE_DEVICE_QUERY_GTS, NULL);
   if (!gts)
      return false;

   for (uint32_t i = 0; i < gts->num_gt; i++) {
      if (gts->gts[i].type == XE_QUERY_GT_TYPE_MAIN)
         devinfo->timestamp_frequency = gts->gts[i].clock_freq;
   }

   free(gts);
   return true;
}

bool
intel_device_info_xe_get_info_from_fd(int fd, struct intel_device_info *devinfo)
{
   if (!intel_device_info_xe_query_regions(fd, devinfo, false))
      return false;

   if (!xe_query_config(fd, devinfo))
      return false;

   if (!xe_query_gts(fd, devinfo))
      return false;

   devinfo->has_context_isolation = true;
   devinfo->has_mmap_offset = true;
   devinfo->has_caching_uapi = false;

   return true;
}
