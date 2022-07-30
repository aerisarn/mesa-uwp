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
#include "tu_cs.h"
#include "tu_descriptor_set.h"
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

struct tu_memory_heap {
   /* Standard bits passed on to the client */
   VkDeviceSize      size;
   VkMemoryHeapFlags flags;

   /** Copied from ANV:
    *
    * Driver-internal book-keeping.
    *
    * Align it to 64 bits to make atomic operations faster on 32 bit platforms.
    */
   VkDeviceSize      used __attribute__ ((aligned (8)));
};

uint64_t
tu_get_system_heap_size(void);

struct tu_physical_device
{
   struct vk_physical_device vk;

   struct tu_instance *instance;

   const char *name;
   uint8_t driver_uuid[VK_UUID_SIZE];
   uint8_t device_uuid[VK_UUID_SIZE];
   uint8_t cache_uuid[VK_UUID_SIZE];

   struct wsi_device wsi_device;

   int local_fd;
   bool has_local;
   int64_t local_major;
   int64_t local_minor;
   int master_fd;
   bool has_master;
   int64_t master_major;
   int64_t master_minor;

   uint32_t gmem_size;
   uint64_t gmem_base;
   uint32_t ccu_offset_gmem;
   uint32_t ccu_offset_bypass;

   struct fd_dev_id dev_id;
   const struct fd_dev_info *info;

   int msm_major_version;
   int msm_minor_version;

   /* Address space and global fault count for this local_fd with DRM backend */
   uint64_t fault_count;

   struct tu_memory_heap heap;

   struct vk_sync_type syncobj_type;
   struct vk_sync_timeline_type timeline_type;
   const struct vk_sync_type *sync_types[3];
};

enum tu_debug_flags
{
   TU_DEBUG_STARTUP = 1 << 0,
   TU_DEBUG_NIR = 1 << 1,
   TU_DEBUG_NOBIN = 1 << 3,
   TU_DEBUG_SYSMEM = 1 << 4,
   TU_DEBUG_FORCEBIN = 1 << 5,
   TU_DEBUG_NOUBWC = 1 << 6,
   TU_DEBUG_NOMULTIPOS = 1 << 7,
   TU_DEBUG_NOLRZ = 1 << 8,
   TU_DEBUG_PERFC = 1 << 9,
   TU_DEBUG_FLUSHALL = 1 << 10,
   TU_DEBUG_SYNCDRAW = 1 << 11,
   TU_DEBUG_DONT_CARE_AS_LOAD = 1 << 12,
   TU_DEBUG_GMEM = 1 << 13,
   TU_DEBUG_RAST_ORDER = 1 << 14,
   TU_DEBUG_UNALIGNED_STORE = 1 << 15,
   TU_DEBUG_LAYOUT = 1 << 16,
   TU_DEBUG_LOG_SKIP_GMEM_OPS = 1 << 17,
   TU_DEBUG_PERF = 1 << 18,
   TU_DEBUG_NOLRZFC = 1 << 19,
   TU_DEBUG_DYNAMIC = 1 << 20,
};

struct tu_instance
{
   struct vk_instance vk;

   uint32_t api_version;
   int physical_device_count;
   struct tu_physical_device physical_devices[TU_MAX_DRM_DEVICES];

   struct driOptionCache dri_options;
   struct driOptionCache available_dri_options;

   enum tu_debug_flags debug_flags;
};

bool
tu_instance_extension_supported(const char *name);
uint32_t
tu_physical_device_api_version(struct tu_physical_device *dev);
bool
tu_physical_device_extension_supported(struct tu_physical_device *dev,
                                       const char *name);

/* queue types */
#define TU_QUEUE_GENERAL 0

#define TU_MAX_QUEUE_FAMILIES 1

struct tu_queue
{
   struct vk_queue vk;

   struct tu_device *device;

   uint32_t msm_queue_id;
   int fence;
};

enum global_shader {
   GLOBAL_SH_VS_BLIT,
   GLOBAL_SH_VS_CLEAR,
   GLOBAL_SH_FS_BLIT,
   GLOBAL_SH_FS_BLIT_ZSCALE,
   GLOBAL_SH_FS_COPY_MS,
   GLOBAL_SH_FS_CLEAR0,
   GLOBAL_SH_FS_CLEAR_MAX = GLOBAL_SH_FS_CLEAR0 + MAX_RTS,
   GLOBAL_SH_COUNT,
};

/**
 * Tracks the results from an individual renderpass. Initially created
 * per renderpass, and appended to the tail of at->pending_results. At a later
 * time, when the GPU has finished writing the results, we fill samples_passed.
 */
struct tu_renderpass_result {
   /* Points into GPU memory */
   struct tu_renderpass_samples* samples;

   struct tu_suballoc_bo bo;

   /*
    * Below here, only used internally within autotune
    */
   uint64_t rp_key;
   struct tu_renderpass_history *history;
   struct list_head node;
   uint32_t fence;
   uint64_t samples_passed;
};

#define TU_BORDER_COLOR_COUNT 4096
#define TU_BORDER_COLOR_BUILTIN 6

#define TU_BLIT_SHADER_SIZE 1024

/* This struct defines the layout of the global_bo */
struct tu6_global
{
   /* clear/blit shaders */
   uint32_t shaders[TU_BLIT_SHADER_SIZE];

   uint32_t seqno_dummy;          /* dummy seqno for CP_EVENT_WRITE */
   uint32_t _pad0;
   volatile uint32_t vsc_draw_overflow;
   uint32_t _pad1;
   volatile uint32_t vsc_prim_overflow;
   uint32_t _pad2;
   uint64_t predicate;

   /* scratch space for VPC_SO[i].FLUSH_BASE_LO/HI, start on 32 byte boundary. */
   struct {
      uint32_t offset;
      uint32_t pad[7];
   } flush_base[4];

   ALIGN16 uint32_t cs_indirect_xyz[3];

   volatile uint32_t vtx_stats_query_not_running;

   /* To know when renderpass stats for autotune are valid */
   volatile uint32_t autotune_fence;

   /* For recycling command buffers for dynamic suspend/resume comamnds */
   volatile uint32_t dynamic_rendering_fence;

   volatile uint32_t dbg_one;
   volatile uint32_t dbg_gmem_total_loads;
   volatile uint32_t dbg_gmem_taken_loads;
   volatile uint32_t dbg_gmem_total_stores;
   volatile uint32_t dbg_gmem_taken_stores;

   /* Written from GPU */
   volatile uint32_t breadcrumb_gpu_sync_seqno;
   uint32_t _pad3;
   /* Written from CPU, acknowledges value written from GPU */
   volatile uint32_t breadcrumb_cpu_sync_seqno;
   uint32_t _pad4;

   /* note: larger global bo will be used for customBorderColors */
   struct bcolor_entry bcolor_builtin[TU_BORDER_COLOR_BUILTIN], bcolor[];
};
#define gb_offset(member) offsetof(struct tu6_global, member)
#define global_iova(cmd, member) ((cmd)->device->global_bo->iova + gb_offset(member))

/* extra space in vsc draw/prim streams */
#define VSC_PAD 0x40

struct tu_device
{
   struct vk_device vk;
   struct tu_instance *instance;

   struct tu_queue *queues[TU_MAX_QUEUE_FAMILIES];
   int queue_count[TU_MAX_QUEUE_FAMILIES];

   struct tu_physical_device *physical_device;
   int fd;

   struct ir3_compiler *compiler;

   /* Backup in-memory cache to be used if the app doesn't provide one */
   struct vk_pipeline_cache *mem_cache;

#define MIN_SCRATCH_BO_SIZE_LOG2 12 /* A page */

   /* Currently the kernel driver uses a 32-bit GPU address space, but it
    * should be impossible to go beyond 48 bits.
    */
   struct {
      struct tu_bo *bo;
      mtx_t construct_mtx;
      bool initialized;
   } scratch_bos[48 - MIN_SCRATCH_BO_SIZE_LOG2];

   struct tu_bo *global_bo;

   uint32_t implicit_sync_bo_count;

   /* Device-global BO suballocator for reducing BO management overhead for
    * (read-only) pipeline state.  Synchronized by pipeline_mutex.
    */
   struct tu_suballocator pipeline_suballoc;
   mtx_t pipeline_mutex;

   /* Device-global BO suballocator for reducing BO management for small
    * gmem/sysmem autotune result buffers.  Synchronized by autotune_mutex.
    */
   struct tu_suballocator autotune_suballoc;
   mtx_t autotune_mutex;

   /* the blob seems to always use 8K factor and 128K param sizes, copy them */
#define TU_TESS_FACTOR_SIZE (8 * 1024)
#define TU_TESS_PARAM_SIZE (128 * 1024)
#define TU_TESS_BO_SIZE (TU_TESS_FACTOR_SIZE + TU_TESS_PARAM_SIZE)
   /* Lazily allocated, protected by the device mutex. */
   struct tu_bo *tess_bo;

   struct ir3_shader_variant *global_shader_variants[GLOBAL_SH_COUNT];
   struct ir3_shader *global_shaders[GLOBAL_SH_COUNT];
   uint64_t global_shader_va[GLOBAL_SH_COUNT];

   uint32_t vsc_draw_strm_pitch;
   uint32_t vsc_prim_strm_pitch;
   BITSET_DECLARE(custom_border_color, TU_BORDER_COLOR_COUNT);
   mtx_t mutex;

   /* bo list for submits: */
   struct drm_msm_gem_submit_bo *bo_list;
   /* map bo handles to bo list index: */
   uint32_t bo_count, bo_list_size;
   mtx_t bo_mutex;
   /* protects imported BOs creation/freeing */
   struct u_rwlock dma_bo_lock;

   /* This array holds all our 'struct tu_bo' allocations. We use this
    * so we can add a refcount to our BOs and check if a particular BO
    * was already allocated in this device using its GEM handle. This is
    * necessary to properly manage BO imports, because the kernel doesn't
    * refcount the underlying BO memory.
    *
    * Specifically, when self-importing (i.e. importing a BO into the same
    * device that created it), the kernel will give us the same BO handle
    * for both BOs and we must only free it once when  both references are
    * freed. Otherwise, if we are not self-importing, we get two different BO
    * handles, and we want to free each one individually.
    *
    * The refcount is also useful for being able to maintain BOs across
    * VK object lifetimes, such as pipelines suballocating out of BOs
    * allocated on the device.
    */
   struct util_sparse_array bo_map;

   /* Command streams to set pass index to a scratch reg */
   struct tu_cs *perfcntrs_pass_cs;
   struct tu_cs_entry *perfcntrs_pass_cs_entries;

   struct util_dynarray dynamic_rendering_pending;
   VkCommandPool dynamic_rendering_pool;
   uint32_t dynamic_rendering_fence;

   /* Condition variable for timeline semaphore to notify waiters when a
    * new submit is executed. */
   pthread_cond_t timeline_cond;
   pthread_mutex_t submit_mutex;

   struct tu_autotune autotune;

   struct breadcrumbs_context *breadcrumbs_ctx;

#ifdef ANDROID
   const void *gralloc;
   enum {
      TU_GRALLOC_UNKNOWN,
      TU_GRALLOC_CROS,
      TU_GRALLOC_OTHER,
   } gralloc_type;
#endif

   uint32_t submit_count;

   struct u_trace_context trace_context;

   #ifdef HAVE_PERFETTO
   struct tu_perfetto_state perfetto;
   #endif

   bool use_z24uint_s8uint;
};

VkResult
tu_device_submit_deferred_locked(struct tu_device *dev);

uint64_t
tu_device_ticks_to_ns(struct tu_device *dev, uint64_t ts);

static inline struct tu_bo *
tu_device_lookup_bo(struct tu_device *device, uint32_t handle)
{
   return (struct tu_bo *) util_sparse_array_get(&device->bo_map, handle);
}

/* Get a scratch bo for use inside a command buffer. This will always return
 * the same bo given the same size or similar sizes, so only one scratch bo
 * can be used at the same time. It's meant for short-lived things where we
 * need to write to some piece of memory, read from it, and then immediately
 * discard it.
 */
VkResult
tu_get_scratch_bo(struct tu_device *dev, uint64_t size, struct tu_bo **bo);

enum tu_draw_state_group_id
{
   TU_DRAW_STATE_PROGRAM_CONFIG,
   TU_DRAW_STATE_PROGRAM,
   TU_DRAW_STATE_PROGRAM_BINNING,
   TU_DRAW_STATE_VB,
   TU_DRAW_STATE_VI,
   TU_DRAW_STATE_VI_BINNING,
   TU_DRAW_STATE_RAST,
   TU_DRAW_STATE_CONST,
   TU_DRAW_STATE_DESC_SETS,
   TU_DRAW_STATE_DESC_SETS_LOAD,
   TU_DRAW_STATE_VS_PARAMS,
   TU_DRAW_STATE_INPUT_ATTACHMENTS_GMEM,
   TU_DRAW_STATE_INPUT_ATTACHMENTS_SYSMEM,
   TU_DRAW_STATE_LRZ_AND_DEPTH_PLANE,
   TU_DRAW_STATE_PRIM_MODE_GMEM,
   TU_DRAW_STATE_PRIM_MODE_SYSMEM,

   /* dynamic state related draw states */
   TU_DRAW_STATE_DYNAMIC,
   TU_DRAW_STATE_COUNT = TU_DRAW_STATE_DYNAMIC + TU_DYNAMIC_STATE_COUNT,
};

struct tu_device_memory
{
   struct vk_object_base base;

   struct tu_bo *bo;
};

struct tu_buffer
{
   struct vk_object_base base;

   VkDeviceSize size;

   VkBufferUsageFlags usage;
   VkBufferCreateFlags flags;

   struct tu_bo *bo;
   uint64_t iova;
};

const char *
tu_get_debug_option_name(int id);

const char *
tu_get_perftest_option_name(int id);

struct tu_attachment_info
{
   struct tu_image_view *attachment;
};

struct tu_tiling_config {
   /* size of the first tile */
   VkExtent2D tile0;
   /* number of tiles */
   VkExtent2D tile_count;

   /* size of the first VSC pipe */
   VkExtent2D pipe0;
   /* number of VSC pipes */
   VkExtent2D pipe_count;

   /* Whether binning should be used for gmem rendering using this framebuffer. */
   bool binning;

   /* Whether binning could be used for gmem rendering using this framebuffer. */
   bool binning_possible;

   /* pipe register values */
   uint32_t pipe_config[MAX_VSC_PIPES];
   uint32_t pipe_sizes[MAX_VSC_PIPES];
};

struct tu_framebuffer
{
   struct vk_object_base base;

   uint32_t width;
   uint32_t height;
   uint32_t layers;

   struct tu_tiling_config tiling[TU_GMEM_LAYOUT_COUNT];

   uint32_t attachment_count;
   struct tu_attachment_info attachments[0];
};

void
tu_framebuffer_tiling_config(struct tu_framebuffer *fb,
                             const struct tu_device *device,
                             const struct tu_render_pass *pass);

struct tu_descriptor_state
{
   struct tu_descriptor_set *sets[MAX_SETS];
   struct tu_descriptor_set push_set;
   uint32_t dynamic_descriptors[MAX_DYNAMIC_BUFFERS_SIZE];
};

enum tu_cmd_dirty_bits
{
   TU_CMD_DIRTY_VERTEX_BUFFERS = BIT(0),
   TU_CMD_DIRTY_VB_STRIDE = BIT(1),
   TU_CMD_DIRTY_GRAS_SU_CNTL = BIT(2),
   TU_CMD_DIRTY_RB_DEPTH_CNTL = BIT(3),
   TU_CMD_DIRTY_RB_STENCIL_CNTL = BIT(4),
   TU_CMD_DIRTY_DESC_SETS_LOAD = BIT(5),
   TU_CMD_DIRTY_COMPUTE_DESC_SETS_LOAD = BIT(6),
   TU_CMD_DIRTY_SHADER_CONSTS = BIT(7),
   TU_CMD_DIRTY_LRZ = BIT(8),
   TU_CMD_DIRTY_VS_PARAMS = BIT(9),
   TU_CMD_DIRTY_RASTERIZER_DISCARD = BIT(10),
   TU_CMD_DIRTY_VIEWPORTS = BIT(11),
   TU_CMD_DIRTY_BLEND = BIT(12),
   /* all draw states were disabled and need to be re-enabled: */
   TU_CMD_DIRTY_DRAW_STATE = BIT(13)
};

/* There are only three cache domains we have to care about: the CCU, or
 * color cache unit, which is used for color and depth/stencil attachments
 * and copy/blit destinations, and is split conceptually into color and depth,
 * and the universal cache or UCHE which is used for pretty much everything
 * else, except for the CP (uncached) and host. We need to flush whenever data
 * crosses these boundaries.
 */

enum tu_cmd_access_mask {
   TU_ACCESS_UCHE_READ = 1 << 0,
   TU_ACCESS_UCHE_WRITE = 1 << 1,
   TU_ACCESS_CCU_COLOR_READ = 1 << 2,
   TU_ACCESS_CCU_COLOR_WRITE = 1 << 3,
   TU_ACCESS_CCU_DEPTH_READ = 1 << 4,
   TU_ACCESS_CCU_DEPTH_WRITE = 1 << 5,

   /* Experiments have shown that while it's safe to avoid flushing the CCU
    * after each blit/renderpass, it's not safe to assume that subsequent
    * lookups with a different attachment state will hit unflushed cache
    * entries. That is, the CCU needs to be flushed and possibly invalidated
    * when accessing memory with a different attachment state. Writing to an
    * attachment under the following conditions after clearing using the
    * normal 2d engine path is known to have issues:
    *
    * - It isn't the 0'th layer.
    * - There are more than one attachment, and this isn't the 0'th attachment
    *   (this seems to also depend on the cpp of the attachments).
    *
    * Our best guess is that the layer/MRT state is used when computing
    * the location of a cache entry in CCU, to avoid conflicts. We assume that
    * any access in a renderpass after or before an access by a transfer needs
    * a flush/invalidate, and use the _INCOHERENT variants to represent access
    * by a renderpass.
    */
   TU_ACCESS_CCU_COLOR_INCOHERENT_READ = 1 << 6,
   TU_ACCESS_CCU_COLOR_INCOHERENT_WRITE = 1 << 7,
   TU_ACCESS_CCU_DEPTH_INCOHERENT_READ = 1 << 8,
   TU_ACCESS_CCU_DEPTH_INCOHERENT_WRITE = 1 << 9,

   /* Accesses which bypasses any cache. e.g. writes via the host,
    * CP_EVENT_WRITE::BLIT, and the CP are SYSMEM_WRITE.
    */
   TU_ACCESS_SYSMEM_READ = 1 << 10,
   TU_ACCESS_SYSMEM_WRITE = 1 << 11,

   /* Memory writes from the CP start in-order with draws and event writes,
    * but execute asynchronously and hence need a CP_WAIT_MEM_WRITES if read.
    */
   TU_ACCESS_CP_WRITE = 1 << 12,

   TU_ACCESS_READ =
      TU_ACCESS_UCHE_READ |
      TU_ACCESS_CCU_COLOR_READ |
      TU_ACCESS_CCU_DEPTH_READ |
      TU_ACCESS_CCU_COLOR_INCOHERENT_READ |
      TU_ACCESS_CCU_DEPTH_INCOHERENT_READ |
      TU_ACCESS_SYSMEM_READ,

   TU_ACCESS_WRITE =
      TU_ACCESS_UCHE_WRITE |
      TU_ACCESS_CCU_COLOR_WRITE |
      TU_ACCESS_CCU_COLOR_INCOHERENT_WRITE |
      TU_ACCESS_CCU_DEPTH_WRITE |
      TU_ACCESS_CCU_DEPTH_INCOHERENT_WRITE |
      TU_ACCESS_SYSMEM_WRITE |
      TU_ACCESS_CP_WRITE,

   TU_ACCESS_ALL =
      TU_ACCESS_READ |
      TU_ACCESS_WRITE,
};

/* Starting with a6xx, the pipeline is split into several "clusters" (really
 * pipeline stages). Each stage has its own pair of register banks and can
 * switch them independently, so that earlier stages can run ahead of later
 * ones. e.g. the FS of draw N and the VS of draw N + 1 can be executing at
 * the same time.
 *
 * As a result of this, we need to insert a WFI when an earlier stage depends
 * on the result of a later stage. CP_DRAW_* and CP_BLIT will wait for any
 * pending WFI's to complete before starting, and usually before reading
 * indirect params even, so a WFI also acts as a full "pipeline stall".
 *
 * Note, the names of the stages come from CLUSTER_* in devcoredump. We
 * include all the stages for completeness, even ones which do not read/write
 * anything.
 */

enum tu_stage {
   /* This doesn't correspond to a cluster, but we need it for tracking
    * indirect draw parameter reads etc.
    */
   TU_STAGE_CP,

   /* - Fetch index buffer
    * - Fetch vertex attributes, dispatch VS
    */
   TU_STAGE_FE,

   /* Execute all geometry stages (VS thru GS) */
   TU_STAGE_SP_VS,

   /* Write to VPC, do primitive assembly. */
   TU_STAGE_PC_VS,

   /* Rasterization. RB_DEPTH_BUFFER_BASE only exists in CLUSTER_PS according
    * to devcoredump so presumably this stage stalls for TU_STAGE_PS when
    * early depth testing is enabled before dispatching fragments? However
    * GRAS reads and writes LRZ directly.
    */
   TU_STAGE_GRAS,

   /* Execute FS */
   TU_STAGE_SP_PS,

   /* - Fragment tests
    * - Write color/depth
    * - Streamout writes (???)
    * - Varying interpolation (???)
    */
   TU_STAGE_PS,
};

enum tu_cmd_flush_bits {
   TU_CMD_FLAG_CCU_FLUSH_DEPTH = 1 << 0,
   TU_CMD_FLAG_CCU_FLUSH_COLOR = 1 << 1,
   TU_CMD_FLAG_CCU_INVALIDATE_DEPTH = 1 << 2,
   TU_CMD_FLAG_CCU_INVALIDATE_COLOR = 1 << 3,
   TU_CMD_FLAG_CACHE_FLUSH = 1 << 4,
   TU_CMD_FLAG_CACHE_INVALIDATE = 1 << 5,
   TU_CMD_FLAG_WAIT_MEM_WRITES = 1 << 6,
   TU_CMD_FLAG_WAIT_FOR_IDLE = 1 << 7,
   TU_CMD_FLAG_WAIT_FOR_ME = 1 << 8,

   TU_CMD_FLAG_ALL_FLUSH =
      TU_CMD_FLAG_CCU_FLUSH_DEPTH |
      TU_CMD_FLAG_CCU_FLUSH_COLOR |
      TU_CMD_FLAG_CACHE_FLUSH |
      /* Treat the CP as a sort of "cache" which may need to be "flushed" via
       * waiting for writes to land with WAIT_FOR_MEM_WRITES.
       */
      TU_CMD_FLAG_WAIT_MEM_WRITES,

   TU_CMD_FLAG_ALL_INVALIDATE =
      TU_CMD_FLAG_CCU_INVALIDATE_DEPTH |
      TU_CMD_FLAG_CCU_INVALIDATE_COLOR |
      TU_CMD_FLAG_CACHE_INVALIDATE |
      /* Treat CP_WAIT_FOR_ME as a "cache" that needs to be invalidated when a
       * a command that needs CP_WAIT_FOR_ME is executed. This means we may
       * insert an extra WAIT_FOR_ME before an indirect command requiring it
       * in case there was another command before the current command buffer
       * that it needs to wait for.
       */
      TU_CMD_FLAG_WAIT_FOR_ME,
};

/* Changing the CCU from sysmem mode to gmem mode or vice-versa is pretty
 * heavy, involving a CCU cache flush/invalidate and a WFI in order to change
 * which part of the gmem is used by the CCU. Here we keep track of what the
 * state of the CCU.
 */
enum tu_cmd_ccu_state {
   TU_CMD_CCU_SYSMEM,
   TU_CMD_CCU_GMEM,
   TU_CMD_CCU_UNKNOWN,
};

struct tu_cache_state {
   /* Caches which must be made available (flushed) eventually if there are
    * any users outside that cache domain, and caches which must be
    * invalidated eventually if there are any reads.
    */
   enum tu_cmd_flush_bits pending_flush_bits;
   /* Pending flushes */
   enum tu_cmd_flush_bits flush_bits;
};

struct tu_vs_params {
   uint32_t vertex_offset;
   uint32_t first_instance;
};

/* This should be for state that is set inside a renderpass and used at
 * renderpass end time, e.g. to decide whether to use sysmem. This needs
 * special handling for secondary cmdbufs and suspending/resuming render
 * passes where the state may need to be combined afterwards.
 */
struct tu_render_pass_state
{
   bool xfb_used;
   bool has_tess;
   bool has_prim_generated_query_in_rp;
   bool disable_gmem;

   /* Track whether conditional predicate for COND_REG_EXEC is changed in draw_cs */
   bool draw_cs_writes_to_cond_pred;

   uint32_t drawcall_count;

   /* A calculated "draw cost" value for renderpass, which tries to
    * estimate the bandwidth-per-sample of all the draws according
    * to:
    *
    *    foreach_draw (...) {
    *      sum += pipeline->color_bandwidth_per_sample;
    *      if (depth_test_enabled)
    *        sum += pipeline->depth_cpp_per_sample;
    *      if (depth_write_enabled)
    *        sum += pipeline->depth_cpp_per_sample;
    *      if (stencil_write_enabled)
    *        sum += pipeline->stencil_cpp_per_sample * 2;
    *    }
    *    drawcall_bandwidth_per_sample = sum / drawcall_count;
    *
    * It allows us to estimate the total bandwidth of drawcalls later, by
    * calculating (drawcall_bandwidth_per_sample * zpass_sample_count).
    *
    * This does ignore depth buffer traffic for samples which do not
    * pass due to depth-test fail, and some other details.  But it is
    * just intended to be a rough estimate that is easy to calculate.
    */
   uint32_t drawcall_bandwidth_per_sample_sum;
};

void tu_render_pass_state_merge(struct tu_render_pass_state *dst,
                                const struct tu_render_pass_state *src);
struct tu_cmd_state
{
   uint32_t dirty;

   struct tu_pipeline *pipeline;
   struct tu_pipeline *compute_pipeline;

   struct tu_render_pass_state rp;

   /* Vertex buffers, viewports, and scissors
    * the states for these can be updated partially, so we need to save these
    * to be able to emit a complete draw state
    */
   struct {
      uint64_t base;
      uint32_t size;
      uint32_t stride;
   } vb[MAX_VBS];
   VkViewport viewport[MAX_VIEWPORTS];
   VkRect2D scissor[MAX_SCISSORS];
   uint32_t max_viewport, max_scissor;

   /* for dynamic states that can't be emitted directly */
   uint32_t dynamic_stencil_mask;
   uint32_t dynamic_stencil_wrmask;
   uint32_t dynamic_stencil_ref;

   uint32_t gras_su_cntl, rb_depth_cntl, rb_stencil_cntl;
   uint32_t pc_raster_cntl, vpc_unknown_9107;
   uint32_t rb_mrt_control[MAX_RTS], rb_mrt_blend_control[MAX_RTS];
   uint32_t rb_mrt_control_rop;
   uint32_t rb_blend_cntl, sp_blend_cntl;
   uint32_t pipeline_color_write_enable, pipeline_blend_enable;
   uint32_t color_write_enable;
   bool logic_op_enabled;
   bool rop_reads_dst;
   enum pc_di_primtype primtype;
   bool primitive_restart_enable;

   /* saved states to re-emit in TU_CMD_DIRTY_DRAW_STATE case */
   struct tu_draw_state dynamic_state[TU_DYNAMIC_STATE_COUNT];
   struct tu_draw_state vertex_buffers;
   struct tu_draw_state shader_const;
   struct tu_draw_state desc_sets;

   struct tu_draw_state vs_params;

   /* Index buffer */
   uint64_t index_va;
   uint32_t max_index_count;
   uint8_t index_size;

   /* because streamout base has to be 32-byte aligned
    * there is an extra offset to deal with when it is
    * unaligned
    */
   uint8_t streamout_offset[IR3_MAX_SO_BUFFERS];

   /* Renderpasses are tricky, because we may need to flush differently if
    * using sysmem vs. gmem and therefore we have to delay any flushing that
    * happens before a renderpass. So we have to have two copies of the flush
    * state, one for intra-renderpass flushes (i.e. renderpass dependencies)
    * and one for outside a renderpass.
    */
   struct tu_cache_state cache;
   struct tu_cache_state renderpass_cache;

   enum tu_cmd_ccu_state ccu_state;

   /* Decides which GMEM layout to use from the tu_pass, based on whether the CCU
    * might get used by tu_store_gmem_attachment().
    */
   enum tu_gmem_layout gmem_layout;

   const struct tu_render_pass *pass;
   const struct tu_subpass *subpass;
   const struct tu_framebuffer *framebuffer;
   const struct tu_tiling_config *tiling;
   VkRect2D render_area;

   const struct tu_image_view **attachments;

   /* State that in the dynamic case comes from VkRenderingInfo and needs to
    * be saved/restored when suspending. This holds the state for the last
    * suspended renderpass, which may point to this command buffer's dynamic_*
    * or another command buffer if executed on a secondary.
    */
   struct {
      const struct tu_render_pass *pass;
      const struct tu_subpass *subpass;
      const struct tu_framebuffer *framebuffer;
      VkRect2D render_area;
      enum tu_gmem_layout gmem_layout;

      const struct tu_image_view **attachments;

      struct tu_lrz_state lrz;
   } suspended_pass;

   bool tessfactor_addr_set;
   bool predication_active;
   enum a5xx_line_mode line_mode;
   bool z_negative_one_to_one;

   /* VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT and
    * VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT are allowed to run simultaniously,
    * but they use the same {START,STOP}_PRIMITIVE_CTRS control.
    */
   uint32_t prim_counters_running;

   bool prim_generated_query_running_before_rp;

   /* These are the states of the suspend/resume state machine. In addition to
    * tracking whether we're in the middle of a chain of suspending and
    * resuming passes that will be merged, we need to track whether the
    * command buffer begins in the middle of such a chain, for when it gets
    * merged with other command buffers. We call such a chain that begins
    * before the command buffer starts a "pre-chain".
    *
    * Note that when this command buffer is finished, this state is untouched
    * but it gains a different meaning. For example, if we finish in state
    * SR_IN_CHAIN, we finished in the middle of a suspend/resume chain, so
    * there's a suspend/resume chain that extends past the end of the command
    * buffer. In this sense it's the "opposite" of SR_AFTER_PRE_CHAIN, which
    * means that there's a suspend/resume chain that extends before the
    * beginning.
    */
   enum {
      /* Either there are no suspend/resume chains, or they are entirely
       * contained in the current command buffer.
       *
       *   BeginCommandBuffer() <- start of current command buffer
       *       ...
       *       // we are here
       */
      SR_NONE = 0,

      /* We are in the middle of a suspend/resume chain that starts before the
       * current command buffer. This happens when the command buffer begins
       * with a resuming render pass and all of the passes up to the current
       * one are suspending. In this state, our part of the chain is not saved
       * and is in the current draw_cs/state.
       *
       *   BeginRendering() ... EndRendering(suspending)
       *   BeginCommandBuffer() <- start of current command buffer
       *       BeginRendering(resuming) ... EndRendering(suspending)
       *       BeginRendering(resuming) ... EndRendering(suspending)
       *       ...
       *       // we are here
       */
      SR_IN_PRE_CHAIN,

      /* We are currently outside of any suspend/resume chains, but there is a
       * chain starting before the current command buffer. It is saved in
       * pre_chain.
       *
       *   BeginRendering() ... EndRendering(suspending)
       *   BeginCommandBuffer() <- start of current command buffer
       *       // This part is stashed in pre_chain
       *       BeginRendering(resuming) ... EndRendering(suspending)
       *       BeginRendering(resuming) ... EndRendering(suspending)
       *       ...
       *       BeginRendering(resuming) ... EndRendering() // end of chain
       *       ...
       *       // we are here
       */
      SR_AFTER_PRE_CHAIN,

      /* We are in the middle of a suspend/resume chain and there is no chain
       * starting before the current command buffer.
       *
       *   BeginCommandBuffer() <- start of current command buffer
       *       ...
       *       BeginRendering() ... EndRendering(suspending)
       *       BeginRendering(resuming) ... EndRendering(suspending)
       *       BeginRendering(resuming) ... EndRendering(suspending)
       *       ...
       *       // we are here
       */
      SR_IN_CHAIN,

      /* We are in the middle of a suspend/resume chain and there is another,
       * separate, chain starting before the current command buffer.
       *
       *   BeginRendering() ... EndRendering(suspending)
       *   CommandBufferBegin() <- start of current command buffer
       *       // This part is stashed in pre_chain
       *       BeginRendering(resuming) ... EndRendering(suspending)
       *       BeginRendering(resuming) ... EndRendering(suspending)
       *       ...
       *       BeginRendering(resuming) ... EndRendering() // end of chain
       *       ...
       *       BeginRendering() ... EndRendering(suspending)
       *       BeginRendering(resuming) ... EndRendering(suspending)
       *       BeginRendering(resuming) ... EndRendering(suspending)
       *       ...
       *       // we are here
       */
      SR_IN_CHAIN_AFTER_PRE_CHAIN,
   } suspend_resume;

   bool suspending, resuming;

   struct tu_lrz_state lrz;

   struct tu_draw_state lrz_and_depth_plane_state;

   struct tu_vs_params last_vs_params;
};

struct tu_cmd_pool
{
   struct vk_command_pool vk;

   struct list_head cmd_buffers;
   struct list_head free_cmd_buffers;
};

enum tu_cmd_buffer_status
{
   TU_CMD_BUFFER_STATUS_INVALID,
   TU_CMD_BUFFER_STATUS_INITIAL,
   TU_CMD_BUFFER_STATUS_RECORDING,
   TU_CMD_BUFFER_STATUS_EXECUTABLE,
   TU_CMD_BUFFER_STATUS_PENDING,
};

struct tu_cmd_buffer
{
   struct vk_command_buffer vk;

   struct tu_device *device;

   struct tu_cmd_pool *pool;
   struct list_head pool_link;

   struct u_trace trace;
   struct u_trace_iterator trace_renderpass_start;
   struct u_trace_iterator trace_renderpass_end;

   struct list_head renderpass_autotune_results;
   struct tu_autotune_results_buffer* autotune_buffer;

   VkCommandBufferUsageFlags usage_flags;
   enum tu_cmd_buffer_status status;

   VkQueryPipelineStatisticFlags inherited_pipeline_statistics;

   struct tu_cmd_state state;
   uint32_t queue_family_index;

   uint32_t push_constants[MAX_PUSH_CONSTANTS_SIZE / 4];
   VkShaderStageFlags push_constant_stages;
   struct tu_descriptor_set meta_push_descriptors;

   struct tu_descriptor_state descriptors[MAX_BIND_POINTS];

   struct tu_render_pass_attachment dynamic_rp_attachments[2 * (MAX_RTS + 1)];
   struct tu_subpass_attachment dynamic_color_attachments[MAX_RTS];
   struct tu_subpass_attachment dynamic_resolve_attachments[MAX_RTS + 1];
   const struct tu_image_view *dynamic_attachments[2 * (MAX_RTS + 1)];

   struct tu_render_pass dynamic_pass;
   struct tu_subpass dynamic_subpass;
   struct tu_framebuffer dynamic_framebuffer;

   VkResult record_result;

   struct tu_cs cs;
   struct tu_cs draw_cs;
   struct tu_cs tile_store_cs;
   struct tu_cs draw_epilogue_cs;
   struct tu_cs sub_cs;

   /* If the first render pass in the command buffer is resuming, then it is
    * part of a suspend/resume chain that starts before the current command
    * buffer and needs to be merged later. In this case, its incomplete state
    * is stored in pre_chain. In the symmetric case where the last render pass
    * is suspending, we just skip ending the render pass and its state is
    * stored in draw_cs/the current state. The first and last render pass
    * might be part of different chains, which is why all the state may need
    * to be saved separately here.
    */
   struct {
      struct tu_cs draw_cs;
      struct tu_cs draw_epilogue_cs;

      struct u_trace_iterator trace_renderpass_start, trace_renderpass_end;

      struct tu_render_pass_state state;
   } pre_chain;

   uint32_t vsc_draw_strm_pitch;
   uint32_t vsc_prim_strm_pitch;
};

static inline uint32_t
tu_attachment_gmem_offset(struct tu_cmd_buffer *cmd,
                          const struct tu_render_pass_attachment *att)
{
   assert(cmd->state.gmem_layout < TU_GMEM_LAYOUT_COUNT);
   return att->gmem_offset[cmd->state.gmem_layout];
}

static inline uint32_t
tu_attachment_gmem_offset_stencil(struct tu_cmd_buffer *cmd,
                                  const struct tu_render_pass_attachment *att)
{
   assert(cmd->state.gmem_layout < TU_GMEM_LAYOUT_COUNT);
   return att->gmem_offset_stencil[cmd->state.gmem_layout];
}

VkResult tu_cmd_buffer_begin(struct tu_cmd_buffer *cmd_buffer,
                             VkCommandBufferUsageFlags usage_flags);

void tu_emit_cache_flush_renderpass(struct tu_cmd_buffer *cmd_buffer,
                                    struct tu_cs *cs);

void tu_emit_cache_flush_ccu(struct tu_cmd_buffer *cmd_buffer,
                             struct tu_cs *cs,
                             enum tu_cmd_ccu_state ccu_state);

void tu_setup_dynamic_framebuffer(struct tu_cmd_buffer *cmd_buffer,
                                  const VkRenderingInfo *pRenderingInfo);

void
tu_append_pre_chain(struct tu_cmd_buffer *cmd,
                    struct tu_cmd_buffer *secondary);

void
tu_append_pre_post_chain(struct tu_cmd_buffer *cmd,
                         struct tu_cmd_buffer *secondary);

void
tu_append_post_chain(struct tu_cmd_buffer *cmd,
                     struct tu_cmd_buffer *secondary);

void
tu_restore_suspended_pass(struct tu_cmd_buffer *cmd,
                          struct tu_cmd_buffer *suspended);

void tu_cmd_render(struct tu_cmd_buffer *cmd);

void
tu6_emit_event_write(struct tu_cmd_buffer *cmd,
                     struct tu_cs *cs,
                     enum vgt_event_type event);

static inline struct tu_descriptor_state *
tu_get_descriptors_state(struct tu_cmd_buffer *cmd_buffer,
                         VkPipelineBindPoint bind_point)
{
   return &cmd_buffer->descriptors[bind_point];
}

struct tu_event
{
   struct vk_object_base base;
   struct tu_bo *bo;
};

void tu6_emit_msaa(struct tu_cs *cs, VkSampleCountFlagBits samples,
                   enum a5xx_line_mode line_mode);

void tu6_emit_window_scissor(struct tu_cs *cs, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2);

void tu6_emit_window_offset(struct tu_cs *cs, uint32_t x1, uint32_t y1);

void tu_disable_draw_states(struct tu_cmd_buffer *cmd, struct tu_cs *cs);

void tu6_apply_depth_bounds_workaround(struct tu_device *device,
                                       uint32_t *rb_depth_cntl);

struct tu_sampler {
   struct vk_object_base base;

   uint32_t descriptor[A6XX_TEX_SAMP_DWORDS];
   struct tu_sampler_ycbcr_conversion *ycbcr_sampler;
};

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

VkResult
tu_physical_device_init(struct tu_physical_device *device,
                        struct tu_instance *instance);
void
tu_copy_timestamp_buffer(struct u_trace_context *utctx, void *cmdstream,
                         void *ts_from, uint32_t from_offset,
                         void *ts_to, uint32_t to_offset,
                         uint32_t count);


VkResult
tu_create_copy_timestamp_cs(struct tu_cmd_buffer *cmdbuf, struct tu_cs** cs,
                            struct u_trace **trace_copy);

/* If we copy trace and timestamps we will have to free them. */
struct tu_u_trace_cmd_data
{
   struct tu_cs *timestamp_copy_cs;
   struct u_trace *trace;
};

/* Data necessary to retrieve timestamps and clean all
 * associated resources afterwards.
 */
struct tu_u_trace_submission_data
{
   uint32_t submission_id;
   /* We have to know when timestamps are available,
    * this sync object indicates it.
    */
   struct tu_u_trace_syncobj *syncobj;

   uint32_t cmd_buffer_count;
   uint32_t last_buffer_with_tracepoints;
   struct tu_u_trace_cmd_data *cmd_trace_data;
};

VkResult
tu_u_trace_submission_data_create(
   struct tu_device *device,
   struct tu_cmd_buffer **cmd_buffers,
   uint32_t cmd_buffer_count,
   struct tu_u_trace_submission_data **submission_data);

void
tu_u_trace_submission_data_finish(
   struct tu_device *device,
   struct tu_u_trace_submission_data *submission_data);

VK_DEFINE_HANDLE_CASTS(tu_cmd_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)
VK_DEFINE_HANDLE_CASTS(tu_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)
VK_DEFINE_HANDLE_CASTS(tu_instance, vk.base, VkInstance,
                       VK_OBJECT_TYPE_INSTANCE)
VK_DEFINE_HANDLE_CASTS(tu_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)
VK_DEFINE_HANDLE_CASTS(tu_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)

VK_DEFINE_NONDISP_HANDLE_CASTS(tu_cmd_pool, vk.base, VkCommandPool,
                               VK_OBJECT_TYPE_COMMAND_POOL)
VK_DEFINE_NONDISP_HANDLE_CASTS(tu_buffer, base, VkBuffer,
                               VK_OBJECT_TYPE_BUFFER)
VK_DEFINE_NONDISP_HANDLE_CASTS(tu_device_memory, base, VkDeviceMemory,
                               VK_OBJECT_TYPE_DEVICE_MEMORY)
VK_DEFINE_NONDISP_HANDLE_CASTS(tu_event, base, VkEvent, VK_OBJECT_TYPE_EVENT)
VK_DEFINE_NONDISP_HANDLE_CASTS(tu_framebuffer, base, VkFramebuffer,
                               VK_OBJECT_TYPE_FRAMEBUFFER)
VK_DEFINE_NONDISP_HANDLE_CASTS(tu_sampler, base, VkSampler,
                               VK_OBJECT_TYPE_SAMPLER)

void
update_stencil_mask(uint32_t *value, VkStencilFaceFlags face, uint32_t mask);

#endif /* TU_PRIVATE_H */
