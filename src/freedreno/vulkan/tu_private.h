/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
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

#ifndef TU_PRIVATE_H
#define TU_PRIVATE_H

#include "tu_common.h"
#include "tu_autotune.h"
#include "tu_clear_blit.h"
#include "tu_cmd_buffer.h"
#include "tu_cs.h"
#include "tu_descriptor_set.h"
#include "tu_device.h"
#include "tu_drm.h"
#include "tu_dynamic_rendering.h"
#include "tu_formats.h"
#include "tu_image.h"
#include "tu_lrz.h"
#include "tu_pass.h"
#include "tu_perfetto.h"
#include "tu_pipeline.h"
#include "tu_query.h"
#include "tu_shader.h"
#include "tu_suballoc.h"
#include "tu_util.h"
#include "tu_wsi.h"

/* Whenever we generate an error, pass it through this function. Useful for
 * debugging, where we can break on it. Only call at error site, not when
 * propagating errors. Might be useful to plug in a stack trace here.
 */

VkResult
__vk_startup_errorf(struct tu_instance *instance,
                    VkResult error,
                    bool force_print,
                    const char *file,
                    int line,
                    const char *format,
                    ...) PRINTFLIKE(6, 7);

/* Prints startup errors if TU_DEBUG=startup is set or on a debug driver
 * build.
 */
#define vk_startup_errorf(instance, error, format, ...) \
   __vk_startup_errorf(instance, error, \
                       instance->debug_flags & TU_DEBUG_STARTUP, \
                       __FILE__, __LINE__, format, ##__VA_ARGS__)

void
__tu_finishme(const char *file, int line, const char *format, ...)
   PRINTFLIKE(3, 4);

/**
 * Print a FINISHME message, including its source location.
 */
#define tu_finishme(format, ...)                                             \
   do {                                                                      \
      static bool reported = false;                                          \
      if (!reported) {                                                       \
         __tu_finishme(__FILE__, __LINE__, format, ##__VA_ARGS__);           \
         reported = true;                                                    \
      }                                                                      \
   } while (0)

#define tu_stub()                                                            \
   do {                                                                      \
      tu_finishme("stub %s", __func__);                                      \
   } while (0)

void
tu_framebuffer_tiling_config(struct tu_framebuffer *fb,
                             const struct tu_device *device,
                             const struct tu_render_pass *pass);

VkResult
tu_gralloc_info(struct tu_device *device,
                const VkNativeBufferANDROID *gralloc_info,
                int *dma_buf,
                uint64_t *modifier);

VkResult
tu_import_memory_from_gralloc_handle(VkDevice device_h,
                                     int dma_buf,
                                     const VkAllocationCallbacks *alloc,
                                     VkImage image_h);

#endif /* TU_PRIVATE_H */
