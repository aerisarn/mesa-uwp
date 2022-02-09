/*
 * Copyright © 2021 Intel Corporation
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
#ifndef VK_RENDER_PASS_H
#define VK_RENDER_PASS_H

#include "vk_object.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vk_subpass_attachment {
   /** VkAttachmentReference2::attachment */
   uint32_t attachment;

   /** Aspects referenced by this attachment
    *
    * For an input attachment, this is VkAttachmentReference2::aspectMask.
    * For all others, it's equal to the vk_render_pass_attachment::aspects.
    */
   VkImageAspectFlags aspects;

   /** Usage for this attachment
    *
    * This is a single VK_IMAGE_USAGE_* describing the usage of this subpass
    * attachment.  Resolve attachments are VK_IMAGE_USAGE_TRANSFER_DST_BIT.
    */
   VkImageUsageFlagBits usage;

   /** VkAttachmentReference2::layout */
   VkImageLayout layout;

   /** VkAttachmentReferenceStencilLayout::stencilLayout
    *
    * If VK_KHR_separate_depth_stencil_layouts is not used, this will be
    * layout if the attachment contains stencil and VK_IMAGE_LAYOUT_UNDEFINED
    * otherwise.
    */
   VkImageLayout stencil_layout;

   /** A per-view mask for if this is the last use of this attachment
    *
    * If the same render pass attachment is used multiple ways within a
    * subpass, corresponding last_subpass bits will be set in all of them.
    * For the non-multiview case, only the first bit is used.
    */
   uint32_t last_subpass;

   /** Resolve attachment, if any */
   struct vk_subpass_attachment *resolve;
};

struct vk_subpass {
   /** Count of all attachments referenced by this subpass */
   uint32_t attachment_count;

   /** Array of all attachments referenced by this subpass */
   struct vk_subpass_attachment *attachments;

   /** VkSubpassDescription2::inputAttachmentCount */
   uint32_t input_count;

   /** VkSubpassDescription2::pInputAttachments */
   struct vk_subpass_attachment *input_attachments;

   /** VkSubpassDescription2::colorAttachmentCount */
   uint32_t color_count;

   /** VkSubpassDescription2::pColorAttachments */
   struct vk_subpass_attachment *color_attachments;

   /** VkSubpassDescription2::colorAttachmentCount or zero */
   uint32_t color_resolve_count;

   /** VkSubpassDescription2::pResolveAttachments */
   struct vk_subpass_attachment *color_resolve_attachments;

   /** VkSubpassDescription2::pDepthStencilAttachment */
   struct vk_subpass_attachment *depth_stencil_attachment;

   /** VkSubpassDescriptionDepthStencilResolve::pDepthStencilResolveAttachment */
   struct vk_subpass_attachment *depth_stencil_resolve_attachment;

   /** VkSubpassDescription2::viewMask or 1 for non-multiview
    *
    * For all view masks in the vk_render_pass data structure, we use a mask
    * of 1 for non-multiview instead of a mask of 0.  To determine if the
    * render pass is multiview or not, see vk_render_pass::is_multiview.
    */
   uint32_t view_mask;

   /** VkSubpassDescriptionDepthStencilResolve::depthResolveMode */
   VkResolveModeFlagBitsKHR depth_resolve_mode;

   /** VkSubpassDescriptionDepthStencilResolve::stencilResolveMode */
   VkResolveModeFlagBitsKHR stencil_resolve_mode;
};

struct vk_render_pass_attachment {
   /** VkAttachmentDescription2::format */
   VkFormat format;

   /** Aspects contained in format */
   VkImageAspectFlags aspects;

   /** VkAttachmentDescription2::samples */
   uint32_t samples;

   /** Views in which this attachment is used, 0 for unused
    *
    * For non-multiview, this will be 1 if the attachment is used.
    */
   uint32_t view_mask;

   /** VkAttachmentDescription2::loadOp */
   VkAttachmentLoadOp load_op;

   /** VkAttachmentDescription2::storeOp */
   VkAttachmentStoreOp store_op;

   /** VkAttachmentDescription2::stencilLoadOp */
   VkAttachmentLoadOp stencil_load_op;

   /** VkAttachmentDescription2::stencilStoreOp */
   VkAttachmentStoreOp stencil_store_op;

   /** VkAttachmentDescription2::initialLayout */
   VkImageLayout initial_layout;

   /** VkAttachmentDescription2::finalLayout */
   VkImageLayout final_layout;

   /** VkAttachmentDescriptionStencilLayout::stencilInitialLayout
    *
    * If VK_KHR_separate_depth_stencil_layouts is not used, this will be
    * initial_layout if format contains stencil and VK_IMAGE_LAYOUT_UNDEFINED
    * otherwise.
    */
   VkImageLayout initial_stencil_layout;

   /** VkAttachmentDescriptionStencilLayout::stencilFinalLayout
    *
    * If VK_KHR_separate_depth_stencil_layouts is not used, this will be
    * final_layout if format contains stencil and VK_IMAGE_LAYOUT_UNDEFINED
    * otherwise.
    */
   VkImageLayout final_stencil_layout;
};

struct vk_subpass_dependency {
   /** VkSubpassDependency2::dependencyFlags */
   VkDependencyFlags flags;

   /** VkSubpassDependency2::srcSubpass */
   uint32_t src_subpass;

   /** VkSubpassDependency2::dstSubpass */
   uint32_t dst_subpass;

   /** VkSubpassDependency2::srcStageMask */
   VkPipelineStageFlags2 src_stage_mask;

   /** VkSubpassDependency2::dstStageMask */
   VkPipelineStageFlags2 dst_stage_mask;

   /** VkSubpassDependency2::srcAccessMask */
   VkAccessFlags2 src_access_mask;

   /** VkSubpassDependency2::dstAccessMask */
   VkAccessFlags2 dst_access_mask;

   /** VkSubpassDependency2::viewOffset */
   int32_t view_offset;
};

struct vk_render_pass {
   struct vk_object_base base;

   /** True if this render pass uses multiview
    *
    * This is true if all subpasses have viewMask != 0.
    */
   bool is_multiview;

   /** Views used by this render pass or 1 for non-multiview */
   uint32_t view_mask;

   /** VkRenderPassCreateInfo2::attachmentCount */
   uint32_t attachment_count;

   /** VkRenderPassCreateInfo2::pAttachments */
   struct vk_render_pass_attachment *attachments;

   /** VkRenderPassCreateInfo2::subpassCount */
   uint32_t subpass_count;

   /** VkRenderPassCreateInfo2::subpasses */
   struct vk_subpass *subpasses;

   /** VkRenderPassCreateInfo2::dependencyCount */
   uint32_t dependency_count;

   /** VkRenderPassCreateInfo2::pDependencies */
   struct vk_subpass_dependency *dependencies;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(vk_render_pass, base, VkRenderPass,
                               VK_OBJECT_TYPE_RENDER_PASS);

#ifdef __cplusplus
}
#endif

#endif /* VK_RENDER_PASS_H */
