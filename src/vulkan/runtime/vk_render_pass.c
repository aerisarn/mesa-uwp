/*
 * Copyright © 2020 Valve Corporation
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

#include "vk_render_pass.h"

#include "vk_alloc.h"
#include "vk_command_buffer.h"
#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_format.h"
#include "vk_framebuffer.h"
#include "vk_image.h"
#include "vk_util.h"

#include "util/log.h"

static void
translate_references(VkAttachmentReference2 **reference_ptr,
                     uint32_t reference_count,
                     const VkAttachmentReference *reference,
                     const VkRenderPassCreateInfo *pass_info,
                     bool is_input_attachment)
{
   VkAttachmentReference2 *reference2 = *reference_ptr;
   *reference_ptr += reference_count;
   for (uint32_t i = 0; i < reference_count; i++) {
      reference2[i] = (VkAttachmentReference2) {
         .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
         .pNext = NULL,
         .attachment = reference[i].attachment,
         .layout = reference[i].layout,
      };

      if (is_input_attachment &&
          reference2[i].attachment != VK_ATTACHMENT_UNUSED) {
         assert(reference2[i].attachment < pass_info->attachmentCount);
         const VkAttachmentDescription *att =
            &pass_info->pAttachments[reference2[i].attachment];
         reference2[i].aspectMask = vk_format_aspects(att->format);
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_CreateRenderPass(VkDevice _device,
                           const VkRenderPassCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkRenderPass *pRenderPass)
{
   VK_FROM_HANDLE(vk_device, device, _device);

   uint32_t reference_count = 0;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      reference_count += pCreateInfo->pSubpasses[i].inputAttachmentCount;
      reference_count += pCreateInfo->pSubpasses[i].colorAttachmentCount;
      if (pCreateInfo->pSubpasses[i].pResolveAttachments)
         reference_count += pCreateInfo->pSubpasses[i].colorAttachmentCount;
      if (pCreateInfo->pSubpasses[i].pDepthStencilAttachment)
         reference_count += 1;
   }

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, VkRenderPassCreateInfo2, create_info, 1);
   VK_MULTIALLOC_DECL(&ma, VkSubpassDescription2, subpasses,
                           pCreateInfo->subpassCount);
   VK_MULTIALLOC_DECL(&ma, VkAttachmentDescription2, attachments,
                           pCreateInfo->attachmentCount);
   VK_MULTIALLOC_DECL(&ma, VkSubpassDependency2, dependencies,
                           pCreateInfo->dependencyCount);
   VK_MULTIALLOC_DECL(&ma, VkAttachmentReference2, references,
                           reference_count);
   if (!vk_multialloc_alloc2(&ma, &device->alloc, pAllocator,
                             VK_SYSTEM_ALLOCATION_SCOPE_COMMAND))
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkAttachmentReference2 *reference_ptr = references;

   const VkRenderPassMultiviewCreateInfo *multiview_info = NULL;
   const VkRenderPassInputAttachmentAspectCreateInfo *aspect_info = NULL;
   vk_foreach_struct(ext, pCreateInfo->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO:
         aspect_info = (const VkRenderPassInputAttachmentAspectCreateInfo *)ext;
         /* We don't care about this information */
         break;

      case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO:
         multiview_info = (const VkRenderPassMultiviewCreateInfo*) ext;
         break;

      default:
         mesa_logd("%s: ignored VkStructureType %u\n", __func__, ext->sType);
         break;
      }
   }

   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      attachments[i] = (VkAttachmentDescription2) {
         .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
         .pNext = NULL,
         .flags = pCreateInfo->pAttachments[i].flags,
         .format = pCreateInfo->pAttachments[i].format,
         .samples = pCreateInfo->pAttachments[i].samples,
         .loadOp = pCreateInfo->pAttachments[i].loadOp,
         .storeOp = pCreateInfo->pAttachments[i].storeOp,
         .stencilLoadOp = pCreateInfo->pAttachments[i].stencilLoadOp,
         .stencilStoreOp = pCreateInfo->pAttachments[i].stencilStoreOp,
         .initialLayout = pCreateInfo->pAttachments[i].initialLayout,
         .finalLayout = pCreateInfo->pAttachments[i].finalLayout,
      };
   }

   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      subpasses[i] = (VkSubpassDescription2) {
         .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
         .pNext = NULL,
         .flags = pCreateInfo->pSubpasses[i].flags,
         .pipelineBindPoint = pCreateInfo->pSubpasses[i].pipelineBindPoint,
         .viewMask = 0,
         .inputAttachmentCount = pCreateInfo->pSubpasses[i].inputAttachmentCount,
         .colorAttachmentCount = pCreateInfo->pSubpasses[i].colorAttachmentCount,
         .preserveAttachmentCount = pCreateInfo->pSubpasses[i].preserveAttachmentCount,
         .pPreserveAttachments = pCreateInfo->pSubpasses[i].pPreserveAttachments,
      };

      if (multiview_info && multiview_info->subpassCount) {
         assert(multiview_info->subpassCount == pCreateInfo->subpassCount);
         subpasses[i].viewMask = multiview_info->pViewMasks[i];
      }

      subpasses[i].pInputAttachments = reference_ptr;
      translate_references(&reference_ptr,
                           subpasses[i].inputAttachmentCount,
                           pCreateInfo->pSubpasses[i].pInputAttachments,
                           pCreateInfo, true);
      subpasses[i].pColorAttachments = reference_ptr;
      translate_references(&reference_ptr,
                           subpasses[i].colorAttachmentCount,
                           pCreateInfo->pSubpasses[i].pColorAttachments,
                           pCreateInfo, false);
      subpasses[i].pResolveAttachments = NULL;
      if (pCreateInfo->pSubpasses[i].pResolveAttachments) {
         subpasses[i].pResolveAttachments = reference_ptr;
         translate_references(&reference_ptr,
                              subpasses[i].colorAttachmentCount,
                              pCreateInfo->pSubpasses[i].pResolveAttachments,
                              pCreateInfo, false);
      }
      subpasses[i].pDepthStencilAttachment = NULL;
      if (pCreateInfo->pSubpasses[i].pDepthStencilAttachment) {
         subpasses[i].pDepthStencilAttachment = reference_ptr;
         translate_references(&reference_ptr, 1,
                              pCreateInfo->pSubpasses[i].pDepthStencilAttachment,
                              pCreateInfo, false);
      }
   }

   assert(reference_ptr == references + reference_count);

   if (aspect_info != NULL) {
      for (uint32_t i = 0; i < aspect_info->aspectReferenceCount; i++) {
         const VkInputAttachmentAspectReference *ref =
            &aspect_info->pAspectReferences[i];

         assert(ref->subpass < pCreateInfo->subpassCount);
         VkSubpassDescription2 *subpass = &subpasses[ref->subpass];

         assert(ref->inputAttachmentIndex < subpass->inputAttachmentCount);
         VkAttachmentReference2 *att = (VkAttachmentReference2 *)
            &subpass->pInputAttachments[ref->inputAttachmentIndex];

         att->aspectMask = ref->aspectMask;
      }
   }

   for (uint32_t i = 0; i < pCreateInfo->dependencyCount; i++) {
      dependencies[i] = (VkSubpassDependency2) {
         .sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,
         .pNext = NULL,
         .srcSubpass = pCreateInfo->pDependencies[i].srcSubpass,
         .dstSubpass = pCreateInfo->pDependencies[i].dstSubpass,
         .srcStageMask = pCreateInfo->pDependencies[i].srcStageMask,
         .dstStageMask = pCreateInfo->pDependencies[i].dstStageMask,
         .srcAccessMask = pCreateInfo->pDependencies[i].srcAccessMask,
         .dstAccessMask = pCreateInfo->pDependencies[i].dstAccessMask,
         .dependencyFlags = pCreateInfo->pDependencies[i].dependencyFlags,
         .viewOffset = 0,
      };

      if (multiview_info && multiview_info->dependencyCount) {
         assert(multiview_info->dependencyCount == pCreateInfo->dependencyCount);
         dependencies[i].viewOffset = multiview_info->pViewOffsets[i];
      }
   }

   *create_info = (VkRenderPassCreateInfo2) {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
      .pNext = pCreateInfo->pNext,
      .flags = pCreateInfo->flags,
      .attachmentCount = pCreateInfo->attachmentCount,
      .pAttachments = attachments,
      .subpassCount = pCreateInfo->subpassCount,
      .pSubpasses = subpasses,
      .dependencyCount = pCreateInfo->dependencyCount,
      .pDependencies = dependencies,
   };

   if (multiview_info && multiview_info->correlationMaskCount > 0) {
      create_info->correlatedViewMaskCount = multiview_info->correlationMaskCount;
      create_info->pCorrelatedViewMasks = multiview_info->pCorrelationMasks;
   }

   VkResult result =
      device->dispatch_table.CreateRenderPass2(_device, create_info,
                                               pAllocator, pRenderPass);

   vk_free2(&device->alloc, pAllocator, create_info);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                             const VkRenderPassBeginInfo* pRenderPassBegin,
                             VkSubpassContents contents)
{
   /* We don't have a vk_command_buffer object but we can assume, since we're
    * using common dispatch, that it's a vk_object of some sort.
    */
   struct vk_object_base *disp = (struct vk_object_base *)commandBuffer;

   VkSubpassBeginInfo info = {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO,
      .contents = contents,
   };

   disp->device->dispatch_table.CmdBeginRenderPass2(commandBuffer,
                                                    pRenderPassBegin, &info);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   /* We don't have a vk_command_buffer object but we can assume, since we're
    * using common dispatch, that it's a vk_object of some sort.
    */
   struct vk_object_base *disp = (struct vk_object_base *)commandBuffer;

   VkSubpassEndInfo info = {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO,
   };

   disp->device->dispatch_table.CmdEndRenderPass2(commandBuffer, &info);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdNextSubpass(VkCommandBuffer commandBuffer,
                         VkSubpassContents contents)
{
   /* We don't have a vk_command_buffer object but we can assume, since we're
    * using common dispatch, that it's a vk_object of some sort.
    */
   struct vk_object_base *disp = (struct vk_object_base *)commandBuffer;

   VkSubpassBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO,
      .contents = contents,
   };

   VkSubpassEndInfo end_info = {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO,
   };

   disp->device->dispatch_table.CmdNextSubpass2(commandBuffer, &begin_info,
                                                &end_info);
}

static unsigned
num_subpass_attachments2(const VkSubpassDescription2 *desc)
{
   bool has_depth_stencil_attachment =
      desc->pDepthStencilAttachment != NULL &&
      desc->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED;

   const VkSubpassDescriptionDepthStencilResolve *ds_resolve =
      vk_find_struct_const(desc->pNext,
                           SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);

   bool has_depth_stencil_resolve_attachment =
      ds_resolve && ds_resolve->pDepthStencilResolveAttachment &&
      ds_resolve->pDepthStencilResolveAttachment->attachment != VK_ATTACHMENT_UNUSED;

   return desc->inputAttachmentCount +
          desc->colorAttachmentCount +
          (desc->pResolveAttachments ? desc->colorAttachmentCount : 0) +
          has_depth_stencil_attachment +
          has_depth_stencil_resolve_attachment;
}

static void
vk_render_pass_attachment_init(struct vk_render_pass_attachment *att,
                               const VkAttachmentDescription2 *desc)
{
   *att = (struct vk_render_pass_attachment) {
      .format                 = desc->format,
      .aspects                = vk_format_aspects(desc->format),
      .samples                = desc->samples,
      .load_op                = desc->loadOp,
      .store_op               = desc->storeOp,
      .stencil_load_op        = desc->stencilLoadOp,
      .stencil_store_op       = desc->stencilStoreOp,
      .initial_layout         = desc->initialLayout,
      .final_layout           = desc->finalLayout,
      .initial_stencil_layout = vk_att_desc_stencil_layout(desc, false),
      .final_stencil_layout   = vk_att_desc_stencil_layout(desc, true),
   };
}

static void
vk_subpass_attachment_init(struct vk_subpass_attachment *att,
                           struct vk_render_pass *pass,
                           uint32_t subpass_idx,
                           const VkAttachmentReference2 *ref,
                           const VkAttachmentDescription2 *attachments,
                           VkImageUsageFlagBits usage)
{
   if (ref->attachment >= pass->attachment_count) {
      assert(ref->attachment == VK_ATTACHMENT_UNUSED);
      *att = (struct vk_subpass_attachment) {
         .attachment = VK_ATTACHMENT_UNUSED,
      };
      return;
   }

   struct vk_render_pass_attachment *pass_att =
      &pass->attachments[ref->attachment];

   *att = (struct vk_subpass_attachment) {
      .attachment =     ref->attachment,
      .aspects =        vk_format_aspects(pass_att->format),
      .usage =          usage,
      .layout =         ref->layout,
      .stencil_layout = vk_att_ref_stencil_layout(ref, attachments),
   };

   switch (usage) {
   case VK_IMAGE_USAGE_TRANSFER_DST_BIT:
      break; /* No special aspect requirements */

   case VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT:
      /* From the Vulkan 1.2.184 spec:
       *
       *    "aspectMask is ignored when this structure is used to describe
       *    anything other than an input attachment reference."
       */
      assert(!(ref->aspectMask & ~att->aspects));
      att->aspects = ref->aspectMask;
      break;

   case VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:
      assert(att->aspects == VK_IMAGE_ASPECT_COLOR_BIT);
      break;

   case VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:
      assert(!(att->aspects & ~(VK_IMAGE_ASPECT_DEPTH_BIT |
                                VK_IMAGE_ASPECT_STENCIL_BIT)));
      break;

   default:
      unreachable("Invalid subpass attachment usage");
   }
}

static void
vk_subpass_attachment_link_resolve(struct vk_subpass_attachment *att,
                                   struct vk_subpass_attachment *resolve,
                                   const VkRenderPassCreateInfo2 *info)
{
   if (resolve->attachment == VK_ATTACHMENT_UNUSED)
      return;

   assert(att->attachment != VK_ATTACHMENT_UNUSED);
   att->resolve = resolve;
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_CreateRenderPass2(VkDevice _device,
                            const VkRenderPassCreateInfo2 *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkRenderPass *pRenderPass)
{
   VK_FROM_HANDLE(vk_device, device, _device);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2);

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct vk_render_pass, pass, 1);
   VK_MULTIALLOC_DECL(&ma, struct vk_render_pass_attachment, attachments,
                           pCreateInfo->attachmentCount);
   VK_MULTIALLOC_DECL(&ma, struct vk_subpass, subpasses,
                           pCreateInfo->subpassCount);
   VK_MULTIALLOC_DECL(&ma, struct vk_subpass_dependency, dependencies,
                           pCreateInfo->dependencyCount);

   uint32_t subpass_attachment_count = 0;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      subpass_attachment_count +=
         num_subpass_attachments2(&pCreateInfo->pSubpasses[i]);
   }
   VK_MULTIALLOC_DECL(&ma, struct vk_subpass_attachment, subpass_attachments,
                      subpass_attachment_count);

   if (!vk_object_multizalloc(device, &ma, pAllocator,
                              VK_OBJECT_TYPE_RENDER_PASS))
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   pass->attachment_count = pCreateInfo->attachmentCount;
   pass->attachments = attachments;
   pass->subpass_count = pCreateInfo->subpassCount;
   pass->subpasses = subpasses;
   pass->dependency_count = pCreateInfo->dependencyCount;
   pass->dependencies = dependencies;

   for (uint32_t a = 0; a < pCreateInfo->attachmentCount; a++) {
      vk_render_pass_attachment_init(&pass->attachments[a],
                                     &pCreateInfo->pAttachments[a]);
   }

   struct vk_subpass_attachment *next_subpass_attachment = subpass_attachments;
   for (uint32_t s = 0; s < pCreateInfo->subpassCount; s++) {
      const VkSubpassDescription2 *desc = &pCreateInfo->pSubpasses[s];
      struct vk_subpass *subpass = &pass->subpasses[s];

      subpass->attachment_count = num_subpass_attachments2(desc);
      subpass->attachments = next_subpass_attachment;
      subpass->view_mask = desc->viewMask;

      subpass->input_count = desc->inputAttachmentCount;
      if (desc->inputAttachmentCount > 0) {
         subpass->input_attachments = next_subpass_attachment;
         next_subpass_attachment += desc->inputAttachmentCount;

         for (uint32_t a = 0; a < desc->inputAttachmentCount; a++) {
            vk_subpass_attachment_init(&subpass->input_attachments[a],
                                       pass, s,
                                       &desc->pInputAttachments[a],
                                       pCreateInfo->pAttachments,
                                       VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
         }
      }

      subpass->color_count = desc->colorAttachmentCount;
      if (desc->colorAttachmentCount > 0) {
         subpass->color_attachments = next_subpass_attachment;
         next_subpass_attachment += desc->colorAttachmentCount;

         for (uint32_t a = 0; a < desc->colorAttachmentCount; a++) {
            vk_subpass_attachment_init(&subpass->color_attachments[a],
                                       pass, s,
                                       &desc->pColorAttachments[a],
                                       pCreateInfo->pAttachments,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
         }
      }

      if (desc->pResolveAttachments) {
         subpass->color_resolve_count = desc->colorAttachmentCount;
         subpass->color_resolve_attachments = next_subpass_attachment;
         next_subpass_attachment += desc->colorAttachmentCount;

         for (uint32_t a = 0; a < desc->colorAttachmentCount; a++) {
            vk_subpass_attachment_init(&subpass->color_resolve_attachments[a],
                                       pass, s,
                                       &desc->pResolveAttachments[a],
                                       pCreateInfo->pAttachments,
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            vk_subpass_attachment_link_resolve(&subpass->color_attachments[a],
                                               &subpass->color_resolve_attachments[a],
                                               pCreateInfo);
         }
      }

      if (desc->pDepthStencilAttachment &&
          desc->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED) {
         subpass->depth_stencil_attachment = next_subpass_attachment++;

         vk_subpass_attachment_init(subpass->depth_stencil_attachment,
                                    pass, s,
                                    desc->pDepthStencilAttachment,
                                    pCreateInfo->pAttachments,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
      }

      const VkSubpassDescriptionDepthStencilResolve *ds_resolve =
         vk_find_struct_const(desc->pNext,
                              SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);

      if (ds_resolve && ds_resolve->pDepthStencilResolveAttachment &&
          ds_resolve->pDepthStencilResolveAttachment->attachment != VK_ATTACHMENT_UNUSED) {
         subpass->depth_stencil_resolve_attachment = next_subpass_attachment++;

         vk_subpass_attachment_init(subpass->depth_stencil_resolve_attachment,
                                    pass, s,
                                    ds_resolve->pDepthStencilResolveAttachment,
                                    pCreateInfo->pAttachments,
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
         vk_subpass_attachment_link_resolve(subpass->depth_stencil_attachment,
                                            subpass->depth_stencil_resolve_attachment,
                                            pCreateInfo);

         /* From the Vulkan 1.3.204 spec:
          *
          *    VUID-VkSubpassDescriptionDepthStencilResolve-pDepthStencilResolveAttachment-03178
          *
          *    "If pDepthStencilResolveAttachment is not NULL and does not
          *    have the value VK_ATTACHMENT_UNUSED, depthResolveMode and
          *    stencilResolveMode must not both be VK_RESOLVE_MODE_NONE"
          */
         assert(ds_resolve->depthResolveMode != VK_RESOLVE_MODE_NONE ||
                ds_resolve->stencilResolveMode != VK_RESOLVE_MODE_NONE);

         subpass->depth_resolve_mode = ds_resolve->depthResolveMode;
         subpass->stencil_resolve_mode = ds_resolve->stencilResolveMode;
      }
   }
   assert(next_subpass_attachment ==
          subpass_attachments + subpass_attachment_count);

   pass->dependency_count = pCreateInfo->dependencyCount;
   for (uint32_t d = 0; d < pCreateInfo->dependencyCount; d++) {
      const VkSubpassDependency2 *dep = &pCreateInfo->pDependencies[d];

      pass->dependencies[d] = (struct vk_subpass_dependency) {
         .flags = dep->dependencyFlags,
         .src_subpass = dep->srcSubpass,
         .dst_subpass = dep->dstSubpass,
         .src_stage_mask = (VkPipelineStageFlags2)dep->srcStageMask,
         .dst_stage_mask = (VkPipelineStageFlags2)dep->dstStageMask,
         .src_access_mask = (VkAccessFlags2)dep->srcAccessMask,
         .dst_access_mask = (VkAccessFlags2)dep->dstAccessMask,
         .view_offset = dep->viewOffset,
      };

      /* From the Vulkan 1.3.204 spec:
       *
       *    "If a VkMemoryBarrier2 is included in the pNext chain,
       *    srcStageMask, dstStageMask, srcAccessMask, and dstAccessMask
       *    parameters are ignored. The synchronization and access scopes
       *    instead are defined by the parameters of VkMemoryBarrier2."
       */
      const VkMemoryBarrier2 *barrier =
         vk_find_struct_const(dep->pNext, MEMORY_BARRIER_2);
      if (barrier != NULL) {
         pass->dependencies[d].src_stage_mask = barrier->srcStageMask;
         pass->dependencies[d].dst_stage_mask = barrier->dstStageMask;
         pass->dependencies[d].src_access_mask = barrier->srcAccessMask;
         pass->dependencies[d].dst_access_mask = barrier->dstAccessMask;
      }
   }

   *pRenderPass = vk_render_pass_to_handle(pass);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vk_common_DestroyRenderPass(VkDevice _device,
                            VkRenderPass renderPass,
                            const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_render_pass, pass, renderPass);

   if (!pass)
      return;

   vk_object_free(device, pAllocator, pass);
}
