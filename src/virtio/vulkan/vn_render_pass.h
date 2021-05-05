/*
 * Copyright 2019 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#ifndef VN_RENDER_PASS_H
#define VN_RENDER_PASS_H

#include "vn_common.h"

struct vn_present_src_attachment {
   bool acquire;
   uint32_t index;
};

struct vn_render_pass {
   struct vn_object_base base;

   VkExtent2D granularity;

   /* track attachments that have PRESENT_SRC as their initialLayout or
    * finalLayout
    */
   uint32_t acquire_count;
   uint32_t release_count;
   uint32_t present_src_count;
   struct vn_present_src_attachment present_src_attachments[];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_render_pass,
                               base.base,
                               VkRenderPass,
                               VK_OBJECT_TYPE_RENDER_PASS)

struct vn_framebuffer {
   struct vn_object_base base;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_framebuffer,
                               base.base,
                               VkFramebuffer,
                               VK_OBJECT_TYPE_FRAMEBUFFER)

#endif /* VN_RENDER_PASS_H */
