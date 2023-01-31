#include "nvk_query_pool.h"

#include "nvk_cmd_buffer.h"
#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "util/os_time.h"

#include "nouveau_bo.h"

#include "nvk_cl9097.h"
#include "nvk_cl906f.h"

struct nvk_query_report {
   uint64_t value;
   uint64_t timestamp;
};

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateQueryPool(VkDevice device,
                    const VkQueryPoolCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkQueryPool *pQueryPool)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   struct nvk_query_pool *pool;

   pool = vk_query_pool_create(&dev->vk, pCreateInfo,
                               pAllocator, sizeof(*pool));
   if (!pool)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* We place the availability first and then data */
   pool->query_start = ALIGN_POT(pool->vk.query_count * sizeof(uint32_t),
                                 sizeof(struct nvk_query_report));

   uint32_t reports_per_query;
   switch (pCreateInfo->queryType) {
   case VK_QUERY_TYPE_OCCLUSION:
      reports_per_query = 2;
      break;
   case VK_QUERY_TYPE_TIMESTAMP:
      reports_per_query = 1;
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      reports_per_query = 2 * util_bitcount(pool->vk.pipeline_statistics);
      break;
   default:
      unreachable("Unsupported query type");
   }
   pool->query_stride = reports_per_query * sizeof(struct nvk_query_report);

   if (pool->vk.query_count > 0) {
      uint32_t bo_size = pool->query_start +
                         pool->query_stride * pool->vk.query_count;
      pool->bo = nouveau_ws_bo_new_mapped(dev->pdev->dev, bo_size, 0,
                                          NOUVEAU_WS_BO_GART,
                                          NOUVEAU_WS_BO_RDWR,
                                          &pool->bo_map);
      if (!pool->bo) {
         vk_query_pool_destroy(&dev->vk, pAllocator, &pool->vk);
         return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }

      if (dev->pdev->dev->debug_flags & NVK_DEBUG_ZERO_MEMORY)
         memset(pool->bo_map, 0, bo_size);
   }

   *pQueryPool = nvk_query_pool_to_handle(pool);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyQueryPool(VkDevice device,
                     VkQueryPool queryPool,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_query_pool, pool, queryPool);

   if (!pool)
      return;

   if (pool->bo) {
      nouveau_ws_bo_unmap(pool->bo, pool->bo_map);
      nouveau_ws_bo_destroy(pool->bo);
   }
   vk_query_pool_destroy(&dev->vk, pAllocator, &pool->vk);
}

static uint64_t
nvk_query_available_addr(struct nvk_query_pool *pool, uint32_t query)
{
   assert(query < pool->vk.query_count);
   return pool->bo->offset + query * sizeof(uint32_t);
}

static uint32_t *
nvk_query_available_map(struct nvk_query_pool *pool, uint32_t query)
{
   assert(query < pool->vk.query_count);
   return (uint32_t *)pool->bo_map + query;
}

static uint64_t
nvk_query_offset(struct nvk_query_pool *pool, uint32_t query)
{
   assert(query < pool->vk.query_count);
   return pool->query_start + query * pool->query_stride;
}

static uint64_t
nvk_query_report_addr(struct nvk_query_pool *pool, uint32_t query)
{
   return pool->bo->offset + nvk_query_offset(pool, query);
}

static struct nvk_query_report *
nvk_query_report_map(struct nvk_query_pool *pool, uint32_t query)
{
   return (void *)((char *)pool->bo_map + nvk_query_offset(pool, query));
}

VKAPI_ATTR void VKAPI_CALL
nvk_ResetQueryPool(VkDevice device,
                   VkQueryPool queryPool,
                   uint32_t firstQuery,
                   uint32_t queryCount)
{
   VK_FROM_HANDLE(nvk_query_pool, pool, queryPool);

   uint32_t *available = nvk_query_available_map(pool, firstQuery);
   memset(available, 0, queryCount * sizeof(*available));
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdResetQueryPool(VkCommandBuffer commandBuffer,
                      VkQueryPool queryPool,
                      uint32_t firstQuery,
                      uint32_t queryCount)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_query_pool, pool, queryPool);

   nvk_cmd_buffer_ref_bo(cmd, pool->bo);

   for (uint32_t i = 0; i < queryCount; i++) {
      uint64_t addr = nvk_query_available_addr(pool, firstQuery + i);

      struct nv_push *p = nvk_cmd_buffer_push(cmd, 5);
      P_MTHD(p, NV9097, SET_REPORT_SEMAPHORE_A);
      P_NV9097_SET_REPORT_SEMAPHORE_A(p, addr >> 32);
      P_NV9097_SET_REPORT_SEMAPHORE_B(p, addr);
      P_NV9097_SET_REPORT_SEMAPHORE_C(p, 0);
      P_NV9097_SET_REPORT_SEMAPHORE_D(p, {
         .operation = OPERATION_RELEASE,
         .release = RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE,
         .pipeline_location = PIPELINE_LOCATION_ALL,
         .structure_size = STRUCTURE_SIZE_ONE_WORD,
      });
   }

   /* Wait for the above writes to complete.  This prevents WaW hazards on any
    * later query availability updates and ensures vkCmdCopyQueryPoolResults
    * will see the query as unavailable if it happens before the query is
    * completed again.
    */
   for (uint32_t i = 0; i < queryCount; i++) {
      uint64_t addr = nvk_query_available_addr(pool, firstQuery + i);

      struct nv_push *p = nvk_cmd_buffer_push(cmd, 5);
      __push_mthd(p, SUBC_NV9097, NV906F_SEMAPHOREA);
      P_NV906F_SEMAPHOREA(p, addr >> 32);
      P_NV906F_SEMAPHOREB(p, (addr & UINT32_MAX) >> 2);
      P_NV906F_SEMAPHOREC(p, 0);
      P_NV906F_SEMAPHORED(p, {
         .operation = OPERATION_ACQUIRE,
         .acquire_switch = ACQUIRE_SWITCH_ENABLED,
         .release_size = RELEASE_SIZE_4BYTE,
      });
   }
}

static bool
nvk_query_is_available(struct nvk_query_pool *pool, uint32_t query)
{
   uint32_t *available = nvk_query_available_map(pool, query);
   return p_atomic_read(available) != 0;
}

#define NVK_QUERY_TIMEOUT 2000000000ull

static VkResult
nvk_query_wait_for_available(struct nvk_device *dev,
                             struct nvk_query_pool *pool,
                             uint32_t query)
{
   uint64_t abs_timeout_ns = os_time_get_absolute_timeout(NVK_QUERY_TIMEOUT);

   while (os_time_get_nano() < abs_timeout_ns) {
      if (nvk_query_is_available(pool, query))
         return VK_SUCCESS;

      VkResult status = vk_device_check_status(&dev->vk);
      if (status != VK_SUCCESS)
         return status;
   }

   return vk_device_set_lost(&dev->vk, "query timeout");
}

static void
cpu_write_query_result(void *dst, uint32_t idx,
                       VkQueryResultFlags flags,
                       uint64_t result)
{
   if (flags & VK_QUERY_RESULT_64_BIT) {
      uint64_t *dst64 = dst;
      dst64[idx] = result;
   } else {
      uint32_t *dst32 = dst;
      dst32[idx] = result;
   }
}

static void
cpu_get_query_delta(void *dst, const struct nvk_query_report *src,
                    uint32_t idx, VkQueryResultFlags flags)
{
   uint64_t delta = src[idx * 2 + 1].value - src[idx * 2].value;
   cpu_write_query_result(dst, idx, flags, delta);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetQueryPoolResults(VkDevice device,
                        VkQueryPool queryPool,
                        uint32_t firstQuery,
                        uint32_t queryCount,
                        size_t dataSize,
                        void *pData,
                        VkDeviceSize stride,
                        VkQueryResultFlags flags)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_query_pool, pool, queryPool);

   if (vk_device_is_lost(&dev->vk))
      return VK_ERROR_DEVICE_LOST;

   VkResult status = VK_SUCCESS;
   for (uint32_t i = 0; i < queryCount; i++) {
      const uint32_t query = firstQuery + i;

      bool available = nvk_query_is_available(pool, query);

      if (!available && (flags & VK_QUERY_RESULT_WAIT_BIT)) {
         status = nvk_query_wait_for_available(dev, pool, query);
         if (status != VK_SUCCESS)
            return status;

         available = true;
      }

      bool write_results = available || (flags & VK_QUERY_RESULT_PARTIAL_BIT);

      const struct nvk_query_report *src = nvk_query_report_map(pool, query);
      assert(i * stride < dataSize);
      void *dst = (char *)pData + i * stride;

      uint32_t available_dst_idx = 1;
      switch (pool->vk.query_type) {
      default:
         unreachable("Unsupported query type");
      }

      if (!write_results)
         status = VK_NOT_READY;

      if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
         cpu_write_query_result(dst, available_dst_idx, flags, available);
   }

   return status;
}
