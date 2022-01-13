/*
 * Copyright 2020 Red Hat, Inc.
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

#include "util/format/u_format.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_box.h"
#include "pipe/p_context.h"
#include "pipe-loader/pipe_loader.h"
#include "state_tracker/st_context.h"
#include "os/os_process.h"
#include "zink/zink_public.h"
#include "zink/zink_instance.h"
#include "driver_trace/tr_screen.h"

#include "dri_screen.h"
#include "utils.h"
#include "dri_context.h"
#include "dri_drawable.h"
#include "dri_helpers.h"
#include "dri_query_renderer.h"

#include <vulkan/vulkan.h>


struct kopper_drawable {
   struct dri_drawable base;
   struct kopper_loader_info info;
};

struct kopper_screen {
   struct dri_screen base;
   struct pipe_screen *screen; //unwrapped
};

extern const __DRIimageExtension driVkImageExtension;

static void
kopper_flush_drawable(__DRIdrawable *dPriv)
{
   dri_flush(dPriv->driContextPriv, dPriv, __DRI2_FLUSH_DRAWABLE, -1);
}

static inline void
kopper_invalidate_drawable(__DRIdrawable *dPriv)
{
   struct dri_drawable *drawable = dri_drawable(dPriv);

   drawable->texture_stamp = dPriv->lastStamp - 1;

   p_atomic_inc(&drawable->base.stamp);
}

static const __DRI2flushExtension driVkFlushExtension = {
    .base = { __DRI2_FLUSH, 4 },

    .flush                = kopper_flush_drawable,
    .invalidate           = kopper_invalidate_drawable,
    .flush_with_flags     = dri_flush,
};

static const __DRIrobustnessExtension dri2Robustness = {
   .base = { __DRI2_ROBUSTNESS, 1 }
};

static const __DRIextension *drivk_screen_extensions[] = {
   &driTexBufferExtension.base,
   &dri2RendererQueryExtension.base,
   &dri2ConfigQueryExtension.base,
   &dri2FenceExtension.base,
   &dri2Robustness.base,
   &driVkImageExtension.base,
   &dri2FlushControlExtension.base,
   &driVkFlushExtension.base,
   NULL
};

static const __DRIconfig **
kopper_init_screen(__DRIscreen * sPriv)
{
   const __DRIconfig **configs;
   struct dri_screen *screen;
   struct kopper_screen *kscreen;
   struct pipe_screen *pscreen = NULL;

   assert(sPriv->kopper_loader);
   kscreen = CALLOC_STRUCT(kopper_screen);
   if (!kscreen)
      return NULL;
   screen = &kscreen->base;

   screen->sPriv = sPriv;
   screen->fd = sPriv->fd;
   screen->can_share_buffer = true;

   sPriv->driverPrivate = (void *)kscreen;

   bool success;
   if (screen->fd != -1)
      success = pipe_loader_drm_probe_fd(&screen->dev, screen->fd);
   else
      success = pipe_loader_vk_probe_dri(&screen->dev, NULL);
   if (success) {
      pscreen = pipe_loader_create_screen(screen->dev);
      dri_init_options(screen);
   }

   if (!pscreen)
      goto fail;

   kscreen->screen = trace_screen_unwrap(pscreen);

   configs = dri_init_screen_helper(screen, pscreen);
   if (!configs)
      goto fail;

   assert(pscreen->get_param(pscreen, PIPE_CAP_DEVICE_RESET_STATUS_QUERY));
   screen->has_reset_status_query = true;
   screen->lookup_egl_image = dri2_lookup_egl_image;
   sPriv->extensions = drivk_screen_extensions;

   const __DRIimageLookupExtension *image = sPriv->dri2.image;
   if (image &&
       image->base.version >= 2 &&
       image->validateEGLImage &&
       image->lookupEGLImageValidated) {
      screen->validate_egl_image = dri2_validate_egl_image;
      screen->lookup_egl_image_validated = dri2_lookup_egl_image_validated;
   }

   return configs;
fail:
   dri_destroy_screen_helper(screen);
   if (screen->dev)
      pipe_loader_release(&screen->dev, 1);
   FREE(screen);
   return NULL;
}

// copypasta alert

static inline void
drisw_present_texture(struct pipe_context *pipe, __DRIdrawable *dPriv,
                      struct pipe_resource *ptex, struct pipe_box *sub_box)
{
   struct dri_drawable *drawable = dri_drawable(dPriv);
   struct dri_screen *screen = dri_screen(drawable->sPriv);

   screen->base.screen->flush_frontbuffer(screen->base.screen, pipe, ptex, 0, 0, drawable, sub_box);
}

extern bool
dri_image_drawable_get_buffers(struct dri_drawable *drawable,
                               struct __DRIimageList *images,
                               const enum st_attachment_type *statts,
                               unsigned statts_count);

static void
kopper_allocate_textures(struct dri_context *ctx,
                         struct dri_drawable *drawable,
                         const enum st_attachment_type *statts,
                         unsigned statts_count)
{
   struct dri_screen *screen = dri_screen(drawable->sPriv);
   struct pipe_resource templ;
   unsigned width, height;
   boolean resized;
   unsigned i;
   struct __DRIimageList images;
   __DRIdrawable *dri_drawable = drawable->dPriv;
   const __DRIimageLoaderExtension *image = drawable->sPriv->image.loader;
   struct kopper_drawable *cdraw = (struct kopper_drawable *)drawable;

   width  = drawable->dPriv->w;
   height = drawable->dPriv->h;

   resized = (drawable->old_w != width ||
              drawable->old_h != height);

   /* First get the buffers from the loader */
   if (image) {
      if (!dri_image_drawable_get_buffers(drawable, &images,
                                          statts, statts_count))
         return;
   }

   if (image) {
      if (images.image_mask & __DRI_IMAGE_BUFFER_FRONT) {
         struct pipe_resource **buf =
            &drawable->textures[ST_ATTACHMENT_FRONT_LEFT];
         struct pipe_resource *texture = images.front->texture;

         dri_drawable->w = texture->width0;
         dri_drawable->h = texture->height0;

         pipe_resource_reference(buf, texture);
      }

      if (images.image_mask & __DRI_IMAGE_BUFFER_BACK) {
         struct pipe_resource **buf =
            &drawable->textures[ST_ATTACHMENT_BACK_LEFT];
         struct pipe_resource *texture = images.back->texture;

         dri_drawable->w = texture->width0;
         dri_drawable->h = texture->height0;

         pipe_resource_reference(buf, texture);
      }

      if (images.image_mask & __DRI_IMAGE_BUFFER_SHARED) {
         struct pipe_resource **buf =
            &drawable->textures[ST_ATTACHMENT_BACK_LEFT];
         struct pipe_resource *texture = images.back->texture;

         dri_drawable->w = texture->width0;
         dri_drawable->h = texture->height0;

         pipe_resource_reference(buf, texture);

         ctx->is_shared_buffer_bound = true;
      } else {
         ctx->is_shared_buffer_bound = false;
      }
   } else {
      /* remove outdated textures */
      if (resized) {
         for (i = 0; i < ST_ATTACHMENT_COUNT; i++) {
            if (drawable->textures[i] && i < ST_ATTACHMENT_DEPTH_STENCIL) {
               drawable->textures[i]->width0 = width;
               drawable->textures[i]->height0 = height;
            } else
               pipe_resource_reference(&drawable->textures[i], NULL);
            pipe_resource_reference(&drawable->msaa_textures[i], NULL);
         }
      }
   }

   memset(&templ, 0, sizeof(templ));
   templ.target = screen->target;
   templ.width0 = width;
   templ.height0 = height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;
   bool is_window = cdraw->info.bos.sType != 0;

   uint32_t attachments = 0;
   for (i = 0; i < statts_count; i++)
      attachments |= BITFIELD_BIT(statts[i]);
   bool front_only = attachments & ST_ATTACHMENT_FRONT_LEFT_MASK && !(attachments & ST_ATTACHMENT_BACK_LEFT_MASK);

   for (i = 0; i < statts_count; i++) {
      enum pipe_format format;
      unsigned bind;

      dri_drawable_get_format(drawable, statts[i], &format, &bind);

      /* the texture already exists or not requested */
      if (!drawable->textures[statts[i]]) {
         if (statts[i] == ST_ATTACHMENT_BACK_LEFT ||
             (statts[i] == ST_ATTACHMENT_FRONT_LEFT && front_only))
            bind |= PIPE_BIND_DISPLAY_TARGET;

         if (format == PIPE_FORMAT_NONE)
            continue;

         templ.format = format;
         templ.bind = bind;
         templ.nr_samples = 0;
         templ.nr_storage_samples = 0;

         if (statts[i] < ST_ATTACHMENT_DEPTH_STENCIL && is_window) {
            void *data;
            if (statts[i] == ST_ATTACHMENT_BACK_LEFT || (statts[i] == ST_ATTACHMENT_FRONT_LEFT && front_only))
               data = &cdraw->info;
            else
               data = drawable->textures[ST_ATTACHMENT_BACK_LEFT];
            assert(data);
            drawable->textures[statts[i]] =
               screen->base.screen->resource_create_drawable(screen->base.screen, &templ, data);
         } else
            drawable->textures[statts[i]] =
               screen->base.screen->resource_create(screen->base.screen, &templ);
      }
      if (drawable->stvis.samples > 1 && !drawable->msaa_textures[statts[i]]) {
         templ.bind = templ.bind &
            ~(PIPE_BIND_SCANOUT | PIPE_BIND_SHARED | PIPE_BIND_DISPLAY_TARGET);
         templ.nr_samples = drawable->stvis.samples;
         templ.nr_storage_samples = drawable->stvis.samples;
         drawable->msaa_textures[statts[i]] =
            screen->base.screen->resource_create(screen->base.screen, &templ);

         dri_pipe_blit(ctx->st->pipe,
                       drawable->msaa_textures[statts[i]],
                       drawable->textures[statts[i]]);
      }
   }

   drawable->old_w = width;
   drawable->old_h = height;
}

static inline void
get_drawable_info(__DRIdrawable *dPriv, int *x, int *y, int *w, int *h)
{
   __DRIscreen *sPriv = dPriv->driScreenPriv;
   const __DRIswrastLoaderExtension *loader = sPriv->swrast_loader;

   if (loader)
      loader->getDrawableInfo(dPriv,
                              x, y, w, h,
                              dPriv->loaderPrivate);
}

static void
kopper_update_drawable_info(struct dri_drawable *drawable)
{
   __DRIdrawable *dPriv = drawable->dPriv;
   __DRIscreen *sPriv = dPriv->driScreenPriv;
   struct kopper_drawable *cdraw = (struct kopper_drawable *)drawable;
   bool is_window = cdraw->info.bos.sType != 0;
   int x, y;
   struct kopper_screen *kscreen = (struct kopper_screen*)sPriv->driverPrivate;
   struct pipe_screen *screen = kscreen->screen;
   struct pipe_resource *ptex = drawable->textures[ST_ATTACHMENT_BACK_LEFT] ?
                                drawable->textures[ST_ATTACHMENT_BACK_LEFT] :
                                drawable->textures[ST_ATTACHMENT_FRONT_LEFT];

   if (is_window && ptex && kscreen->base.fd == -1)
      zink_kopper_update(screen, ptex, &dPriv->w, &dPriv->h);
   else
      get_drawable_info(dPriv, &x, &y, &dPriv->w, &dPriv->h);
}

static inline void
kopper_present_texture(struct pipe_context *pipe, __DRIdrawable *dPriv,
                      struct pipe_resource *ptex, struct pipe_box *sub_box)
{
   struct dri_drawable *drawable = dri_drawable(dPriv);
   struct dri_screen *screen = dri_screen(drawable->sPriv);

   screen->base.screen->flush_frontbuffer(screen->base.screen, pipe, ptex, 0, 0, drawable, sub_box);
}

static inline void
kopper_copy_to_front(struct pipe_context *pipe,
                    __DRIdrawable * dPriv,
                    struct pipe_resource *ptex)
{
   kopper_present_texture(pipe, dPriv, ptex, NULL);

   kopper_invalidate_drawable(dPriv);
}

static bool
kopper_flush_frontbuffer(struct dri_context *ctx,
                         struct dri_drawable *drawable,
                         enum st_attachment_type statt)
{
   struct pipe_resource *ptex;

   if (!ctx || statt != ST_ATTACHMENT_FRONT_LEFT)
      return false;

   if (drawable) {
      /* prevent recursion */
      if (drawable->flushing)
         return true;

      drawable->flushing = true;
   }

   if (drawable->stvis.samples > 1) {
      /* Resolve the front buffer. */
      dri_pipe_blit(ctx->st->pipe,
                    drawable->textures[ST_ATTACHMENT_FRONT_LEFT],
                    drawable->msaa_textures[ST_ATTACHMENT_FRONT_LEFT]);
   }
   ptex = drawable->textures[statt];

   if (ptex) {
      ctx->st->pipe->flush_resource(ctx->st->pipe, drawable->textures[ST_ATTACHMENT_FRONT_LEFT]);
      struct pipe_screen *screen = drawable->screen->base.screen;
      struct st_context_iface *st;
      struct pipe_fence_handle *new_fence = NULL;
      st = ctx->st;
      if (st->thread_finish)
         st->thread_finish(st);

      st->flush(st, ST_FLUSH_FRONT, &new_fence, NULL, NULL);
      if (drawable) {
         drawable->flushing = false;
      }
      /* throttle on the previous fence */
      if (drawable->throttle_fence) {
         screen->fence_finish(screen, NULL, drawable->throttle_fence, PIPE_TIMEOUT_INFINITE);
         screen->fence_reference(screen, &drawable->throttle_fence, NULL);
      }
      drawable->throttle_fence = new_fence;
      kopper_copy_to_front(st->pipe, ctx->dPriv, ptex);
   }

   return true;
}

static void
kopper_update_tex_buffer(struct dri_drawable *drawable,
                         struct dri_context *ctx,
                         struct pipe_resource *res)
{

}

static void
kopper_flush_swapbuffers(struct dri_context *ctx,
                         struct dri_drawable *drawable)
{
   /* does this actually need to do anything? */
}

// XXX this frees its second argument as a side effect - regardless of success
// - since the point is to use it as the superclass initializer before we add
// our own state. kindagross but easier than fixing the object model first.
static struct kopper_drawable *
kopper_create_drawable(__DRIdrawable *dPriv, struct dri_drawable *base)
{
   struct kopper_drawable *_ret = CALLOC_STRUCT(kopper_drawable);

   if (!_ret)
      goto out;
   struct dri_drawable *ret = &_ret->base;

   // copy all the elements
   *ret = *base;

   // relocate references to the old struct
   ret->base.visual = &ret->stvis;
   ret->base.st_manager_private = (void *) ret;
   dPriv->driverPrivate = ret;

   // and fill in the vtable
   ret->allocate_textures = kopper_allocate_textures;
   ret->update_drawable_info = kopper_update_drawable_info;
   ret->flush_frontbuffer = kopper_flush_frontbuffer;
   ret->update_tex_buffer = kopper_update_tex_buffer;
   ret->flush_swapbuffers = kopper_flush_swapbuffers;

out:
   free(base);
   return _ret;
}

static boolean
kopper_create_buffer(__DRIscreen * sPriv,
                     __DRIdrawable * dPriv,
                     const struct gl_config *visual, boolean isPixmap)
{
   struct kopper_drawable *drawable = NULL;

   /* always pass !pixmap because it isn't "handled" or relevant */
   if (!dri_create_buffer(sPriv, dPriv, visual, false))
      return FALSE;

   drawable = kopper_create_drawable(dPriv, dPriv->driverPrivate);
   if (!drawable)
      return FALSE;

   drawable->info.has_alpha = visual->alphaBits > 0;
   if (sPriv->kopper_loader->SetSurfaceCreateInfo && !isPixmap)
      sPriv->kopper_loader->SetSurfaceCreateInfo(dPriv->loaderPrivate,
                                                 &drawable->info);

   return TRUE;
}

static void
kopper_swap_buffers(__DRIdrawable *dPriv)
{
   struct dri_context *ctx = dri_get_current(dPriv->driScreenPriv);
   struct dri_drawable *drawable = dri_drawable(dPriv);
   struct pipe_resource *ptex;

   if (!ctx)
      return;

   ptex = drawable->textures[ST_ATTACHMENT_BACK_LEFT];
   if (!ptex)
      return;

   drawable->texture_stamp = dPriv->lastStamp - 1;
   dri_flush(dPriv->driContextPriv, dPriv, __DRI2_FLUSH_DRAWABLE | __DRI2_FLUSH_CONTEXT, __DRI2_THROTTLE_SWAPBUFFER);
   kopper_copy_to_front(ctx->st->pipe, dPriv, ptex);
   if (!drawable->textures[ST_ATTACHMENT_FRONT_LEFT]) {
      return;
   }
   /* have to manually swap the pointers here to make frontbuffer readback work */
   drawable->textures[ST_ATTACHMENT_BACK_LEFT] = drawable->textures[ST_ATTACHMENT_FRONT_LEFT];
   drawable->textures[ST_ATTACHMENT_FRONT_LEFT] = ptex;
}

static __DRIdrawable *
kopperCreateNewDrawable(__DRIscreen *screen,
                        const __DRIconfig *config,
                        void *data,
                        int is_pixmap)
{
    __DRIdrawable *pdraw;

    assert(data != NULL);

    pdraw = malloc(sizeof *pdraw);
    if (!pdraw)
	return NULL;

    pdraw->loaderPrivate = data;

    pdraw->driScreenPriv = screen;
    pdraw->driContextPriv = NULL;
    pdraw->refcount = 0;
    pdraw->lastStamp = 0;
    pdraw->w = 0;
    pdraw->h = 0;

    //dri_get_drawable(pdraw);
    pdraw->refcount++;

    if (!screen->driver->CreateBuffer(screen, pdraw, &config->modes,
                                      is_pixmap)) {
       free(pdraw);
       return NULL;
    }

    pdraw->dri2.stamp = pdraw->lastStamp + 1;

    return pdraw;
}


const __DRIkopperExtension driKopperExtension = {
   .base = { __DRI_KOPPER, 1 },
   .createNewDrawable          = kopperCreateNewDrawable,
};

const struct __DriverAPIRec galliumvk_driver_api = {
   .InitScreen = kopper_init_screen,
   .DestroyScreen = dri_destroy_screen,
   .CreateBuffer = kopper_create_buffer,
   .DestroyBuffer = dri_destroy_buffer,
   .SwapBuffers = kopper_swap_buffers,
   .CopySubBuffer = NULL,
};

static const struct __DRIDriverVtableExtensionRec galliumvk_vtable = {
   .base = { __DRI_DRIVER_VTABLE, 1 },
   .vtable = &galliumvk_driver_api,
};

const __DRIextension *galliumvk_driver_extensions[] = {
   &driCoreExtension.base,
   &driSWRastExtension.base,
   &driDRI2Extension.base,
   &driImageDriverExtension.base,
   &driKopperExtension.base,
   &gallium_config_options.base,
   &galliumvk_vtable.base,
   NULL
};

/* vim: set sw=3 ts=8 sts=3 expandtab: */
