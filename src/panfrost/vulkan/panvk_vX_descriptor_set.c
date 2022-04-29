/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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

#include "genxml/gen_macros.h"

#include "panvk_private.h"

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "util/mesa-sha1.h"
#include "vk_descriptors.h"
#include "vk_util.h"

#include "pan_bo.h"
#include "panvk_cs.h"

#define PANVK_DESCRIPTOR_ALIGN 16

struct panvk_bview_desc {
   uint32_t elems;
};

static void
panvk_fill_bview_desc(struct panvk_bview_desc *desc,
                      struct panvk_buffer_view *view)
{
   desc->elems = view->elems;
}

struct panvk_image_desc {
   uint32_t width;
   uint32_t height;
   uint32_t depth;
   uint16_t levels;
   uint16_t samples;
};

static void
panvk_fill_image_desc(struct panvk_image_desc *desc,
                      struct panvk_image_view *view)
{
   desc->width = view->vk.extent.width;
   desc->height = view->vk.extent.height;
   desc->depth = view->vk.extent.depth;
   desc->levels = view->vk.level_count;
   desc->samples = view->vk.image->samples;

   /* Stick array layer count after the last valid size component */
   if (view->vk.image->image_type == VK_IMAGE_TYPE_1D)
      desc->height = view->vk.layer_count;
   else if (view->vk.image->image_type == VK_IMAGE_TYPE_2D)
      desc->depth = view->vk.layer_count;
}

VkResult
panvk_per_arch(CreateDescriptorSetLayout)(VkDevice _device,
                                          const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                          const VkAllocationCallbacks *pAllocator,
                                          VkDescriptorSetLayout *pSetLayout)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_descriptor_set_layout *set_layout;
   VkDescriptorSetLayoutBinding *bindings = NULL;
   unsigned num_bindings = 0;
   VkResult result;

   if (pCreateInfo->bindingCount) {
      result =
         vk_create_sorted_bindings(pCreateInfo->pBindings,
                                   pCreateInfo->bindingCount,
                                   &bindings);
      if (result != VK_SUCCESS)
         return vk_error(device, result);

      num_bindings = bindings[pCreateInfo->bindingCount - 1].binding + 1;
   }

   unsigned num_immutable_samplers = 0;
   for (unsigned i = 0; i < pCreateInfo->bindingCount; i++) {
      if (bindings[i].pImmutableSamplers)
         num_immutable_samplers += bindings[i].descriptorCount;
   }

   size_t size = sizeof(*set_layout) +
                 (sizeof(struct panvk_descriptor_set_binding_layout) *
                  num_bindings) +
                 (sizeof(struct panvk_sampler *) * num_immutable_samplers);
   set_layout = vk_object_zalloc(&device->vk, NULL, size,
                                 VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT);
   if (!set_layout) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto err_free_bindings;
   }

   struct panvk_sampler **immutable_samplers =
      (struct panvk_sampler **)((uint8_t *)set_layout + sizeof(*set_layout) +
                                (sizeof(struct panvk_descriptor_set_binding_layout) *
                                 num_bindings));

   set_layout->flags = pCreateInfo->flags;
   set_layout->binding_count = num_bindings;

   unsigned sampler_idx = 0, tex_idx = 0, ubo_idx = 0;
   unsigned dyn_ubo_idx = 0, dyn_ssbo_idx = 0, desc_idx = 0, img_idx = 0;
   uint32_t desc_ubo_size = 0;

   for (unsigned i = 0; i < pCreateInfo->bindingCount; i++) {
      const VkDescriptorSetLayoutBinding *binding = &bindings[i];
      struct panvk_descriptor_set_binding_layout *binding_layout =
         &set_layout->bindings[binding->binding];

      binding_layout->type = binding->descriptorType;
      binding_layout->array_size = binding->descriptorCount;
      binding_layout->shader_stages = binding->stageFlags;
      binding_layout->desc_ubo_stride = 0;
      if (binding->pImmutableSamplers) {
         binding_layout->immutable_samplers = immutable_samplers;
         immutable_samplers += binding_layout->array_size;
         for (unsigned j = 0; j < binding_layout->array_size; j++) {
            VK_FROM_HANDLE(panvk_sampler, sampler, binding->pImmutableSamplers[j]);
            binding_layout->immutable_samplers[j] = sampler;
         }
      }

      binding_layout->desc_idx = desc_idx;
      desc_idx += binding->descriptorCount;
      switch (binding_layout->type) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
         binding_layout->sampler_idx = sampler_idx;
         sampler_idx += binding_layout->array_size;
         break;
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         binding_layout->sampler_idx = sampler_idx;
         binding_layout->tex_idx = tex_idx;
         sampler_idx += binding_layout->array_size;
         tex_idx += binding_layout->array_size;
         binding_layout->desc_ubo_stride = sizeof(struct panvk_image_desc);
         break;
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         binding_layout->tex_idx = tex_idx;
         tex_idx += binding_layout->array_size;
         binding_layout->desc_ubo_stride = sizeof(struct panvk_image_desc);
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
         binding_layout->tex_idx = tex_idx;
         tex_idx += binding_layout->array_size;
         binding_layout->desc_ubo_stride = sizeof(struct panvk_bview_desc);
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         binding_layout->dyn_ubo_idx = dyn_ubo_idx;
         dyn_ubo_idx += binding_layout->array_size;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         binding_layout->ubo_idx = ubo_idx;
         ubo_idx += binding_layout->array_size;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         binding_layout->dyn_ssbo_idx = dyn_ssbo_idx;
         dyn_ssbo_idx += binding_layout->array_size;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         binding_layout->desc_ubo_stride = sizeof(struct panvk_ssbo_addr);
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         binding_layout->img_idx = img_idx;
         img_idx += binding_layout->array_size;
         binding_layout->desc_ubo_stride = sizeof(struct panvk_image_desc);
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         binding_layout->img_idx = img_idx;
         img_idx += binding_layout->array_size;
         binding_layout->desc_ubo_stride = sizeof(struct panvk_bview_desc);
         break;
      default:
         unreachable("Invalid descriptor type");
      }

      desc_ubo_size = ALIGN_POT(desc_ubo_size, PANVK_DESCRIPTOR_ALIGN);
      binding_layout->desc_ubo_offset = desc_ubo_size;
      desc_ubo_size += binding_layout->desc_ubo_stride *
                       binding_layout->array_size;
   }

   set_layout->desc_ubo_size = desc_ubo_size;
   if (desc_ubo_size > 0)
      set_layout->desc_ubo_index = ubo_idx++;

   set_layout->num_descs = desc_idx;
   set_layout->num_samplers = sampler_idx;
   set_layout->num_textures = tex_idx;
   set_layout->num_ubos = ubo_idx;
   set_layout->num_dyn_ubos = dyn_ubo_idx;
   set_layout->num_dyn_ssbos = dyn_ssbo_idx;
   set_layout->num_imgs = img_idx;
   p_atomic_set(&set_layout->refcount, 1);

   free(bindings);
   *pSetLayout = panvk_descriptor_set_layout_to_handle(set_layout);
   return VK_SUCCESS;

err_free_bindings:
   free(bindings);
   return vk_error(device, result);
}

static VkResult
panvk_per_arch(descriptor_set_create)(struct panvk_device *device,
                                      struct panvk_descriptor_pool *pool,
                                      const struct panvk_descriptor_set_layout *layout,
                                      struct panvk_descriptor_set **out_set)
{
   struct panvk_descriptor_set *set;

   /* TODO: Allocate from the pool! */
   set = vk_object_zalloc(&device->vk, NULL,
                          sizeof(struct panvk_descriptor_set),
                          VK_OBJECT_TYPE_DESCRIPTOR_SET);
   if (!set)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   set->layout = layout;
   set->descs = vk_alloc(&device->vk.alloc,
                         sizeof(*set->descs) * layout->num_descs, 8,
                         VK_OBJECT_TYPE_DESCRIPTOR_SET);
   if (!set->descs)
      goto err_free_set;

   if (layout->num_ubos) {
      set->ubos = vk_zalloc(&device->vk.alloc,
                            pan_size(UNIFORM_BUFFER) * layout->num_ubos, 8,
                            VK_OBJECT_TYPE_DESCRIPTOR_SET);
      if (!set->ubos)
         goto err_free_set;
   }

   if (layout->num_dyn_ubos) {
      set->dyn_ubos = vk_zalloc(&device->vk.alloc,
                            sizeof(*set->dyn_ubos) * layout->num_dyn_ubos, 8,
                            VK_OBJECT_TYPE_DESCRIPTOR_SET);
      if (!set->dyn_ubos)
         goto err_free_set;
   }

   if (layout->num_dyn_ssbos) {
      set->dyn_ssbos = vk_zalloc(&device->vk.alloc,
                            sizeof(*set->dyn_ssbos) * layout->num_dyn_ssbos, 8,
                            VK_OBJECT_TYPE_DESCRIPTOR_SET);
      if (!set->dyn_ssbos)
         goto err_free_set;
   }

   if (layout->num_samplers) {
      set->samplers = vk_zalloc(&device->vk.alloc,
                                pan_size(SAMPLER) * layout->num_samplers, 8,
                                VK_OBJECT_TYPE_DESCRIPTOR_SET);
      if (!set->samplers)
         goto err_free_set;
   }

   if (layout->num_textures) {
      set->textures =
         vk_zalloc(&device->vk.alloc,
                   (PAN_ARCH >= 6 ? pan_size(TEXTURE) : sizeof(mali_ptr)) *
                   layout->num_textures,
                   8, VK_OBJECT_TYPE_DESCRIPTOR_SET);
      if (!set->textures)
         goto err_free_set;
   }

   if (layout->num_imgs) {
      set->img_fmts =
         vk_zalloc(&device->vk.alloc,
                   sizeof(*set->img_fmts) * layout->num_imgs,
                   8, VK_OBJECT_TYPE_DESCRIPTOR_SET);
      if (!set->img_fmts)
         goto err_free_set;

      set->img_attrib_bufs =
         vk_zalloc(&device->vk.alloc,
                   pan_size(ATTRIBUTE_BUFFER) * 2 * layout->num_imgs,
                   8, VK_OBJECT_TYPE_DESCRIPTOR_SET);
      if (!set->img_attrib_bufs)
         goto err_free_set;
   }

   for (unsigned i = 0; i < layout->binding_count; i++) {
      if (!layout->bindings[i].immutable_samplers)
         continue;

      for (unsigned j = 0; j < layout->bindings[i].array_size; j++) {
         set->descs[layout->bindings[i].desc_idx].image.sampler =
            layout->bindings[i].immutable_samplers[j];
      }
   }

   if (layout->desc_ubo_size) {
      set->desc_bo = panfrost_bo_create(&device->physical_device->pdev,
                                        layout->desc_ubo_size,
                                        0, "Descriptor set");
      if (!set->desc_bo)
         goto err_free_set;

      struct mali_uniform_buffer_packed *ubos = set->ubos;

      panvk_per_arch(emit_ubo)(set->desc_bo->ptr.gpu,
                               layout->desc_ubo_size,
                               &ubos[layout->desc_ubo_index]);
   }

   *out_set = set;
   return VK_SUCCESS;

err_free_set:
   vk_free(&device->vk.alloc, set->textures);
   vk_free(&device->vk.alloc, set->samplers);
   vk_free(&device->vk.alloc, set->ubos);
   vk_free(&device->vk.alloc, set->dyn_ubos);
   vk_free(&device->vk.alloc, set->dyn_ssbos);
   vk_free(&device->vk.alloc, set->img_fmts);
   vk_free(&device->vk.alloc, set->img_attrib_bufs);
   vk_free(&device->vk.alloc, set->descs);
   if (set->desc_bo)
      panfrost_bo_unreference(set->desc_bo);
   vk_object_free(&device->vk, NULL, set);
   return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
}

VkResult
panvk_per_arch(AllocateDescriptorSets)(VkDevice _device,
                                       const VkDescriptorSetAllocateInfo *pAllocateInfo,
                                       VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_descriptor_pool, pool, pAllocateInfo->descriptorPool);
   VkResult result;
   unsigned i;

   for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      VK_FROM_HANDLE(panvk_descriptor_set_layout, layout,
                     pAllocateInfo->pSetLayouts[i]);
      struct panvk_descriptor_set *set = NULL;

      result = panvk_per_arch(descriptor_set_create)(device, pool, layout, &set);
      if (result != VK_SUCCESS)
         goto err_free_sets;

      pDescriptorSets[i] = panvk_descriptor_set_to_handle(set);
   }

   return VK_SUCCESS;

err_free_sets:
   panvk_FreeDescriptorSets(_device, pAllocateInfo->descriptorPool, i, pDescriptorSets);
   for (i = 0; i < pAllocateInfo->descriptorSetCount; i++)
      pDescriptorSets[i] = VK_NULL_HANDLE;

   return result; 
}

static void
panvk_set_buffer_desc(struct panvk_buffer_desc *bdesc,
                      const VkDescriptorBufferInfo *pBufferInfo)
{
   VK_FROM_HANDLE(panvk_buffer, buffer, pBufferInfo->buffer);

   bdesc->buffer = buffer;
   bdesc->offset = pBufferInfo->offset;
   bdesc->size = pBufferInfo->range;
}

static void
panvk_per_arch(set_ubo_desc)(void *ubo,
                             const VkDescriptorBufferInfo *pBufferInfo)
{
   VK_FROM_HANDLE(panvk_buffer, buffer, pBufferInfo->buffer);
   mali_ptr ptr = panvk_buffer_gpu_ptr(buffer, pBufferInfo->offset);
   size_t size = panvk_buffer_range(buffer, pBufferInfo->offset,
                                    pBufferInfo->range);
   panvk_per_arch(emit_ubo)(ptr, size, ubo);
}

static void
panvk_set_ssbo_desc(struct panvk_descriptor_set *set,
                    const struct panvk_descriptor_set_binding_layout *binding_layout,
                    uint32_t idx, const VkDescriptorBufferInfo *pBufferInfo)
{
   VK_FROM_HANDLE(panvk_buffer, buffer, pBufferInfo->buffer);

   void *desc = (char *)set->desc_bo->ptr.cpu +
                binding_layout->desc_ubo_offset +
                binding_layout->desc_ubo_stride * idx;

   *(struct panvk_ssbo_addr *)desc = (struct panvk_ssbo_addr) {
      .base_addr = panvk_buffer_gpu_ptr(buffer, pBufferInfo->offset),
      .size = panvk_buffer_range(buffer, pBufferInfo->offset,
                                         pBufferInfo->range),
   };
}

static void
panvk_set_sampler_desc(void *desc,
                       const VkDescriptorImageInfo *pImageInfo)
{
   VK_FROM_HANDLE(panvk_sampler, sampler, pImageInfo->sampler);

   memcpy(desc, &sampler->desc, sizeof(sampler->desc));
}

static void
panvk_set_tex_desc(struct panvk_descriptor_set *set,
                   const struct panvk_descriptor_set_binding_layout *binding_layout,
                   unsigned elem,
                   const VkDescriptorImageInfo *pImageInfo)
{
   VK_FROM_HANDLE(panvk_image_view, view, pImageInfo->imageView);

   unsigned tex_idx = binding_layout->tex_idx + elem;

#if PAN_ARCH >= 6
   memcpy(&((struct mali_texture_packed *)set->textures)[tex_idx],
          view->descs.tex, pan_size(TEXTURE));
#else
   ((mali_ptr *)set->textures)[tex_idx] = view->bo->ptr.gpu;
#endif

   void *desc = (char *)set->desc_bo->ptr.cpu +
                binding_layout->desc_ubo_offset +
                elem * binding_layout->desc_ubo_stride;

   panvk_fill_image_desc(desc, view);
}

static void
panvk_set_tex_buf_desc(struct panvk_descriptor_set *set,
                       const struct panvk_descriptor_set_binding_layout *binding_layout,
                       unsigned elem,
                       const VkBufferView bufferView)
{
   VK_FROM_HANDLE(panvk_buffer_view, view, bufferView);

   unsigned tex_idx = binding_layout->tex_idx + elem;

#if PAN_ARCH >= 6
   memcpy(&((struct mali_texture_packed *)set->textures)[tex_idx],
          view->descs.tex, pan_size(TEXTURE));
#else
   ((mali_ptr *)set->textures)[tex_idx] = view->bo->ptr.gpu;
#endif

   void *desc = (char *)set->desc_bo->ptr.cpu +
                binding_layout->desc_ubo_offset +
                elem * binding_layout->desc_ubo_stride;

   panvk_fill_bview_desc(desc, view);
}

static void
panvk_set_img_desc(struct panvk_device *dev,
                   struct panvk_descriptor_set *set,
                   const struct panvk_descriptor_set_binding_layout *binding_layout,
                   unsigned elem,
                   const VkDescriptorImageInfo *pImageInfo)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   VK_FROM_HANDLE(panvk_image_view, view, pImageInfo->imageView);

   unsigned img_idx = binding_layout->img_idx + elem;

   void *attrib_buf = (uint8_t *)set->img_attrib_bufs +
                      (pan_size(ATTRIBUTE_BUFFER) * 2 * img_idx);

   set->img_fmts[img_idx] = pdev->formats[view->pview.format].hw;
   memcpy(attrib_buf, view->descs.img_attrib_buf, pan_size(ATTRIBUTE_BUFFER) * 2);

   void *desc = (char *)set->desc_bo->ptr.cpu +
                binding_layout->desc_ubo_offset +
                elem * binding_layout->desc_ubo_stride;

   panvk_fill_image_desc(desc, view);
}

static void
panvk_set_img_buf_desc(struct panvk_device *dev,
                       struct panvk_descriptor_set *set,
                       const struct panvk_descriptor_set_binding_layout *binding_layout,
                       unsigned elem,
                       const VkBufferView bufferView)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   VK_FROM_HANDLE(panvk_buffer_view, view, bufferView);

   unsigned img_idx = binding_layout->img_idx + elem;

   void *attrib_buf = (uint8_t *)set->img_attrib_bufs +
                      (pan_size(ATTRIBUTE_BUFFER) * 2 * img_idx);

   set->img_fmts[img_idx] = pdev->formats[view->fmt].hw;
   memcpy(attrib_buf, view->descs.img_attrib_buf, pan_size(ATTRIBUTE_BUFFER) * 2);

   void *desc = (char *)set->desc_bo->ptr.cpu +
                binding_layout->desc_ubo_offset +
                elem * binding_layout->desc_ubo_stride;

   panvk_fill_bview_desc(desc, view);
}

static void
panvk_per_arch(write_descriptor_set)(struct panvk_device *dev,
                                     const VkWriteDescriptorSet *pDescriptorWrite)
{
   VK_FROM_HANDLE(panvk_descriptor_set, set, pDescriptorWrite->dstSet);
   const struct panvk_descriptor_set_layout *layout = set->layout;
   unsigned dest_offset = pDescriptorWrite->dstArrayElement;
   unsigned binding = pDescriptorWrite->dstBinding;
   struct mali_uniform_buffer_packed *ubos = set->ubos;
   struct mali_sampler_packed *samplers = set->samplers;
   unsigned src_offset = 0;

   while (src_offset < pDescriptorWrite->descriptorCount &&
          binding < layout->binding_count) {
      const struct panvk_descriptor_set_binding_layout *binding_layout =
         &layout->bindings[binding];

      if (!binding_layout->array_size) {
         binding++;
         dest_offset = 0;
         continue;
      }

      assert(pDescriptorWrite->descriptorType == binding_layout->type);
      unsigned ndescs = MIN2(pDescriptorWrite->descriptorCount - src_offset,
                             binding_layout->array_size - dest_offset);
      assert(binding_layout->desc_idx + dest_offset + ndescs <= set->layout->num_descs);

      switch (pDescriptorWrite->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
         for (unsigned i = 0; i < ndescs; i++) {
            const VkDescriptorImageInfo *info = &pDescriptorWrite->pImageInfo[src_offset + i];

            if ((pDescriptorWrite->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                 pDescriptorWrite->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
                !binding_layout->immutable_samplers) {
               unsigned sampler = binding_layout->sampler_idx + dest_offset + i;

               panvk_set_sampler_desc(&samplers[sampler], info);
            }

            if (pDescriptorWrite->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                pDescriptorWrite->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {

               panvk_set_tex_desc(set, binding_layout, dest_offset + i, info);
            }
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
         for (unsigned i = 0; i < ndescs; i++) {
            panvk_set_tex_buf_desc(set, binding_layout, dest_offset + i,
                                   pDescriptorWrite->pTexelBufferView[src_offset + i]);
         }
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         for (unsigned i = 0; i < ndescs; i++) {
            const VkDescriptorImageInfo *info = &pDescriptorWrite->pImageInfo[src_offset + i];
            panvk_set_img_desc(dev, set, binding_layout, dest_offset + i, info);
         }
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         for (unsigned i = 0; i < ndescs; i++) {
            panvk_set_img_buf_desc(dev, set, binding_layout, dest_offset + i,
                                   pDescriptorWrite->pTexelBufferView[src_offset + i]);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         for (unsigned i = 0; i < ndescs; i++) {
            unsigned ubo = binding_layout->ubo_idx + dest_offset + i;
            panvk_per_arch(set_ubo_desc)(&ubos[ubo],
                                         &pDescriptorWrite->pBufferInfo[src_offset + i]);
         }
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         for (unsigned i = 0; i < ndescs; i++) {
            unsigned ubo = binding_layout->dyn_ubo_idx + dest_offset + i;
            panvk_set_buffer_desc(&set->dyn_ubos[ubo], &pDescriptorWrite->pBufferInfo[src_offset + i]);
         }
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         for (unsigned i = 0; i < ndescs; i++) {
            panvk_set_ssbo_desc(set, binding_layout, i,
                                &pDescriptorWrite->pBufferInfo[src_offset + i]);
         }
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         for (unsigned i = 0; i < ndescs; i++) {
            unsigned ssbo = binding_layout->dyn_ssbo_idx + dest_offset + i;
            panvk_set_buffer_desc(&set->dyn_ssbos[ssbo], &pDescriptorWrite->pBufferInfo[src_offset + i]);
         }
         break;
      default:
         unreachable("Invalid type");
      }

      src_offset += ndescs;
      binding++;
      dest_offset = 0;
   }
}

static void
panvk_copy_descriptor_set(struct panvk_device *dev,
                          const VkCopyDescriptorSet *pDescriptorCopy)
{
   VK_FROM_HANDLE(panvk_descriptor_set, dest_set, pDescriptorCopy->dstSet);
   VK_FROM_HANDLE(panvk_descriptor_set, src_set, pDescriptorCopy->srcSet);
   const struct panvk_descriptor_set_layout *dest_layout = dest_set->layout;
   const struct panvk_descriptor_set_layout *src_layout = dest_set->layout;
   unsigned dest_offset = pDescriptorCopy->dstArrayElement;
   unsigned src_offset = pDescriptorCopy->srcArrayElement;
   unsigned dest_binding = pDescriptorCopy->dstBinding;
   unsigned src_binding = pDescriptorCopy->srcBinding;
   unsigned desc_count = pDescriptorCopy->descriptorCount;

   while (desc_count && src_binding < src_layout->binding_count &&
          dest_binding < dest_layout->binding_count) {
      const struct panvk_descriptor_set_binding_layout *dest_binding_layout =
         &src_layout->bindings[dest_binding];

      if (!dest_binding_layout->array_size) {
         dest_binding++;
         dest_offset = 0;
         continue;
      }

      const struct panvk_descriptor_set_binding_layout *src_binding_layout =
         &src_layout->bindings[src_binding];

      if (!src_binding_layout->array_size) {
         src_binding++;
         src_offset = 0;
         continue;
      }

      assert(dest_binding_layout->type == src_binding_layout->type);

      unsigned ndescs = MAX3(desc_count,
                             dest_binding_layout->array_size - dest_offset,
                             src_binding_layout->array_size - src_offset);

      struct panvk_descriptor *dest_descs = dest_set->descs + dest_binding_layout->desc_idx + dest_offset;
      struct panvk_descriptor *src_descs = src_set->descs + src_binding_layout->desc_idx + src_offset;
      memcpy(dest_descs, src_descs, ndescs * sizeof(*dest_descs));
      desc_count -= ndescs;
      dest_offset += ndescs;
      if (dest_offset == dest_binding_layout->array_size) {
         dest_binding++;
         dest_offset = 0;
         continue;
      }
      src_offset += ndescs;
      if (src_offset == src_binding_layout->array_size) {
         src_binding++;
         src_offset = 0;
         continue;
      }
   }

   assert(!desc_count);
}

void
panvk_per_arch(UpdateDescriptorSets)(VkDevice _device,
                                     uint32_t descriptorWriteCount,
                                     const VkWriteDescriptorSet *pDescriptorWrites,
                                     uint32_t descriptorCopyCount,
                                     const VkCopyDescriptorSet *pDescriptorCopies)
{
   VK_FROM_HANDLE(panvk_device, dev, _device);

   for (unsigned i = 0; i < descriptorWriteCount; i++)
      panvk_per_arch(write_descriptor_set)(dev, &pDescriptorWrites[i]);
   for (unsigned i = 0; i < descriptorCopyCount; i++)
      panvk_copy_descriptor_set(dev, &pDescriptorCopies[i]);
}
