/*
 * Copyright © 2018 Google, Inc.
 * Copyright © 2015 Intel Corporation
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

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include "vk_util.h"

#include "drm-uapi/msm_drm.h"
#include "util/timespec.h"
#include "util/os_time.h"
#include "util/perf/u_trace.h"

#include "tu_private.h"

#include "tu_cs.h"

struct tu_queue_submit
{
   struct vk_queue_submit *vk_submit;
   struct tu_u_trace_cmd_data *cmd_buffer_trace_data;

   struct drm_msm_gem_submit_cmd *cmds;
   struct drm_msm_gem_submit_syncobj *in_syncobjs;
   struct drm_msm_gem_submit_syncobj *out_syncobjs;

   uint32_t nr_in_syncobjs;
   uint32_t nr_out_syncobjs;
   uint32_t entry_count;
   uint32_t perf_pass_index;
};

struct tu_u_trace_syncobj
{
   uint32_t msm_queue_id;
   uint32_t fence;
};

static int
tu_drm_get_param(const struct tu_physical_device *dev,
                 uint32_t param,
                 uint64_t *value)
{
   /* Technically this requires a pipe, but the kernel only supports one pipe
    * anyway at the time of writing and most of these are clearly pipe
    * independent. */
   struct drm_msm_param req = {
      .pipe = MSM_PIPE_3D0,
      .param = param,
   };

   int ret = drmCommandWriteRead(dev->local_fd, DRM_MSM_GET_PARAM, &req,
                                 sizeof(req));
   if (ret)
      return ret;

   *value = req.value;

   return 0;
}

static int
tu_drm_get_gpu_id(const struct tu_physical_device *dev, uint32_t *id)
{
   uint64_t value;
   int ret = tu_drm_get_param(dev, MSM_PARAM_GPU_ID, &value);
   if (ret)
      return ret;

   *id = value;
   return 0;
}

static int
tu_drm_get_gmem_size(const struct tu_physical_device *dev, uint32_t *size)
{
   uint64_t value;
   int ret = tu_drm_get_param(dev, MSM_PARAM_GMEM_SIZE, &value);
   if (ret)
      return ret;

   *size = value;
   return 0;
}

static int
tu_drm_get_gmem_base(const struct tu_physical_device *dev, uint64_t *base)
{
   return tu_drm_get_param(dev, MSM_PARAM_GMEM_BASE, base);
}

int
tu_drm_get_timestamp(struct tu_physical_device *device, uint64_t *ts)
{
   return tu_drm_get_param(device, MSM_PARAM_TIMESTAMP, ts);
}

int
tu_drm_submitqueue_new(const struct tu_device *dev,
                       int priority,
                       uint32_t *queue_id)
{
   struct drm_msm_submitqueue req = {
      .flags = 0,
      .prio = priority,
   };

   int ret = drmCommandWriteRead(dev->fd,
                                 DRM_MSM_SUBMITQUEUE_NEW, &req, sizeof(req));
   if (ret)
      return ret;

   *queue_id = req.id;
   return 0;
}

void
tu_drm_submitqueue_close(const struct tu_device *dev, uint32_t queue_id)
{
   drmCommandWrite(dev->fd, DRM_MSM_SUBMITQUEUE_CLOSE,
                   &queue_id, sizeof(uint32_t));
}

static void
tu_gem_close(const struct tu_device *dev, uint32_t gem_handle)
{
   struct drm_gem_close req = {
      .handle = gem_handle,
   };

   drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &req);
}

/** Helper for DRM_MSM_GEM_INFO, returns 0 on error. */
static uint64_t
tu_gem_info(const struct tu_device *dev, uint32_t gem_handle, uint32_t info)
{
   struct drm_msm_gem_info req = {
      .handle = gem_handle,
      .info = info,
   };

   int ret = drmCommandWriteRead(dev->fd,
                                 DRM_MSM_GEM_INFO, &req, sizeof(req));
   if (ret < 0)
      return 0;

   return req.value;
}

static VkResult
tu_bo_init(struct tu_device *dev,
           struct tu_bo *bo,
           uint32_t gem_handle,
           uint64_t size,
           bool dump)
{
   uint64_t iova = tu_gem_info(dev, gem_handle, MSM_INFO_GET_IOVA);
   if (!iova) {
      tu_gem_close(dev, gem_handle);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   *bo = (struct tu_bo) {
      .gem_handle = gem_handle,
      .size = size,
      .iova = iova,
   };

   mtx_lock(&dev->bo_mutex);
   uint32_t idx = dev->bo_count++;

   /* grow the bo list if needed */
   if (idx >= dev->bo_list_size) {
      uint32_t new_len = idx + 64;
      struct drm_msm_gem_submit_bo *new_ptr =
         vk_realloc(&dev->vk.alloc, dev->bo_list, new_len * sizeof(*dev->bo_list),
                    8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!new_ptr)
         goto fail_bo_list;

      dev->bo_list = new_ptr;
      dev->bo_list_size = new_len;
   }

   /* grow the "bo idx" list (maps gem handles to index in the bo list) */
   if (bo->gem_handle >= dev->bo_idx_size) {
      uint32_t new_len = bo->gem_handle + 256;
      uint32_t *new_ptr =
         vk_realloc(&dev->vk.alloc, dev->bo_idx, new_len * sizeof(*dev->bo_idx),
                    8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!new_ptr)
         goto fail_bo_idx;

      dev->bo_idx = new_ptr;
      dev->bo_idx_size = new_len;
   }

   dev->bo_idx[bo->gem_handle] = idx;
   dev->bo_list[idx] = (struct drm_msm_gem_submit_bo) {
      .flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE |
               COND(dump, MSM_SUBMIT_BO_DUMP),
      .handle = gem_handle,
      .presumed = iova,
   };
   mtx_unlock(&dev->bo_mutex);

   return VK_SUCCESS;

fail_bo_idx:
   vk_free(&dev->vk.alloc, dev->bo_list);
fail_bo_list:
   tu_gem_close(dev, gem_handle);
   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult
tu_bo_init_new(struct tu_device *dev, struct tu_bo *bo, uint64_t size,
               enum tu_bo_alloc_flags flags)
{
   /* TODO: Choose better flags. As of 2018-11-12, freedreno/drm/msm_bo.c
    * always sets `flags = MSM_BO_WC`, and we copy that behavior here.
    */
   struct drm_msm_gem_new req = {
      .size = size,
      .flags = MSM_BO_WC
   };

   if (flags & TU_BO_ALLOC_GPU_READ_ONLY)
      req.flags |= MSM_BO_GPU_READONLY;

   int ret = drmCommandWriteRead(dev->fd,
                                 DRM_MSM_GEM_NEW, &req, sizeof(req));
   if (ret)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   return tu_bo_init(dev, bo, req.handle, size, flags & TU_BO_ALLOC_ALLOW_DUMP);
}

VkResult
tu_bo_init_dmabuf(struct tu_device *dev,
                  struct tu_bo *bo,
                  uint64_t size,
                  int prime_fd)
{
   /* lseek() to get the real size */
   off_t real_size = lseek(prime_fd, 0, SEEK_END);
   lseek(prime_fd, 0, SEEK_SET);
   if (real_size < 0 || (uint64_t) real_size < size)
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   uint32_t gem_handle;
   int ret = drmPrimeFDToHandle(dev->fd, prime_fd,
                                &gem_handle);
   if (ret)
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   return tu_bo_init(dev, bo, gem_handle, size, false);
}

int
tu_bo_export_dmabuf(struct tu_device *dev, struct tu_bo *bo)
{
   int prime_fd;
   int ret = drmPrimeHandleToFD(dev->fd, bo->gem_handle,
                                DRM_CLOEXEC, &prime_fd);

   return ret == 0 ? prime_fd : -1;
}

VkResult
tu_bo_map(struct tu_device *dev, struct tu_bo *bo)
{
   if (bo->map)
      return VK_SUCCESS;

   uint64_t offset = tu_gem_info(dev, bo->gem_handle, MSM_INFO_GET_OFFSET);
   if (!offset)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   /* TODO: Should we use the wrapper os_mmap() like Freedreno does? */
   void *map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    dev->fd, offset);
   if (map == MAP_FAILED)
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);

   bo->map = map;
   return VK_SUCCESS;
}

void
tu_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   assert(bo->gem_handle);

   if (bo->map)
      munmap(bo->map, bo->size);

   mtx_lock(&dev->bo_mutex);
   uint32_t idx = dev->bo_idx[bo->gem_handle];
   dev->bo_count--;
   dev->bo_list[idx] = dev->bo_list[dev->bo_count];
   dev->bo_idx[dev->bo_list[idx].handle] = idx;
   mtx_unlock(&dev->bo_mutex);

   tu_gem_close(dev, bo->gem_handle);
}

static VkResult
tu_drm_device_init(struct tu_physical_device *device,
                   struct tu_instance *instance,
                   drmDevicePtr drm_device)
{
   const char *path = drm_device->nodes[DRM_NODE_RENDER];
   VkResult result = VK_SUCCESS;
   drmVersionPtr version;
   int fd;
   int master_fd = -1;

   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      return vk_startup_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                               "failed to open device %s", path);
   }

   /* Version 1.6 added SYNCOBJ support. */
   const int min_version_major = 1;
   const int min_version_minor = 6;

   version = drmGetVersion(fd);
   if (!version) {
      close(fd);
      return vk_startup_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                               "failed to query kernel driver version for device %s",
                               path);
   }

   if (strcmp(version->name, "msm")) {
      drmFreeVersion(version);
      close(fd);
      return vk_startup_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                               "device %s does not use the msm kernel driver",
                               path);
   }

   if (version->version_major != min_version_major ||
       version->version_minor < min_version_minor) {
      result = vk_startup_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                                 "kernel driver for device %s has version %d.%d, "
                                 "but Vulkan requires version >= %d.%d",
                                 path,
                                 version->version_major, version->version_minor,
                                 min_version_major, min_version_minor);
      drmFreeVersion(version);
      close(fd);
      return result;
   }

   device->msm_major_version = version->version_major;
   device->msm_minor_version = version->version_minor;

   drmFreeVersion(version);

   if (instance->debug_flags & TU_DEBUG_STARTUP)
      mesa_logi("Found compatible device '%s'.", path);

   device->instance = instance;

   if (instance->vk.enabled_extensions.KHR_display) {
      master_fd =
         open(drm_device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
      if (master_fd >= 0) {
         /* TODO: free master_fd is accel is not working? */
      }
   }

   device->master_fd = master_fd;
   device->local_fd = fd;

   if (tu_drm_get_gpu_id(device, &device->dev_id.gpu_id)) {
      result = vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                                 "could not get GPU ID");
      goto fail;
   }

   if (tu_drm_get_param(device, MSM_PARAM_CHIP_ID, &device->dev_id.chip_id)) {
      result = vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                                 "could not get CHIP ID");
      goto fail;
   }

   if (tu_drm_get_gmem_size(device, &device->gmem_size)) {
      result = vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                                "could not get GMEM size");
      goto fail;
   }

   if (tu_drm_get_gmem_base(device, &device->gmem_base)) {
      result = vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                                 "could not get GMEM size");
      goto fail;
   }

   device->syncobj_type = vk_drm_syncobj_get_type(fd);

   device->sync_types[0] = &device->syncobj_type;
   device->sync_types[1] = NULL;

   device->heap.size = tu_get_system_heap_size();
   device->heap.used = 0u;
   device->heap.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

   result = tu_physical_device_init(device, instance);
   device->vk.supported_sync_types = device->sync_types;

   if (result == VK_SUCCESS)
       return result;

fail:
   close(fd);
   if (master_fd != -1)
      close(master_fd);
   return result;
}

VkResult
tu_enumerate_devices(struct tu_instance *instance)
{
   /* TODO: Check for more devices ? */
   drmDevicePtr devices[8];
   VkResult result = VK_ERROR_INCOMPATIBLE_DRIVER;
   int max_devices;

   instance->physical_device_count = 0;

   max_devices = drmGetDevices2(0, devices, ARRAY_SIZE(devices));

   if (instance->debug_flags & TU_DEBUG_STARTUP) {
      if (max_devices < 0)
         mesa_logi("drmGetDevices2 returned error: %s\n", strerror(max_devices));
      else
         mesa_logi("Found %d drm nodes", max_devices);
   }

   if (max_devices < 1)
      return vk_startup_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                               "No DRM devices found");

   for (unsigned i = 0; i < (unsigned) max_devices; i++) {
      if (devices[i]->available_nodes & 1 << DRM_NODE_RENDER &&
          devices[i]->bustype == DRM_BUS_PLATFORM) {

         result = tu_drm_device_init(
            instance->physical_devices + instance->physical_device_count,
            instance, devices[i]);
         if (result == VK_SUCCESS)
            ++instance->physical_device_count;
         else if (result != VK_ERROR_INCOMPATIBLE_DRIVER)
            break;
      }
   }
   drmFreeDevices(devices, max_devices);

   return result;
}

static VkResult
tu_queue_submit_create_locked(struct tu_queue *queue,
                              struct vk_queue_submit *vk_submit,
                              const uint32_t nr_in_syncobjs,
                              const uint32_t nr_out_syncobjs,
                              uint32_t perf_pass_index,
                              struct tu_queue_submit **submit)
{
   VkResult result;

   struct tu_queue_submit *new_submit = vk_zalloc(&queue->device->vk.alloc,
               sizeof(*new_submit), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (new_submit == NULL) {
      result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_new_submit;
   }

   bool u_trace_enabled = u_trace_context_actively_tracing(&queue->device->trace_context);
   bool has_trace_points = false;

   struct vk_command_buffer **vk_cmd_buffers = vk_submit->command_buffers;
   struct tu_cmd_buffer **cmd_buffers = (void *)vk_cmd_buffers;

   uint32_t entry_count = 0;
   for (uint32_t j = 0; j < vk_submit->command_buffer_count; ++j) {
      struct tu_cmd_buffer *cmdbuf = cmd_buffers[j];

      if (perf_pass_index != ~0)
         entry_count++;

      entry_count += cmdbuf->cs.entry_count;

      if (u_trace_enabled && u_trace_has_points(&cmdbuf->trace)) {
         if (!(cmdbuf->usage_flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))
            entry_count++;

         has_trace_points = true;
      }
   }

   new_submit->cmds = vk_zalloc(&queue->device->vk.alloc,
         entry_count * sizeof(*new_submit->cmds), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (new_submit->cmds == NULL) {
      result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_cmds;
   }

   if (has_trace_points) {
      new_submit->cmd_buffer_trace_data = vk_zalloc(&queue->device->vk.alloc,
            vk_submit->command_buffer_count * sizeof(struct tu_u_trace_cmd_data),
            8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

      if (new_submit->cmd_buffer_trace_data == NULL) {
         result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
         goto fail_cmd_trace_data;
      }

      for (uint32_t i = 0; i < vk_submit->command_buffer_count; ++i) {
         struct tu_cmd_buffer *cmdbuf = cmd_buffers[i];

         if (!(cmdbuf->usage_flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) &&
             u_trace_has_points(&cmdbuf->trace)) {
            /* A single command buffer could be submitted several times, but we
             * already backed timestamp iova addresses and trace points are
             * single-use. Therefor we have to copy trace points and create
             * a new timestamp buffer on every submit of reusable command buffer.
             */
            if (tu_create_copy_timestamp_cs(cmdbuf,
                  &new_submit->cmd_buffer_trace_data[i].timestamp_copy_cs,
                  &new_submit->cmd_buffer_trace_data[i].trace) != VK_SUCCESS) {
               result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
               goto fail_copy_timestamp_cs;
            }
            assert(new_submit->cmd_buffer_trace_data[i].timestamp_copy_cs->entry_count == 1);
         } else {
            new_submit->cmd_buffer_trace_data[i].trace = &cmdbuf->trace;
         }
      }
   }

   /* Allocate without wait timeline semaphores */
   new_submit->in_syncobjs = vk_zalloc(&queue->device->vk.alloc,
         nr_in_syncobjs * sizeof(*new_submit->in_syncobjs), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (new_submit->in_syncobjs == NULL) {
      result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_in_syncobjs;
   }

   /* Allocate with signal timeline semaphores considered */
   new_submit->out_syncobjs = vk_zalloc(&queue->device->vk.alloc,
         nr_out_syncobjs * sizeof(*new_submit->out_syncobjs), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (new_submit->out_syncobjs == NULL) {
      result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_out_syncobjs;
   }

   new_submit->entry_count = entry_count;
   new_submit->nr_in_syncobjs = nr_in_syncobjs;
   new_submit->nr_out_syncobjs = nr_out_syncobjs;
   new_submit->perf_pass_index = perf_pass_index;
   new_submit->vk_submit = vk_submit;

   *submit = new_submit;

   return VK_SUCCESS;

fail_out_syncobjs:
   vk_free(&queue->device->vk.alloc, new_submit->in_syncobjs);
fail_in_syncobjs:
   if (new_submit->cmd_buffer_trace_data)
      tu_u_trace_cmd_data_finish(queue->device, new_submit->cmd_buffer_trace_data,
                                 new_submit->vk_submit->command_buffer_count);
fail_copy_timestamp_cs:
   vk_free(&queue->device->vk.alloc, new_submit->cmd_buffer_trace_data);
fail_cmd_trace_data:
   vk_free(&queue->device->vk.alloc, new_submit->cmds);
fail_cmds:
   vk_free(&queue->device->vk.alloc, new_submit);
fail_new_submit:
   return result;
}

static void
tu_queue_build_msm_gem_submit_cmds(struct tu_queue *queue,
                                   struct tu_queue_submit *submit)
{
   struct drm_msm_gem_submit_cmd *cmds = submit->cmds;

   struct vk_command_buffer **vk_cmd_buffers = submit->vk_submit->command_buffers;
   struct tu_cmd_buffer **cmd_buffers = (void *)vk_cmd_buffers;

   uint32_t entry_idx = 0;
   for (uint32_t j = 0; j < submit->vk_submit->command_buffer_count; ++j) {
      struct tu_device *dev = queue->device;
      struct tu_cmd_buffer *cmdbuf = cmd_buffers[j];
      struct tu_cs *cs = &cmdbuf->cs;

      if (submit->perf_pass_index != ~0) {
         struct tu_cs_entry *perf_cs_entry =
            &dev->perfcntrs_pass_cs_entries[submit->perf_pass_index];

         cmds[entry_idx].type = MSM_SUBMIT_CMD_BUF;
         cmds[entry_idx].submit_idx =
            dev->bo_idx[perf_cs_entry->bo->gem_handle];
         cmds[entry_idx].submit_offset = perf_cs_entry->offset;
         cmds[entry_idx].size = perf_cs_entry->size;
         cmds[entry_idx].pad = 0;
         cmds[entry_idx].nr_relocs = 0;
         cmds[entry_idx++].relocs = 0;
      }

      for (unsigned i = 0; i < cs->entry_count; ++i, ++entry_idx) {
         cmds[entry_idx].type = MSM_SUBMIT_CMD_BUF;
         cmds[entry_idx].submit_idx =
            dev->bo_idx[cs->entries[i].bo->gem_handle];
         cmds[entry_idx].submit_offset = cs->entries[i].offset;
         cmds[entry_idx].size = cs->entries[i].size;
         cmds[entry_idx].pad = 0;
         cmds[entry_idx].nr_relocs = 0;
         cmds[entry_idx].relocs = 0;
      }

      if (submit->cmd_buffer_trace_data) {
         struct tu_cs *ts_cs = submit->cmd_buffer_trace_data[j].timestamp_copy_cs;
         if (ts_cs) {
            cmds[entry_idx].type = MSM_SUBMIT_CMD_BUF;
            cmds[entry_idx].submit_idx =
               queue->device->bo_idx[ts_cs->entries[0].bo->gem_handle];

            assert(cmds[entry_idx].submit_idx < queue->device->bo_count);

            cmds[entry_idx].submit_offset = ts_cs->entries[0].offset;
            cmds[entry_idx].size = ts_cs->entries[0].size;
            cmds[entry_idx].pad = 0;
            cmds[entry_idx].nr_relocs = 0;
            cmds[entry_idx++].relocs = 0;
         }
      }
   }
}

static VkResult
tu_queue_submit_locked(struct tu_queue *queue, struct tu_queue_submit *submit)
{
   queue->device->submit_count++;

#if HAVE_PERFETTO
   tu_perfetto_submit(queue->device, queue->device->submit_count);
#endif

   uint32_t flags = MSM_PIPE_3D0;

   if (submit->vk_submit->wait_count)
      flags |= MSM_SUBMIT_SYNCOBJ_IN;

   if (submit->vk_submit->signal_count)
      flags |= MSM_SUBMIT_SYNCOBJ_OUT;

   mtx_lock(&queue->device->bo_mutex);

   /* drm_msm_gem_submit_cmd requires index of bo which could change at any
    * time when bo_mutex is not locked. So we build submit cmds here the real
    * place to submit.
    */
   tu_queue_build_msm_gem_submit_cmds(queue, submit);

   struct drm_msm_gem_submit req = {
      .flags = flags,
      .queueid = queue->msm_queue_id,
      .bos = (uint64_t)(uintptr_t) queue->device->bo_list,
      .nr_bos = queue->device->bo_count,
      .cmds = (uint64_t)(uintptr_t)submit->cmds,
      .nr_cmds = submit->entry_count,
      .in_syncobjs = (uint64_t)(uintptr_t)submit->in_syncobjs,
      .out_syncobjs = (uint64_t)(uintptr_t)submit->out_syncobjs,
      .nr_in_syncobjs = submit->nr_in_syncobjs,
      .nr_out_syncobjs = submit->nr_out_syncobjs,
      .syncobj_stride = sizeof(struct drm_msm_gem_submit_syncobj),
   };

   int ret = drmCommandWriteRead(queue->device->fd,
                                 DRM_MSM_GEM_SUBMIT,
                                 &req, sizeof(req));

   mtx_unlock(&queue->device->bo_mutex);

   if (ret)
      return vk_device_set_lost(&queue->device->vk, "submit failed: %m");

   if (submit->cmd_buffer_trace_data) {
      struct tu_u_trace_flush_data *flush_data =
         vk_alloc(&queue->device->vk.alloc, sizeof(struct tu_u_trace_flush_data),
               8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      flush_data->submission_id = queue->device->submit_count;
      flush_data->syncobj =
         vk_alloc(&queue->device->vk.alloc, sizeof(struct tu_u_trace_syncobj),
               8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      flush_data->syncobj->fence = req.fence;
      flush_data->syncobj->msm_queue_id = queue->msm_queue_id;

      flush_data->cmd_trace_data = submit->cmd_buffer_trace_data;
      flush_data->trace_count = submit->vk_submit->command_buffer_count;
      submit->cmd_buffer_trace_data = NULL;

      for (uint32_t i = 0; i < submit->vk_submit->command_buffer_count; i++) {
         bool free_data = i == (submit->vk_submit->command_buffer_count - 1);
         u_trace_flush(flush_data->cmd_trace_data[i].trace, flush_data, free_data);
      }
   }

   return VK_SUCCESS;
}

static inline void
get_abs_timeout(struct drm_msm_timespec *tv, uint64_t ns)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   tv->tv_sec = t.tv_sec + ns / 1000000000;
   tv->tv_nsec = t.tv_nsec + ns % 1000000000;
}
VkResult
tu_device_wait_u_trace(struct tu_device *dev, struct tu_u_trace_syncobj *syncobj)
{
   struct drm_msm_wait_fence req = {
      .fence = syncobj->fence,
      .queueid = syncobj->msm_queue_id,
   };
   int ret;

   get_abs_timeout(&req.timeout, 1000000000);

   ret = drmCommandWrite(dev->fd, DRM_MSM_WAIT_FENCE, &req, sizeof(req));
   if (ret && (ret != -ETIMEDOUT)) {
      fprintf(stderr, "wait-fence failed! %d (%s)", ret, strerror(errno));
      return VK_TIMEOUT;
   }

   return VK_SUCCESS;
}

VkResult
tu_queue_submit(struct vk_queue *vk_queue, struct vk_queue_submit *submit)
{
   struct tu_queue *queue = container_of(vk_queue, struct tu_queue, vk);
   uint32_t perf_pass_index = queue->device->perfcntrs_pass_cs ?
                              submit->perf_pass_index : ~0;
   struct tu_queue_submit *submit_req = NULL;

   pthread_mutex_lock(&queue->device->submit_mutex);

   VkResult ret = tu_queue_submit_create_locked(queue, submit,
         submit->wait_count, submit->signal_count,
         perf_pass_index, &submit_req);

   if (ret != VK_SUCCESS) {
      pthread_mutex_unlock(&queue->device->submit_mutex);
      return ret;
   }

   /* note: assuming there won't be any very large semaphore counts */
   struct drm_msm_gem_submit_syncobj *in_syncobjs = submit_req->in_syncobjs;
   struct drm_msm_gem_submit_syncobj *out_syncobjs = submit_req->out_syncobjs;

   uint32_t nr_in_syncobjs = 0, nr_out_syncobjs = 0;

   for (uint32_t i = 0; i < submit->wait_count; i++) {
      struct vk_sync *sync = submit->waits[i].sync;

      if (vk_sync_type_is_drm_syncobj(sync->type)) {
         struct vk_drm_syncobj *syncobj = vk_sync_as_drm_syncobj(sync);

         in_syncobjs[nr_in_syncobjs++] = (struct drm_msm_gem_submit_syncobj) {
            .handle = syncobj->syncobj,
            .flags = 0,
         };
      }
   }

   for (uint32_t i = 0; i < submit->signal_count; i++) {
      struct vk_sync *sync = submit->signals[i].sync;

      if (vk_sync_type_is_drm_syncobj(sync->type)) {
         struct vk_drm_syncobj *syncobj = vk_sync_as_drm_syncobj(sync);

         out_syncobjs[nr_out_syncobjs++] = (struct drm_msm_gem_submit_syncobj) {
            .handle = syncobj->syncobj,
            .flags = 0,
         };
      }
   }

   ret = tu_queue_submit_locked(queue, submit_req);

   pthread_mutex_unlock(&queue->device->submit_mutex);
   if (ret != VK_SUCCESS)
       return ret;

   return VK_SUCCESS;
}

VkResult
tu_signal_syncs(struct tu_device *device,
                struct vk_sync *sync1, struct vk_sync *sync2)
{
   VkResult ret = VK_SUCCESS;

   if (sync1) {
      ret = vk_sync_signal(&device->vk, sync1, 0);

      if (ret != VK_SUCCESS)
         return ret;
   }

   if (sync2)
      ret = vk_sync_signal(&device->vk, sync2, 0);

   return ret;
}

int
tu_syncobj_to_fd(struct tu_device *device, struct vk_sync *sync)
{
   VkResult ret;
   int fd;
   ret = vk_sync_export_opaque_fd(&device->vk, sync, &fd);
   return ret ? -1 : fd;
}
