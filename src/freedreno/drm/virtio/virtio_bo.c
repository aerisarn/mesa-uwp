/*
 * Copyright © 2022 Google, Inc.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "util/libsync.h"

#include "virtio_priv.h"

static int
bo_allocate(struct virtio_bo *virtio_bo)
{
   struct fd_bo *bo = &virtio_bo->base;
   if (!virtio_bo->offset) {
      struct drm_virtgpu_map req = {
         .handle = bo->handle,
      };
      int ret;

      ret = drmIoctl(bo->dev->fd, DRM_IOCTL_VIRTGPU_MAP, &req);
      if (ret) {
         ERROR_MSG("alloc failed: %s", strerror(errno));
         return ret;
      }

      virtio_bo->offset = req.offset;
   }

   return 0;
}

static int
virtio_bo_offset(struct fd_bo *bo, uint64_t *offset)
{
   struct virtio_bo *virtio_bo = to_virtio_bo(bo);
   int ret = bo_allocate(virtio_bo);
   if (ret)
      return ret;
   *offset = virtio_bo->offset;
   return 0;
}

static int
virtio_bo_cpu_prep_guest(struct fd_bo *bo)
{
   struct drm_virtgpu_3d_wait args = {
         .handle = bo->handle,
   };
   int ret;

   /* Side note, this ioctl is defined as IO_WR but should be IO_W: */
   ret = drmIoctl(bo->dev->fd, DRM_IOCTL_VIRTGPU_WAIT, &args);
   if (ret && errno == EBUSY)
      return -EBUSY;

   return 0;
}

static int
virtio_bo_cpu_prep(struct fd_bo *bo, struct fd_pipe *pipe, uint32_t op)
{
   int ret;

   /*
    * Wait first in the guest, to avoid a blocking call in host.
    * If implicit sync it used, we still need to *also* wait in
    * host, if it is a shared buffer, because the guest doesn't
    * know about usage of the bo in the host (or other guests).
    */

   ret = virtio_bo_cpu_prep_guest(bo);
   if (ret)
      goto out;

   /* If buffer is not shared, then it is not shared with host,
    * so we don't need to worry about implicit sync in host:
    */
   if (!bo->shared)
      goto out;

   /* If buffer is shared, but we are using explicit sync, no
    * need to fallback to implicit sync in host:
    */
   if (pipe && to_virtio_pipe(pipe)->no_implicit_sync)
      goto out;

   struct msm_ccmd_gem_cpu_prep_req req = {
         .hdr = MSM_CCMD(GEM_CPU_PREP, sizeof(req)),
         .host_handle = virtio_bo_host_handle(bo),
         .op = op,
   };
   struct msm_ccmd_gem_cpu_prep_rsp *rsp;

   /* We can't do a blocking wait in the host, so we have to poll: */
   do {
      rsp = virtio_alloc_rsp(bo->dev, &req.hdr, sizeof(*rsp));

      ret = virtio_execbuf(bo->dev, &req.hdr, true);
      if (ret)
         goto out;

      ret = rsp->ret;
   } while (ret == -EBUSY);

out:
   return ret;
}

static void
virtio_bo_cpu_fini(struct fd_bo *bo)
{
   /* no-op */
}

static int
virtio_bo_madvise(struct fd_bo *bo, int willneed)
{
   /* TODO:
    * Currently unsupported, synchronous WILLNEED calls would introduce too
    * much latency.. ideally we'd keep state in the guest and only flush
    * down to host when host is under memory pressure.  (Perhaps virtio-balloon
    * could signal this?)
    */
   return willneed;
}

static uint64_t
virtio_bo_iova(struct fd_bo *bo)
{
   /* The shmem bo is allowed to have no iova, as it is only used for
    * guest<->host communications:
    */
   assert(bo->iova || (to_virtio_bo(bo)->blob_id == 0));
   return bo->iova;
}

static void
virtio_bo_set_name(struct fd_bo *bo, const char *fmt, va_list ap)
{
   char name[32];
   int sz;

   /* Note, we cannot set name on the host for the shmem bo, as
    * that isn't a real gem obj on the host side.. not having
    * an iova is a convenient way to detect this case:
    */
   if (!bo->iova)
      return;

   sz = vsnprintf(name, sizeof(name), fmt, ap);
   sz = MIN2(sz, sizeof(name));

   unsigned req_len = sizeof(struct msm_ccmd_gem_set_name_req) + align(sz, 4);

   uint8_t buf[req_len];
   struct msm_ccmd_gem_set_name_req *req = (void *)buf;

   req->hdr = MSM_CCMD(GEM_SET_NAME, req_len);
   req->host_handle = virtio_bo_host_handle(bo);
   req->len = sz;

   memcpy(req->payload, name, sz);

   virtio_execbuf(bo->dev, &req->hdr, false);
}

static void
virtio_bo_upload(struct fd_bo *bo, void *src, unsigned len)
{
   unsigned req_len = sizeof(struct msm_ccmd_gem_upload_req) + align(len, 4);

   uint8_t buf[req_len];
   struct msm_ccmd_gem_upload_req *req = (void *)buf;

   req->hdr = MSM_CCMD(GEM_UPLOAD, req_len);
   req->host_handle = virtio_bo_host_handle(bo);
   req->pad = 0;
   req->off = 0;
   req->len = len;

   memcpy(req->payload, src, len);

   virtio_execbuf(bo->dev, &req->hdr, false);
}

static void
virtio_bo_destroy(struct fd_bo *bo)
{
   struct virtio_bo *virtio_bo = to_virtio_bo(bo);
   struct virtio_device *virtio_dev = to_virtio_device(bo->dev);

   if (virtio_dev->userspace_allocates_iova && bo->iova) {
      struct msm_ccmd_gem_close_req req = {
            .hdr = MSM_CCMD(GEM_CLOSE, sizeof(req)),
            .host_handle = virtio_bo_host_handle(bo),
      };

      virtio_execbuf(bo->dev, &req.hdr, false);

      virtio_dev_free_iova(bo->dev, bo->iova, bo->size);
   }

   free(virtio_bo);
}

static const struct fd_bo_funcs funcs = {
   .offset = virtio_bo_offset,
   .cpu_prep = virtio_bo_cpu_prep,
   .cpu_fini = virtio_bo_cpu_fini,
   .madvise = virtio_bo_madvise,
   .iova = virtio_bo_iova,
   .set_name = virtio_bo_set_name,
   .upload = virtio_bo_upload,
   .destroy = virtio_bo_destroy,
};

struct allocation_wait {
   struct fd_bo *bo;
   int fence_fd;
   struct msm_ccmd_gem_new_rsp *new_rsp;
   struct msm_ccmd_gem_info_rsp *info_rsp;
};

static void
allocation_wait_execute(void *job, void *gdata, int thread_index)
{
   struct allocation_wait *wait = job;
   struct virtio_bo *virtio_bo = to_virtio_bo(wait->bo);

   sync_wait(wait->fence_fd, -1);
   close(wait->fence_fd);

   if (wait->new_rsp) {
      virtio_bo->host_handle = wait->new_rsp->host_handle;
   } else {
      virtio_bo->host_handle = wait->info_rsp->host_handle;
      wait->bo->size = wait->info_rsp->size;
   }
   fd_bo_del(wait->bo);
   free(wait);
}

static void
enqueue_allocation_wait(struct fd_bo *bo, int fence_fd,
                        struct msm_ccmd_gem_new_rsp *new_rsp,
                        struct msm_ccmd_gem_info_rsp *info_rsp)
{
   struct allocation_wait *wait = malloc(sizeof(*wait));

   wait->bo = fd_bo_ref(bo);
   wait->fence_fd = fence_fd;
   wait->new_rsp = new_rsp;
   wait->info_rsp = info_rsp;

   util_queue_add_job(&bo->dev->submit_queue,
                      wait, &to_virtio_bo(bo)->fence,
                      allocation_wait_execute,
                      NULL, 0);
}

static struct fd_bo *
bo_from_handle(struct fd_device *dev, uint32_t size, uint32_t handle)
{
   struct virtio_bo *virtio_bo;
   struct fd_bo *bo;

   virtio_bo = calloc(1, sizeof(*virtio_bo));
   if (!virtio_bo)
      return NULL;

   util_queue_fence_init(&virtio_bo->fence);

   bo = &virtio_bo->base;

   /* Note we need to set these because allocation_wait_execute() could
    * run before bo_init_commont():
    */
   bo->dev = dev;
   p_atomic_set(&bo->refcnt, 1);

   bo->size = size;
   bo->funcs = &funcs;
   bo->handle = handle;

   fd_bo_init_common(bo, dev);

   return bo;
}

/* allocate a new buffer object from existing handle */
struct fd_bo *
virtio_bo_from_handle(struct fd_device *dev, uint32_t size, uint32_t handle)
{
   struct virtio_device *virtio_dev = to_virtio_device(dev);
   struct fd_bo *bo = bo_from_handle(dev, size, handle);
   struct drm_virtgpu_resource_info args = {
         .bo_handle = handle,
   };
   int ret;

   ret = drmCommandWriteRead(dev->fd, DRM_VIRTGPU_RESOURCE_INFO, &args, sizeof(args));
   if (ret) {
      INFO_MSG("failed to get resource info: %s", strerror(errno));
      goto fail;
   }

   struct msm_ccmd_gem_info_req req = {
         .hdr = MSM_CCMD(GEM_INFO, sizeof(req)),
         .res_id = args.res_handle,
         .blob_mem = args.blob_mem,
         .blob_id = p_atomic_inc_return(&virtio_dev->next_blob_id),
   };

   if (virtio_dev->userspace_allocates_iova) {
      req.iova = virtio_dev_alloc_iova(dev, size);
      if (!req.iova) {
         virtio_dev_free_iova(dev, req.iova, size);
         ret = -ENOMEM;
         goto fail;
      }
   }

   struct msm_ccmd_gem_info_rsp *rsp =
         virtio_alloc_rsp(dev, &req.hdr, sizeof(*rsp));

   struct virtio_bo *virtio_bo = to_virtio_bo(bo);

   virtio_bo->blob_id = req.blob_id;

   if (virtio_dev->userspace_allocates_iova) {
      int fence_fd;
      ret = virtio_execbuf_fenced(dev, &req.hdr, -1, &fence_fd, 0);
      if (ret) {
         INFO_MSG("failed to get gem info: %s", strerror(errno));
         goto fail;
      }

      bo->iova = req.iova;

      enqueue_allocation_wait(bo, fence_fd, NULL, rsp);
   } else {
      ret = virtio_execbuf(dev, &req.hdr, true);
      if (ret) {
         INFO_MSG("failed to get gem info: %s", strerror(errno));
         goto fail;
      }
      if (rsp->ret) {
         INFO_MSG("failed (on host) to get gem info: %s", strerror(rsp->ret));
         goto fail;
      }

      virtio_bo->host_handle = rsp->host_handle;
      bo->iova = rsp->iova;

      /* If the imported buffer is allocated via virgl context (for example
       * minigbm/arc-cros-gralloc) then the guest gem object size is fake,
       * potentially not accounting for UBWC meta data, required pitch
       * alignment, etc.  But in the import path the gallium driver checks
       * that the size matches the minimum size based on layout.  So replace
       * the guest potentially-fake size with the real size from the host:
       */
      bo->size = rsp->size;
   }

   return bo;

fail:
   virtio_bo_destroy(bo);
   return NULL;
}

/* allocate a buffer object: */
struct fd_bo *
virtio_bo_new(struct fd_device *dev, uint32_t size, uint32_t flags)
{
   struct virtio_device *virtio_dev = to_virtio_device(dev);
   struct drm_virtgpu_resource_create_blob args = {
         .blob_mem   = VIRTGPU_BLOB_MEM_HOST3D,
         .size       = size,
   };
   struct msm_ccmd_gem_new_req req = {
         .hdr = MSM_CCMD(GEM_NEW, sizeof(req)),
         .size = size,
   };
   struct msm_ccmd_gem_new_rsp *rsp = NULL;
   int ret;

   if (flags & FD_BO_SCANOUT)
      req.flags |= MSM_BO_SCANOUT;

   if (flags & FD_BO_GPUREADONLY)
      req.flags |= MSM_BO_GPU_READONLY;

   if (flags & FD_BO_CACHED_COHERENT) {
      req.flags |= MSM_BO_CACHED_COHERENT;
   } else {
      req.flags |= MSM_BO_WC;
   }

   if (flags & _FD_BO_VIRTIO_SHM) {
      args.blob_id = 0;
      args.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
   } else {
      if (flags & (FD_BO_SHARED | FD_BO_SCANOUT)) {
         args.blob_flags = VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE |
               VIRTGPU_BLOB_FLAG_USE_SHAREABLE;
      }

      if (!(flags & FD_BO_NOMAP)) {
         args.blob_flags |= VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
      }

      args.blob_id = p_atomic_inc_return(&virtio_dev->next_blob_id);
      args.cmd = VOID2U64(&req);
      args.cmd_size = sizeof(req);

      /* tunneled cmds are processed separately on host side,
       * before the renderer->get_blob() callback.. the blob_id
       * is used to like the created bo to the get_blob() call
       */
      req.blob_id = args.blob_id;

      rsp = virtio_alloc_rsp(dev, &req.hdr, sizeof(*rsp));

      if (virtio_dev->userspace_allocates_iova) {
         req.iova = virtio_dev_alloc_iova(dev, size);
         if (!req.iova) {
            ret = -ENOMEM;
            goto fail;
         }
      }
   }

   simple_mtx_lock(&virtio_dev->eb_lock);
   if (rsp)
      req.hdr.seqno = ++virtio_dev->next_seqno;
   ret = drmIoctl(dev->fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &args);
   simple_mtx_unlock(&virtio_dev->eb_lock);
   if (ret)
      goto fail;

   struct fd_bo *bo = bo_from_handle(dev, size, args.bo_handle);
   struct virtio_bo *virtio_bo = to_virtio_bo(bo);

   virtio_bo->blob_id = args.blob_id;

   if (rsp) {
      if (virtio_dev->userspace_allocates_iova) {
         int fence_fd;

         /* We can't get a fence fd from RESOURCE_CREATE_BLOB, so send
          * a NOP packet just for that purpose:
          */
         struct msm_ccmd_nop_req nop = {
               .hdr = MSM_CCMD(NOP, sizeof(nop)),
         };

         ret = virtio_execbuf_fenced(dev, &nop.hdr, -1, &fence_fd, 0);
         if (ret) {
            INFO_MSG("failed to get gem info: %s", strerror(errno));
            goto fail;
         }

         bo->iova = req.iova;

         enqueue_allocation_wait(bo, fence_fd, rsp, NULL);
      } else {
         /* RESOURCE_CREATE_BLOB is async, so we need to wait for host..
          * which is a bit unfortunate, but better to sync here than
          * add extra code to check if we need to wait each time we
          * emit a reloc.
          */
         virtio_host_sync(dev, &req.hdr);

         virtio_bo->host_handle = rsp->host_handle;
         bo->iova = rsp->iova;
      }
   }

   return bo;

fail:
   if (req.iova) {
      assert(virtio_dev->userspace_allocates_iova);
      virtio_dev_free_iova(dev, req.iova, size);
   }
   return NULL;
}

uint32_t
virtio_bo_host_handle(struct fd_bo *bo)
{
   struct virtio_bo *virtio_bo = to_virtio_bo(bo);
   util_queue_fence_wait(&virtio_bo->fence);
   return virtio_bo->host_handle;
}
