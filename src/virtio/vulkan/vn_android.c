/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#include "vn_android.h"

#include <dlfcn.h>
#include <drm/drm_fourcc.h>
#include <hardware/gralloc.h>
#include <hardware/hwvulkan.h>
#include <vndk/hardware_buffer.h>
#include <vulkan/vk_icd.h>

#include "util/libsync.h"
#include "util/os_file.h"

#include "vn_device.h"
#include "vn_device_memory.h"
#include "vn_image.h"
#include "vn_queue.h"

static int
vn_hal_open(const struct hw_module_t *mod,
            const char *id,
            struct hw_device_t **dev);

static void UNUSED
static_asserts(void)
{
   STATIC_ASSERT(HWVULKAN_DISPATCH_MAGIC == ICD_LOADER_MAGIC);
}

PUBLIC struct hwvulkan_module_t HAL_MODULE_INFO_SYM = {
   .common = {
      .tag = HARDWARE_MODULE_TAG,
      .module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
      .hal_api_version = HARDWARE_HAL_API_VERSION,
      .id = HWVULKAN_HARDWARE_MODULE_ID,
      .name = "Venus Vulkan HAL",
      .author = "Google LLC",
      .methods = &(hw_module_methods_t) {
         .open = vn_hal_open,
      },
   },
};

static const gralloc_module_t *gralloc = NULL;

static int
vn_hal_close(UNUSED struct hw_device_t *dev)
{
   dlclose(gralloc->common.dso);
   return 0;
}

static hwvulkan_device_t vn_hal_dev = {
  .common = {
     .tag = HARDWARE_DEVICE_TAG,
     .version = HWVULKAN_DEVICE_API_VERSION_0_1,
     .module = &HAL_MODULE_INFO_SYM.common,
     .close = vn_hal_close,
  },
 .EnumerateInstanceExtensionProperties = vn_EnumerateInstanceExtensionProperties,
 .CreateInstance = vn_CreateInstance,
 .GetInstanceProcAddr = vn_GetInstanceProcAddr,
};

static int
vn_hal_open(const struct hw_module_t *mod,
            const char *id,
            struct hw_device_t **dev)
{
   static const char CROS_GRALLOC_MODULE_NAME[] = "CrOS Gralloc";

   assert(mod == &HAL_MODULE_INFO_SYM.common);
   assert(strcmp(id, HWVULKAN_DEVICE_0) == 0);

   /* get gralloc module for gralloc buffer info query */
   int ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                           (const hw_module_t **)&gralloc);
   if (ret) {
      if (VN_DEBUG(WSI))
         vn_log(NULL, "failed to open gralloc module(ret=%d)", ret);
      return ret;
   }

   if (VN_DEBUG(WSI))
      vn_log(NULL, "opened gralloc module name: %s", gralloc->common.name);

   if (strcmp(gralloc->common.name, CROS_GRALLOC_MODULE_NAME) != 0 ||
       !gralloc->perform) {
      dlclose(gralloc->common.dso);
      return -1;
   }

   *dev = &vn_hal_dev.common;

   return 0;
}

static uint32_t
vn_android_ahb_format_from_vk_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
      return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
   case VK_FORMAT_R8G8B8_UNORM:
      return AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM;
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
      return AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
   case VK_FORMAT_R16G16B16A16_SFLOAT:
      return AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT;
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
   case VK_FORMAT_D16_UNORM:
      return AHARDWAREBUFFER_FORMAT_D16_UNORM;
   case VK_FORMAT_X8_D24_UNORM_PACK32:
      return AHARDWAREBUFFER_FORMAT_D24_UNORM;
   case VK_FORMAT_D24_UNORM_S8_UINT:
      return AHARDWAREBUFFER_FORMAT_D24_UNORM_S8_UINT;
   case VK_FORMAT_D32_SFLOAT:
      return AHARDWAREBUFFER_FORMAT_D32_FLOAT;
   case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return AHARDWAREBUFFER_FORMAT_D32_FLOAT_S8_UINT;
   case VK_FORMAT_S8_UINT:
      return AHARDWAREBUFFER_FORMAT_S8_UINT;
   case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
      return AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
   default:
      return 0;
   }
}

VkFormat
vn_android_ahb_format_to_vk_format(uint32_t format)
{
   switch (format) {
   case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
   case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
      return VK_FORMAT_R8G8B8A8_UNORM;
   case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
      return VK_FORMAT_R8G8B8_UNORM;
   case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
      return VK_FORMAT_R5G6B5_UNORM_PACK16;
   case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
   case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   case AHARDWAREBUFFER_FORMAT_D16_UNORM:
      return VK_FORMAT_D16_UNORM;
   case AHARDWAREBUFFER_FORMAT_D24_UNORM:
      return VK_FORMAT_X8_D24_UNORM_PACK32;
   case AHARDWAREBUFFER_FORMAT_D24_UNORM_S8_UINT:
      return VK_FORMAT_D24_UNORM_S8_UINT;
   case AHARDWAREBUFFER_FORMAT_D32_FLOAT:
      return VK_FORMAT_D32_SFLOAT;
   case AHARDWAREBUFFER_FORMAT_D32_FLOAT_S8_UINT:
      return VK_FORMAT_D32_SFLOAT_S8_UINT;
   case AHARDWAREBUFFER_FORMAT_S8_UINT:
      return VK_FORMAT_S8_UINT;
   case AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420:
      return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   default:
      return VK_FORMAT_UNDEFINED;
   }
}

uint64_t
vn_android_get_ahb_usage(const VkImageUsageFlags usage,
                         const VkImageCreateFlags flags)
{
   uint64_t ahb_usage = 0;
   if (usage &
       (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
      ahb_usage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

   if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
      ahb_usage |= AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;

   if (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
      ahb_usage |= AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP;

   if (flags & VK_IMAGE_CREATE_PROTECTED_BIT)
      ahb_usage |= AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT;

   /* must include at least one GPU usage flag */
   if (ahb_usage == 0)
      ahb_usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

   return ahb_usage;
}

VkResult
vn_GetSwapchainGrallocUsage2ANDROID(
   VkDevice device,
   VkFormat format,
   VkImageUsageFlags imageUsage,
   VkSwapchainImageUsageFlagsANDROID swapchainImageUsage,
   uint64_t *grallocConsumerUsage,
   uint64_t *grallocProducerUsage)
{
   struct vn_device *dev = vn_device_from_handle(device);
   *grallocConsumerUsage = 0;
   *grallocProducerUsage = 0;

   if (swapchainImageUsage & VK_SWAPCHAIN_IMAGE_USAGE_SHARED_BIT_ANDROID)
      return vn_error(dev->instance, VK_ERROR_INITIALIZATION_FAILED);

   if (VN_DEBUG(WSI))
      vn_log(dev->instance, "format=%d, imageUsage=0x%x", format, imageUsage);

   if (imageUsage & (VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
      *grallocProducerUsage |= AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;

   if (imageUsage &
       (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
      *grallocConsumerUsage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

   return VK_SUCCESS;
}

struct cros_gralloc0_buffer_info {
   uint32_t drm_fourcc; /* ignored */
   int num_fds;         /* ignored */
   int fds[4];          /* ignored */
   uint64_t modifier;
   uint32_t offset[4];
   uint32_t stride[4];
};

static VkResult
vn_android_get_dma_buf_from_native_handle(const native_handle_t *handle,
                                          int *out_dma_buf)
{
   /* There can be multiple fds wrapped inside a native_handle_t, but we
    * expect only the 1st one points to the dma_buf. For multi-planar format,
    * there should only exist one dma_buf as well. The other fd(s) may point
    * to shared memory used to store buffer metadata or other vendor specific
    * bits.
    */
   if (handle->numFds < 1) {
      vn_log(NULL, "handle->numFds is %d, expected >= 1", handle->numFds);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   if (handle->data[0] < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   *out_dma_buf = handle->data[0];
   return VK_SUCCESS;
}

static VkResult
vn_android_get_mem_type_bits_from_dma_buf(VkDevice device,
                                          int dma_buf,
                                          uint32_t *out_mem_type_bits)
{
   VkMemoryFdPropertiesKHR fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      .pNext = NULL,
      .memoryTypeBits = 0,
   };
   VkResult result = vn_GetMemoryFdPropertiesKHR(
      device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dma_buf,
      &fd_props);
   if (result != VK_SUCCESS)
      return result;

   if (!fd_props.memoryTypeBits)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   *out_mem_type_bits = fd_props.memoryTypeBits;

   return VK_SUCCESS;
}

static bool
vn_android_get_gralloc_buffer_info(buffer_handle_t handle,
                                   uint32_t out_strides[4],
                                   uint32_t out_offsets[4],
                                   uint64_t *out_format_modifier)
{
   static const int32_t CROS_GRALLOC_DRM_GET_BUFFER_INFO = 4;
   struct cros_gralloc0_buffer_info info;
   if (gralloc->perform(gralloc, CROS_GRALLOC_DRM_GET_BUFFER_INFO, handle,
                        &info) != 0)
      return false;

   if (info.modifier == DRM_FORMAT_MOD_INVALID)
      return false;

   for (uint32_t i = 0; i < 4; i++) {
      out_strides[i] = info.stride[i];
      out_offsets[i] = info.offset[i];
   }
   *out_format_modifier = info.modifier;

   return true;
}

static VkResult
vn_android_get_modifier_properties(VkPhysicalDevice physical_device,
                                   VkFormat format,
                                   uint64_t modifier,
                                   const VkAllocationCallbacks *alloc,
                                   VkDrmFormatModifierPropertiesEXT *out_props)
{
   VkDrmFormatModifierPropertiesListEXT mod_prop_list = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
      .pNext = NULL,
      .drmFormatModifierCount = 0,
      .pDrmFormatModifierProperties = NULL,
   };
   VkFormatProperties2 format_prop = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      .pNext = &mod_prop_list,
   };
   vn_GetPhysicalDeviceFormatProperties2(physical_device, format,
                                         &format_prop);

   if (!mod_prop_list.drmFormatModifierCount)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   VkDrmFormatModifierPropertiesEXT *mod_props =
      vk_zalloc(alloc,
                sizeof(VkDrmFormatModifierPropertiesEXT) *
                   mod_prop_list.drmFormatModifierCount,
                VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!mod_props)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   mod_prop_list.pDrmFormatModifierProperties = mod_props;
   vn_GetPhysicalDeviceFormatProperties2(physical_device, format,
                                         &format_prop);

   for (uint32_t i = 0; i < mod_prop_list.drmFormatModifierCount; i++) {
      if (mod_props[i].drmFormatModifier == modifier) {
         *out_props = mod_props[i];
         break;
      }
   }

   vk_free(alloc, mod_props);
   return VK_SUCCESS;
}

VkResult
vn_android_image_from_anb(struct vn_device *dev,
                          const VkImageCreateInfo *image_info,
                          const VkNativeBufferANDROID *anb_info,
                          const VkAllocationCallbacks *alloc,
                          struct vn_image **out_img)
{
   /* If anb_info->handle points to a classic resouce created from
    * virtio_gpu_cmd_resource_create_3d, anb_info->stride is the stride of the
    * guest shadow storage other than the host gpu storage.
    *
    * We also need to pass the correct stride to vn_CreateImage, which will be
    * done via VkImageDrmFormatModifierExplicitCreateInfoEXT and will require
    * VK_EXT_image_drm_format_modifier support in the host driver. The struct
    * needs host storage info which can be queried from cros gralloc.
    */
   VkResult result = VK_SUCCESS;
   VkDevice device = vn_device_to_handle(dev);
   VkPhysicalDevice physical_device =
      vn_physical_device_to_handle(dev->physical_device);
   VkDeviceMemory memory = VK_NULL_HANDLE;
   VkImage image = VK_NULL_HANDLE;
   struct vn_image *img = NULL;
   uint32_t mem_type_bits = 0;
   int dma_buf_fd = -1;
   int dup_fd = -1;
   uint32_t strides[4] = { 0, 0, 0, 0 };
   uint32_t offsets[4] = { 0, 0, 0, 0 };
   uint64_t format_modifier = 0;

   result = vn_android_get_dma_buf_from_native_handle(anb_info->handle,
                                                      &dma_buf_fd);
   if (result != VK_SUCCESS)
      goto fail;

   if (!vn_android_get_gralloc_buffer_info(anb_info->handle, strides, offsets,
                                           &format_modifier)) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   }

   VkDrmFormatModifierPropertiesEXT mod_props;
   result =
      vn_android_get_modifier_properties(physical_device, image_info->format,
                                         format_modifier, alloc, &mod_props);
   if (result != VK_SUCCESS)
      goto fail;

   /* TODO support multi-planar format */
   if (mod_props.drmFormatModifierPlaneCount != 1) {
      if (VN_DEBUG(WSI))
         vn_log(dev->instance, "plane count is %d, expected 1",
                mod_props.drmFormatModifierPlaneCount);
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   }

   const VkSubresourceLayout layout = {
      .offset = offsets[0],
      .size = 0,
      .rowPitch = strides[0],
      .arrayPitch = 0,
      .depthPitch = 0,
   };
   const VkImageDrmFormatModifierExplicitCreateInfoEXT drm_mod_info = {
      .sType =
         VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .pNext = image_info->pNext,
      .drmFormatModifier = format_modifier,
      .drmFormatModifierPlaneCount = 1,
      .pPlaneLayouts = &layout,
   };
   const VkExternalMemoryImageCreateInfo external_img_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &drm_mod_info,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   VkImageCreateInfo local_image_info = *image_info;
   local_image_info.pNext = &external_img_info;
   local_image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

   /* Force VK_SHARING_MODE_CONCURRENT if necessary.
    * For physical devices supporting multiple queue families, if a swapchain is
    * created with exclusive mode, we must transfer the image ownership into the
    * queue family of the present queue. However, there's no way to get that
    * queue at the 1st acquire of the image. Thus, when multiple queue families
    * are supported in a physical device, we include all queue families in the
    * image create info along with VK_SHARING_MODE_CONCURRENT, which forces us
    * to transfer the ownership into VK_QUEUE_FAMILY_IGNORED. Then if there's
    * only one queue family, we can safely use queue family index 0.
    */
   if (dev->physical_device->queue_family_count > 1) {
      local_image_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
      local_image_info.queueFamilyIndexCount =
         dev->physical_device->queue_family_count;
      local_image_info.pQueueFamilyIndices =
         dev->android_wsi->queue_family_indices;
   }

   /* encoder will strip the Android specific pNext structs */
   result = vn_image_create(dev, &local_image_info, alloc, &img);
   if (result != VK_SUCCESS)
      goto fail;

   image = vn_image_to_handle(img);

   result = vn_image_android_wsi_init(dev, img, alloc);
   if (result != VK_SUCCESS)
      goto fail;

   VkMemoryRequirements mem_req;
   vn_GetImageMemoryRequirements(device, image, &mem_req);
   if (!mem_req.memoryTypeBits) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   }

   result = vn_android_get_mem_type_bits_from_dma_buf(device, dma_buf_fd,
                                                      &mem_type_bits);
   if (result != VK_SUCCESS)
      goto fail;

   if (VN_DEBUG(WSI))
      vn_log(dev->instance, "memoryTypeBits = img(0x%X) & fd(0x%X)",
             mem_req.memoryTypeBits, mem_type_bits);

   mem_type_bits &= mem_req.memoryTypeBits;
   if (!mem_type_bits) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   }

   dup_fd = os_dupfd_cloexec(dma_buf_fd);
   if (dup_fd < 0) {
      result = (errno == EMFILE) ? VK_ERROR_TOO_MANY_OBJECTS
                                 : VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   const VkImportMemoryFdInfoKHR import_fd_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = NULL,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = dup_fd,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import_fd_info,
      .allocationSize = mem_req.size,
      .memoryTypeIndex = ffs(mem_type_bits) - 1,
   };
   result = vn_AllocateMemory(device, &memory_info, alloc, &memory);
   if (result != VK_SUCCESS) {
      /* only need to close the dup_fd on import failure */
      close(dup_fd);
      goto fail;
   }

   result = vn_BindImageMemory(device, image, memory, 0);
   if (result != VK_SUCCESS)
      goto fail;

   /* Android WSI image owns the memory */
   img->private_memory = memory;
   *out_img = img;

   return VK_SUCCESS;

fail:
   if (image != VK_NULL_HANDLE)
      vn_DestroyImage(device, image, alloc);
   if (memory != VK_NULL_HANDLE)
      vn_FreeMemory(device, memory, alloc);
   return vn_error(dev->instance, result);
}

static bool
vn_is_queue_compatible_with_wsi(struct vn_queue *queue)
{
   static const int32_t compatible_flags =
      VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
   return compatible_flags & queue->device->physical_device
                                ->queue_family_properties[queue->family]
                                .queueFamilyProperties.queueFlags;
}

VkResult
vn_AcquireImageANDROID(VkDevice device,
                       VkImage image,
                       int nativeFenceFd,
                       VkSemaphore semaphore,
                       VkFence fence)
{
   /* At this moment, out semaphore and fence are filled with already signaled
    * payloads, and the native fence fd is waited inside until signaled.
    */
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_semaphore *sem = vn_semaphore_from_handle(semaphore);
   struct vn_fence *fen = vn_fence_from_handle(fence);
   struct vn_image *img = vn_image_from_handle(image);
   struct vn_queue *queue = img->acquire_queue;

   if (nativeFenceFd >= 0) {
      int ret = sync_wait(nativeFenceFd, INT32_MAX);
      /* Android loader expects the ICD to always close the fd */
      close(nativeFenceFd);
      if (ret)
         return vn_error(dev->instance, VK_ERROR_SURFACE_LOST_KHR);
   }

   if (sem)
      vn_semaphore_signal_wsi(dev, sem);

   if (fen)
      vn_fence_signal_wsi(dev, fen);

   if (!queue) {
      /* pick a compatible queue for the 1st acquire of this image */
      for (uint32_t i = 0; i < dev->queue_count; i++) {
         if (vn_is_queue_compatible_with_wsi(&dev->queues[i])) {
            queue = &dev->queues[i];
            break;
         }
      }
   }
   if (!queue)
      return vn_error(dev->instance, VK_ERROR_UNKNOWN);

   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .pNext = NULL,
      .waitSemaphoreCount = 0,
      .pWaitSemaphores = NULL,
      .pWaitDstStageMask = NULL,
      .commandBufferCount = 1,
      .pCommandBuffers =
         &img->ownership_cmds[queue->family].cmds[VN_IMAGE_OWNERSHIP_ACQUIRE],
      .signalSemaphoreCount = 0,
      .pSignalSemaphores = NULL,
   };

   VkResult result = vn_QueueSubmit(vn_queue_to_handle(queue), 1,
                                    &submit_info, queue->wait_fence);
   if (result != VK_SUCCESS)
      return vn_error(dev->instance, result);

   result =
      vn_WaitForFences(device, 1, &queue->wait_fence, VK_TRUE, UINT64_MAX);
   vn_ResetFences(device, 1, &queue->wait_fence);

   return vn_result(dev->instance, result);
}

VkResult
vn_QueueSignalReleaseImageANDROID(VkQueue queue,
                                  uint32_t waitSemaphoreCount,
                                  const VkSemaphore *pWaitSemaphores,
                                  VkImage image,
                                  int *pNativeFenceFd)
{
   /* At this moment, the wait semaphores are converted to a VkFence via an
    * empty submit. The VkFence is then waited inside until signaled, and the
    * out native fence fd is set to -1.
    */
   VkResult result = VK_SUCCESS;
   struct vn_queue *que = vn_queue_from_handle(queue);
   struct vn_image *img = vn_image_from_handle(image);
   const VkAllocationCallbacks *alloc = &que->device->base.base.alloc;
   VkDevice device = vn_device_to_handle(que->device);
   VkPipelineStageFlags local_stage_masks[8];
   VkPipelineStageFlags *stage_masks = local_stage_masks;

   if (!vn_is_queue_compatible_with_wsi(que))
      return vn_error(que->device->instance, VK_ERROR_UNKNOWN);

   if (waitSemaphoreCount > ARRAY_SIZE(local_stage_masks)) {
      stage_masks =
         vk_alloc(alloc, sizeof(VkPipelineStageFlags) * waitSemaphoreCount,
                  VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!stage_masks) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto out;
      }
   }

   for (uint32_t i = 0; i < waitSemaphoreCount; i++)
      stage_masks[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .pNext = NULL,
      .waitSemaphoreCount = waitSemaphoreCount,
      .pWaitSemaphores = pWaitSemaphores,
      .pWaitDstStageMask = stage_masks,
      .commandBufferCount = 1,
      .pCommandBuffers =
         &img->ownership_cmds[que->family].cmds[VN_IMAGE_OWNERSHIP_RELEASE],
      .signalSemaphoreCount = 0,
      .pSignalSemaphores = NULL,
   };
   result = vn_QueueSubmit(queue, 1, &submit_info, que->wait_fence);
   if (stage_masks != local_stage_masks)
      vk_free(alloc, stage_masks);
   if (result != VK_SUCCESS)
      goto out;

   result =
      vn_WaitForFences(device, 1, &que->wait_fence, VK_TRUE, UINT64_MAX);
   vn_ResetFences(device, 1, &que->wait_fence);

   img->acquire_queue = que;

out:
   *pNativeFenceFd = -1;
   return result;
}

VkResult
vn_android_wsi_init(struct vn_device *dev, const VkAllocationCallbacks *alloc)
{
   VkResult result = VK_SUCCESS;

   struct vn_android_wsi *android_wsi =
      vk_zalloc(alloc, sizeof(struct vn_android_wsi), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!android_wsi)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   const uint32_t count = dev->physical_device->queue_family_count;
   if (count > 1) {
      android_wsi->queue_family_indices =
         vk_alloc(alloc, sizeof(uint32_t) * count, VN_DEFAULT_ALIGN,
                  VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!android_wsi->queue_family_indices) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }

      for (uint32_t i = 0; i < count; i++)
         android_wsi->queue_family_indices[i] = i;
   }

   android_wsi->cmd_pools =
      vk_zalloc(alloc, sizeof(VkCommandPool) * count, VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!android_wsi->cmd_pools) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   VkDevice device = vn_device_to_handle(dev);
   for (uint32_t i = 0; i < count; i++) {
      const VkCommandPoolCreateInfo cmd_pool_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .pNext = NULL,
         .flags = 0,
         .queueFamilyIndex = i,
      };
      result = vn_CreateCommandPool(device, &cmd_pool_info, alloc,
                                    &android_wsi->cmd_pools[i]);
      if (result != VK_SUCCESS)
         goto fail;
   }

   mtx_init(&android_wsi->cmd_pools_lock, mtx_plain);

   dev->android_wsi = android_wsi;

   return VK_SUCCESS;

fail:
   if (android_wsi->cmd_pools) {
      for (uint32_t i = 0; i < count; i++) {
         if (android_wsi->cmd_pools[i] != VK_NULL_HANDLE)
            vn_DestroyCommandPool(device, android_wsi->cmd_pools[i], alloc);
      }
      vk_free(alloc, android_wsi->cmd_pools);
   }

   if (android_wsi->queue_family_indices)
      vk_free(alloc, android_wsi->queue_family_indices);

   vk_free(alloc, android_wsi);

   return vn_error(dev->instance, result);
}

void
vn_android_wsi_fini(struct vn_device *dev, const VkAllocationCallbacks *alloc)
{
   if (!dev->android_wsi)
      return;

   mtx_destroy(&dev->android_wsi->cmd_pools_lock);

   VkDevice device = vn_device_to_handle(dev);
   for (uint32_t i = 0; i < dev->physical_device->queue_family_count; i++) {
      vn_DestroyCommandPool(device, dev->android_wsi->cmd_pools[i], alloc);
   }
   vk_free(alloc, dev->android_wsi->cmd_pools);

   if (dev->android_wsi->queue_family_indices)
      vk_free(alloc, dev->android_wsi->queue_family_indices);

   vk_free(alloc, dev->android_wsi);
}

static VkResult
vn_android_get_ahb_format_properties(
   struct vn_device *dev,
   const struct AHardwareBuffer *ahb,
   VkAndroidHardwareBufferFormatPropertiesANDROID *out_props)
{
   VkPhysicalDevice physical_device =
      vn_physical_device_to_handle(dev->physical_device);

   AHardwareBuffer_Desc desc;
   AHardwareBuffer_describe(ahb, &desc);

   /* AHB usage must include at least one GPU bit for image or buffer */
   if (!(desc.usage & (AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                       AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                       AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER)))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   /* We implement AHB extension support with EXT_image_drm_format_modifier.
    * It requires us to have a compatible VkFormat but not DRM formats. So if
    * the ahb is not intended for backing a VkBuffer, error out early if the
    * format is VK_FORMAT_UNDEFINED.
    */
   VkFormat format = vn_android_ahb_format_to_vk_format(desc.format);
   if (format == VK_FORMAT_UNDEFINED) {
      if (desc.format != AHARDWAREBUFFER_FORMAT_BLOB)
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;

      out_props->format = format;
      out_props->externalFormat = desc.format;
      return VK_SUCCESS;
   }

   uint32_t strides[4] = { 0, 0, 0, 0 };
   uint32_t offsets[4] = { 0, 0, 0, 0 };
   uint64_t format_modifier = 0;
   const native_handle_t *handle = AHardwareBuffer_getNativeHandle(ahb);
   if (!vn_android_get_gralloc_buffer_info(handle, strides, offsets,
                                           &format_modifier))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   VkDrmFormatModifierPropertiesEXT mod_props;
   VkResult result = vn_android_get_modifier_properties(
      physical_device, format, format_modifier, &dev->base.base.alloc,
      &mod_props);
   if (result != VK_SUCCESS)
      return result;

   /* The spec requires that formatFeatures must include at least one of
    * VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT or
    * VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT.
    */
   const VkFormatFeatureFlags format_features =
      mod_props.drmFormatModifierTilingFeatures |
      VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;
   *out_props = (VkAndroidHardwareBufferFormatPropertiesANDROID) {
      .sType = out_props->sType,
      .pNext = out_props->pNext,
      .format = format,
      .externalFormat = desc.format,
      .formatFeatures = format_features,
      .samplerYcbcrConversionComponents = {
         .r = VK_COMPONENT_SWIZZLE_IDENTITY,
         .g = VK_COMPONENT_SWIZZLE_IDENTITY,
         .b = VK_COMPONENT_SWIZZLE_IDENTITY,
         .a = VK_COMPONENT_SWIZZLE_IDENTITY,
      },
      .suggestedYcbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601,
      .suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
      .suggestedXChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
      .suggestedYChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
   };

   return VK_SUCCESS;
}

VkResult
vn_GetAndroidHardwareBufferPropertiesANDROID(
   VkDevice device,
   const struct AHardwareBuffer *buffer,
   VkAndroidHardwareBufferPropertiesANDROID *pProperties)
{
   struct vn_device *dev = vn_device_from_handle(device);
   VkResult result = VK_SUCCESS;
   int dma_buf_fd = -1;
   uint32_t mem_type_bits = 0;

   VkAndroidHardwareBufferFormatPropertiesANDROID *format_props =
      vk_find_struct(pProperties->pNext,
                     ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID);
   if (format_props) {
      result =
         vn_android_get_ahb_format_properties(dev, buffer, format_props);
      if (result != VK_SUCCESS)
         return vn_error(dev->instance, result);
   }

   const native_handle_t *handle = AHardwareBuffer_getNativeHandle(buffer);
   result = vn_android_get_dma_buf_from_native_handle(handle, &dma_buf_fd);
   if (result != VK_SUCCESS)
      return vn_error(dev->instance, result);

   result = vn_android_get_mem_type_bits_from_dma_buf(device, dma_buf_fd,
                                                      &mem_type_bits);
   if (result != VK_SUCCESS)
      return vn_error(dev->instance, result);

   pProperties->allocationSize = lseek(dma_buf_fd, 0, SEEK_END);
   pProperties->memoryTypeBits = mem_type_bits;

   return VK_SUCCESS;
}

static AHardwareBuffer *
vn_android_ahb_allocate(uint32_t width,
                        uint32_t height,
                        uint32_t layers,
                        uint32_t format,
                        uint64_t usage)
{
   AHardwareBuffer *ahb = NULL;
   AHardwareBuffer_Desc desc;
   int ret = 0;

   memset(&desc, 0, sizeof(desc));
   desc.width = width;
   desc.height = height;
   desc.layers = layers;
   desc.format = format;
   desc.usage = usage;

   ret = AHardwareBuffer_allocate(&desc, &ahb);
   if (ret) {
      /* We just log the error code here for now since the platform falsely
       * maps all gralloc allocation failures to oom.
       */
      vn_log(NULL, "AHB alloc(w=%u,h=%u,l=%u,f=%u,u=%" PRIu64 ") failed(%d)",
             width, height, layers, format, usage, ret);
      return NULL;
   }

   return ahb;
}

bool
vn_android_get_drm_format_modifier_info(
   const VkPhysicalDeviceImageFormatInfo2 *format_info,
   VkPhysicalDeviceImageDrmFormatModifierInfoEXT *out_info)
{
   /* To properly fill VkPhysicalDeviceImageDrmFormatModifierInfoEXT, we have
    * to allocate an ahb to retrieve the drm format modifier. For the image
    * sharing mode, we assume VK_SHARING_MODE_EXCLUSIVE for now.
    */
   AHardwareBuffer *ahb = NULL;
   const native_handle_t *handle = NULL;
   uint32_t format = 0;
   uint64_t usage = 0;
   uint32_t strides[4] = { 0, 0, 0, 0 };
   uint32_t offsets[4] = { 0, 0, 0, 0 };
   uint64_t format_modifier = 0;

   assert(format_info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);

   format = vn_android_ahb_format_from_vk_format(format_info->format);
   if (!format)
      return false;

   usage = vn_android_get_ahb_usage(format_info->usage, format_info->flags);
   ahb = vn_android_ahb_allocate(16, 16, 1, format, usage);
   if (!ahb)
      return false;

   handle = AHardwareBuffer_getNativeHandle(ahb);
   if (!vn_android_get_gralloc_buffer_info(handle, strides, offsets,
                                           &format_modifier)) {
      AHardwareBuffer_release(ahb);
      return false;
   }

   *out_info = (VkPhysicalDeviceImageDrmFormatModifierInfoEXT){
      .sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
      .pNext = NULL,
      .drmFormatModifier = format_modifier,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = 0,
      .pQueueFamilyIndices = NULL,
   };

   AHardwareBuffer_release(ahb);
   return true;
}

VkResult
vn_android_image_from_ahb(struct vn_device *dev,
                          const VkImageCreateInfo *create_info,
                          const VkAllocationCallbacks *alloc,
                          struct vn_image **out_img)
{
   const VkExternalFormatANDROID *ext_info =
      vk_find_struct_const(create_info->pNext, EXTERNAL_FORMAT_ANDROID);

   VkImageCreateInfo local_info;
   if (ext_info && ext_info->externalFormat) {
      assert(create_info->format == VK_FORMAT_UNDEFINED);
      assert(create_info->imageType == VK_IMAGE_TYPE_2D);
      assert(create_info->usage == VK_IMAGE_USAGE_SAMPLED_BIT);
      assert(create_info->tiling == VK_IMAGE_TILING_OPTIMAL);

      local_info = *create_info;
      local_info.format =
         vn_android_ahb_format_to_vk_format(ext_info->externalFormat);
      create_info = &local_info;
   }

   return vn_image_create_deferred(dev, create_info, alloc, out_img);
}

VkResult
vn_android_device_import_ahb(struct vn_device *dev,
                             struct vn_device_memory *mem,
                             const VkMemoryAllocateInfo *alloc_info,
                             struct AHardwareBuffer *ahb)
{
   const VkMemoryDedicatedAllocateInfo *dedicated_info =
      vk_find_struct_const(alloc_info->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   const native_handle_t *handle = NULL;
   int dma_buf_fd = -1;
   int dup_fd = -1;
   VkDeviceSize alloc_size = alloc_info->allocationSize;
   VkResult result = VK_SUCCESS;

   handle = AHardwareBuffer_getNativeHandle(ahb);
   result = vn_android_get_dma_buf_from_native_handle(handle, &dma_buf_fd);
   if (result != VK_SUCCESS)
      return result;

   /* If ahb is for an image, finish the deferred image creation first */
   if (dedicated_info && dedicated_info->image != VK_NULL_HANDLE) {
      const VkAllocationCallbacks *alloc = &dev->base.base.alloc;
      struct vn_image *img = vn_image_from_handle(dedicated_info->image);
      VkImageCreateInfo *image_info = &img->deferred_info->create;
      uint32_t strides[4] = { 0, 0, 0, 0 };
      uint32_t offsets[4] = { 0, 0, 0, 0 };
      uint64_t format_modifier = 0;
      VkDrmFormatModifierPropertiesEXT mod_props;

      if (!vn_android_get_gralloc_buffer_info(handle, strides, offsets,
                                              &format_modifier))
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;

      result = vn_android_get_modifier_properties(
         vn_physical_device_to_handle(dev->physical_device),
         image_info->format, format_modifier, alloc, &mod_props);
      if (result != VK_SUCCESS)
         return result;

      /* XXX fix plane count > 1 case for external memory  */
      if (mod_props.drmFormatModifierPlaneCount != 1) {
         vn_log(dev->instance, "plane count is %d, expected 1",
                mod_props.drmFormatModifierPlaneCount);
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      }

      const VkSubresourceLayout layout = {
         .offset = offsets[0],
         .size = 0,
         .rowPitch = strides[0],
         .arrayPitch = 0,
         .depthPitch = 0,
      };
      const VkImageDrmFormatModifierExplicitCreateInfoEXT drm_mod_info = {
         .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
         .pNext = image_info->pNext,
         .drmFormatModifier = format_modifier,
         .drmFormatModifierPlaneCount = 1,
         .pPlaneLayouts = &layout,
      };
      const VkExternalMemoryImageCreateInfo external_img_info = {
         .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
         .pNext = &drm_mod_info,
         .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
      image_info->pNext = &external_img_info;
      image_info->tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
      result = vn_image_init_deferred(dev, image_info, img);
      if (result != VK_SUCCESS)
         return result;

      /* For AHB memory allocation of a dedicated image, allocationSize must
       * be zero from the app side. So we need to get the proper allocation
       * size here used to override memory allocation info.
       */
      VkMemoryRequirements mem_req;
      vn_GetImageMemoryRequirements(vn_device_to_handle(dev),
                                    dedicated_info->image, &mem_req);
      alloc_size = mem_req.size;
   }

   errno = 0;
   dup_fd = os_dupfd_cloexec(dma_buf_fd);
   if (dup_fd < 0)
      return (errno == EMFILE) ? VK_ERROR_TOO_MANY_OBJECTS
                               : VK_ERROR_OUT_OF_HOST_MEMORY;

   /* Spec requires AHB export info to be present, so we must strip it. In
    * practice, the AHB import path here only needs the main allocation info
    * and the dedicated_info.
    */
   VkMemoryDedicatedAllocateInfo local_dedicated_info;
   /* Override when dedicated_info exists and is not the tail struct. */
   if (dedicated_info && dedicated_info->pNext) {
      local_dedicated_info = *dedicated_info;
      local_dedicated_info.pNext = NULL;
      dedicated_info = &local_dedicated_info;
   }
   const VkMemoryAllocateInfo local_alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = dedicated_info,
      .allocationSize = alloc_size,
      .memoryTypeIndex = alloc_info->memoryTypeIndex,
   };
   result =
      vn_device_memory_import_dma_buf(dev, mem, &local_alloc_info, dup_fd);
   if (result != VK_SUCCESS) {
      close(dup_fd);
      return result;
   }

   AHardwareBuffer_acquire(ahb);
   mem->ahb = ahb;

   return VK_SUCCESS;
}

VkResult
vn_android_device_allocate_ahb(struct vn_device *dev,
                               struct vn_device_memory *mem,
                               const VkMemoryAllocateInfo *alloc_info)
{
   const VkMemoryDedicatedAllocateInfo *dedicated_info =
      vk_find_struct_const(alloc_info->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   uint32_t width = 0;
   uint32_t height = 1;
   uint32_t layers = 1;
   uint32_t format = 0;
   uint64_t usage = 0;
   struct AHardwareBuffer *ahb = NULL;

   if (dedicated_info && dedicated_info->image != VK_NULL_HANDLE) {
      const VkImageCreateInfo *image_info =
         &vn_image_from_handle(dedicated_info->image)->deferred_info->create;
      assert(image_info);
      width = image_info->extent.width;
      height = image_info->extent.height;
      layers = image_info->arrayLayers;
      format = vn_android_ahb_format_from_vk_format(image_info->format);
      /* TODO Need to further resolve the gralloc usage bits for image format
       * list info, which might involve disabling compression if there exists
       * no universally applied compression strategy across the formats.
       */
      usage = vn_android_get_ahb_usage(image_info->usage, image_info->flags);
   } else {
      width = alloc_info->allocationSize;
      format = AHARDWAREBUFFER_FORMAT_BLOB;
      /* TODO AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER is not supported by cros
       * gralloc. So here we work around with CPU usage bits for VkBuffer.
       */
      usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
              AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
   }

   ahb = vn_android_ahb_allocate(width, height, layers, format, usage);
   if (!ahb)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkResult result = vn_android_device_import_ahb(dev, mem, alloc_info, ahb);

   /* ahb alloc has already acquired a ref and import will acquire another,
    * must release one here to avoid leak.
    */
   AHardwareBuffer_release(ahb);

   return result;
}

void
vn_android_release_ahb(struct AHardwareBuffer *ahb)
{
   AHardwareBuffer_release(ahb);
}

VkResult
vn_GetMemoryAndroidHardwareBufferANDROID(
   VkDevice device,
   const VkMemoryGetAndroidHardwareBufferInfoANDROID *pInfo,
   struct AHardwareBuffer **pBuffer)
{
   struct vn_device_memory *mem = vn_device_memory_from_handle(pInfo->memory);

   AHardwareBuffer_acquire(mem->ahb);
   *pBuffer = mem->ahb;

   return VK_SUCCESS;
}
