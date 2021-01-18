/*
 * Copyright © 2021 Bas Nieuwenhuizen
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
#include "radv_private.h"

#include "util/half_float.h"

struct radv_accel_struct_header {
   uint32_t root_node_offset;
   uint32_t reserved;
   float aabb[2][3];
   uint64_t compacted_size;
   uint64_t serialization_size;
};

struct radv_bvh_triangle_node {
   float coords[3][3];
   uint32_t reserved[3];
   uint32_t triangle_id;
   /* flags in upper 4 bits */
   uint32_t geometry_id_and_flags;
   uint32_t reserved2;
   uint32_t id;
};

struct radv_bvh_aabb_node {
   float aabb[2][3];
   uint32_t primitive_id;
   /* flags in upper 4 bits */
   uint32_t geometry_id_and_flags;
   uint32_t reserved[8];
};

struct radv_bvh_instance_node {
   uint64_t base_ptr;
   /* lower 24 bits are the custom instance index, upper 8 bits are the visibility mask */
   uint32_t custom_instance_and_mask;
   /* lower 24 bits are the sbt offset, upper 8 bits are VkGeometryInstanceFlagsKHR */
   uint32_t sbt_offset_and_flags;

   /* The translation component is actually a pre-translation instead of a post-translation. If you
    * want to get a proper matrix out of it you need to apply the directional component of the
    * matrix to it. The pre-translation of the world->object matrix is the same as the
    * post-translation of the object->world matrix so this way we can share data between both
    * matrices. */
   float wto_matrix[12];
   float aabb[2][3];
   uint32_t instance_id;
   uint32_t reserved[9];
};

struct radv_bvh_box16_node {
   uint32_t children[4];
   uint32_t coords[4][3];
};

struct radv_bvh_box32_node {
   uint32_t children[4];
   float coords[4][2][3];
   uint32_t reserved[4];
};

void
radv_GetAccelerationStructureBuildSizesKHR(
   VkDevice _device, VkAccelerationStructureBuildTypeKHR buildType,
   const VkAccelerationStructureBuildGeometryInfoKHR *pBuildInfo,
   const uint32_t *pMaxPrimitiveCounts, VkAccelerationStructureBuildSizesInfoKHR *pSizeInfo)
{
   uint64_t triangles = 0, boxes = 0, instances = 0;

   for (uint32_t i = 0; i < pBuildInfo->geometryCount; ++i) {
      const VkAccelerationStructureGeometryKHR *geometry;
      if (pBuildInfo->pGeometries)
         geometry = &pBuildInfo->pGeometries[i];
      else
         geometry = pBuildInfo->ppGeometries[i];

      switch (geometry->geometryType) {
      case VK_GEOMETRY_TYPE_TRIANGLES_KHR:
         triangles += pMaxPrimitiveCounts[i];
         break;
      case VK_GEOMETRY_TYPE_AABBS_KHR:
         boxes += pMaxPrimitiveCounts[i];
         break;
      case VK_GEOMETRY_TYPE_INSTANCES_KHR:
         instances += pMaxPrimitiveCounts[i];
         break;
      case VK_GEOMETRY_TYPE_MAX_ENUM_KHR:
         unreachable("VK_GEOMETRY_TYPE_MAX_ENUM_KHR unhandled");
      }
   }

   uint64_t children = boxes + instances + triangles;
   uint64_t internal_nodes = 0;
   while (children > 1) {
      children = DIV_ROUND_UP(children, 4);
      internal_nodes += children;
   }

   /* The stray 128 is to ensure we have space for a header
    * which we'd want to use for some metadata (like the
    * total AABB of the BVH) */
   uint64_t size = boxes * 128 + instances * 128 + triangles * 64 + internal_nodes * 128 + 192;

   pSizeInfo->accelerationStructureSize = size;

   /* 2x the max number of nodes in a BVH layer (one uint32_t each) */
   pSizeInfo->updateScratchSize = pSizeInfo->buildScratchSize =
      MAX2(4096, 2 * (boxes + instances + triangles) * sizeof(uint32_t));
}

VkResult
radv_CreateAccelerationStructureKHR(VkDevice _device,
                                    const VkAccelerationStructureCreateInfoKHR *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    VkAccelerationStructureKHR *pAccelerationStructure)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   RADV_FROM_HANDLE(radv_buffer, buffer, pCreateInfo->buffer);
   struct radv_acceleration_structure *accel;

   accel = vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*accel), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (accel == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &accel->base, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR);

   accel->mem_offset = buffer->offset + pCreateInfo->offset;
   accel->size = pCreateInfo->size;
   accel->bo = buffer->bo;

   *pAccelerationStructure = radv_acceleration_structure_to_handle(accel);
   return VK_SUCCESS;
}

void
radv_DestroyAccelerationStructureKHR(VkDevice _device,
                                     VkAccelerationStructureKHR accelerationStructure,
                                     const VkAllocationCallbacks *pAllocator)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   RADV_FROM_HANDLE(radv_acceleration_structure, accel, accelerationStructure);

   if (!accel)
      return;

   vk_object_base_finish(&accel->base);
   vk_free2(&device->vk.alloc, pAllocator, accel);
}

VkDeviceAddress
radv_GetAccelerationStructureDeviceAddressKHR(
   VkDevice _device, const VkAccelerationStructureDeviceAddressInfoKHR *pInfo)
{
   RADV_FROM_HANDLE(radv_acceleration_structure, accel, pInfo->accelerationStructure);
   return radv_accel_struct_get_va(accel);
}

VkResult
radv_WriteAccelerationStructuresPropertiesKHR(
   VkDevice _device, uint32_t accelerationStructureCount,
   const VkAccelerationStructureKHR *pAccelerationStructures, VkQueryType queryType,
   size_t dataSize, void *pData, size_t stride)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   char *data_out = (char*)pData;

   for (uint32_t i = 0; i < accelerationStructureCount; ++i) {
      RADV_FROM_HANDLE(radv_acceleration_structure, accel, pAccelerationStructures[i]);
      const char *base_ptr = (const char *)device->ws->buffer_map(accel->bo);
      if (!base_ptr)
         return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

      const struct radv_accel_struct_header *header = (const void*)(base_ptr + accel->mem_offset);
      if (stride * i + sizeof(VkDeviceSize) <= dataSize) {
         uint64_t value;
         switch (queryType) {
         case VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR:
            value = header->compacted_size;
            break;
         case VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR:
            value = header->serialization_size;
            break;
         default:
            unreachable("Unhandled acceleration structure query");
         }
         *(VkDeviceSize *)(data_out + stride * i) = value;
      }
      device->ws->buffer_unmap(accel->bo);
   }
   return VK_SUCCESS;
}

struct radv_bvh_build_ctx {
   uint32_t *write_scratch;
   char *base;
   char *curr_ptr;
};

static void
build_triangles(struct radv_bvh_build_ctx *ctx, const VkAccelerationStructureGeometryKHR *geom,
                const VkAccelerationStructureBuildRangeInfoKHR *range, unsigned geometry_id)
{
   const VkAccelerationStructureGeometryTrianglesDataKHR *tri_data = &geom->geometry.triangles;
   VkTransformMatrixKHR matrix;
   const char *index_data = (const char *)tri_data->indexData.hostAddress + range->primitiveOffset;

   if (tri_data->transformData.hostAddress) {
      matrix = *(const VkTransformMatrixKHR *)((const char *)tri_data->transformData.hostAddress +
                                               range->transformOffset);
   } else {
      matrix = (VkTransformMatrixKHR){
         .matrix = {{1.0, 0.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0}, {0.0, 0.0, 1.0, 0.0}}};
   }

   for (uint32_t p = 0; p < range->primitiveCount; ++p, ctx->curr_ptr += 64) {
      struct radv_bvh_triangle_node *node = (void*)ctx->curr_ptr;
      uint32_t node_offset = ctx->curr_ptr - ctx->base;
      uint32_t node_id = node_offset >> 3;
      *ctx->write_scratch++ = node_id;

      for (unsigned v = 0; v < 3; ++v) {
         uint32_t v_index = range->firstVertex;
         switch (tri_data->indexType) {
         case VK_INDEX_TYPE_NONE_KHR:
            v_index += p * 3 + v;
            break;
         case VK_INDEX_TYPE_UINT8_EXT:
            v_index += *(const uint8_t *)index_data;
            index_data += 1;
            break;
         case VK_INDEX_TYPE_UINT16:
            v_index += *(const uint16_t *)index_data;
            index_data += 2;
            break;
         case VK_INDEX_TYPE_UINT32:
            v_index += *(const uint32_t *)index_data;
            index_data += 4;
            break;
         case VK_INDEX_TYPE_MAX_ENUM:
            unreachable("Unhandled VK_INDEX_TYPE_MAX_ENUM");
            break;
         }

         const char *v_data = (const char *)tri_data->vertexData.hostAddress + v_index * tri_data->vertexStride;
         float coords[4];
         switch (tri_data->vertexFormat) {
         case VK_FORMAT_R32G32B32_SFLOAT:
            coords[0] = *(const float *)(v_data + 0);
            coords[1] = *(const float *)(v_data + 4);
            coords[2] = *(const float *)(v_data + 8);
            coords[3] = 1.0f;
            break;
         case VK_FORMAT_R32G32B32A32_SFLOAT:
            coords[0] = *(const float *)(v_data + 0);
            coords[1] = *(const float *)(v_data + 4);
            coords[2] = *(const float *)(v_data + 8);
            coords[3] = *(const float *)(v_data + 12);
            break;
         case VK_FORMAT_R16G16B16_SFLOAT:
            coords[0] = _mesa_half_to_float(*(const uint16_t *)(v_data + 0));
            coords[1] = _mesa_half_to_float(*(const uint16_t *)(v_data + 2));
            coords[2] = _mesa_half_to_float(*(const uint16_t *)(v_data + 4));
            coords[3] = 1.0f;
            break;
         case VK_FORMAT_R16G16B16A16_SFLOAT:
            coords[0] = _mesa_half_to_float(*(const uint16_t *)(v_data + 0));
            coords[1] = _mesa_half_to_float(*(const uint16_t *)(v_data + 2));
            coords[2] = _mesa_half_to_float(*(const uint16_t *)(v_data + 4));
            coords[3] = _mesa_half_to_float(*(const uint16_t *)(v_data + 6));
            break;
         default:
            unreachable("Unhandled vertex format in BVH build");
         }

         for (unsigned j = 0; j < 3; ++j) {
            float r = 0;
            for (unsigned k = 0; k < 4; ++k)
               r += matrix.matrix[j][k] * coords[k];
            node->coords[v][j] = r;
         }

         node->triangle_id = p;
         node->geometry_id_and_flags = geometry_id | (geom->flags << 28);

         /* Seems to be needed for IJ, otherwise I = J = ? */
         node->id = 9;
      }
   }
}

static VkResult
build_instances(struct radv_device *device, struct radv_bvh_build_ctx *ctx,
                const VkAccelerationStructureGeometryKHR *geom,
                const VkAccelerationStructureBuildRangeInfoKHR *range)
{
   const VkAccelerationStructureGeometryInstancesDataKHR *inst_data = &geom->geometry.instances;

   for (uint32_t p = 0; p < range->primitiveCount; ++p, ctx->curr_ptr += 128) {
      const VkAccelerationStructureInstanceKHR *instance =
         inst_data->arrayOfPointers
            ? (((const VkAccelerationStructureInstanceKHR *const *)inst_data->data.hostAddress)[p])
            : &((const VkAccelerationStructureInstanceKHR *)inst_data->data.hostAddress)[p];
      if (!instance->accelerationStructureReference) {
         continue;
      }

      struct radv_bvh_instance_node *node = (void*)ctx->curr_ptr;
      uint32_t node_offset = ctx->curr_ptr - ctx->base;
      uint32_t node_id = (node_offset >> 3) | 6;
      *ctx->write_scratch++ = node_id;

      float transform[16], inv_transform[16];
      memcpy(transform, &instance->transform.matrix, sizeof(instance->transform.matrix));
      transform[12] = transform[13] = transform[14] = 0.0f;
      transform[15] = 1.0f;

      util_invert_mat4x4(inv_transform, transform);
      memcpy(node->wto_matrix, inv_transform, sizeof(node->wto_matrix));
      node->wto_matrix[3] = transform[3];
      node->wto_matrix[7] = transform[7];
      node->wto_matrix[11] = transform[11];
      node->custom_instance_and_mask = instance->instanceCustomIndex | (instance->mask << 24);
      node->sbt_offset_and_flags =
         instance->instanceShaderBindingTableRecordOffset | (instance->flags << 24);
      node->instance_id = p;

      RADV_FROM_HANDLE(radv_acceleration_structure, src_accel_struct,
                       (VkAccelerationStructureKHR)instance->accelerationStructureReference);
      const void *src_base = device->ws->buffer_map(src_accel_struct->bo);
      if (!src_base)
         return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

      src_base = (const char *)src_base + src_accel_struct->mem_offset;
      const struct radv_accel_struct_header *src_header = src_base;
      node->base_ptr = radv_accel_struct_get_va(src_accel_struct) | src_header->root_node_offset;

      for (unsigned j = 0; j < 3; ++j) {
         node->aabb[0][j] = instance->transform.matrix[j][3];
         node->aabb[1][j] = instance->transform.matrix[j][3];
         for (unsigned k = 0; k < 3; ++k) {
            node->aabb[0][j] += MIN2(instance->transform.matrix[j][k] * src_header->aabb[0][k],
                                     instance->transform.matrix[j][k] * src_header->aabb[1][k]);
            node->aabb[1][j] += MAX2(instance->transform.matrix[j][k] * src_header->aabb[0][k],
                                     instance->transform.matrix[j][k] * src_header->aabb[1][k]);
         }
      }
      device->ws->buffer_unmap(src_accel_struct->bo);
   }
   return VK_SUCCESS;
}

static void
build_aabbs(struct radv_bvh_build_ctx *ctx, const VkAccelerationStructureGeometryKHR *geom,
            const VkAccelerationStructureBuildRangeInfoKHR *range, unsigned geometry_id)
{
   const VkAccelerationStructureGeometryAabbsDataKHR *aabb_data = &geom->geometry.aabbs;

   for (uint32_t p = 0; p < range->primitiveCount; ++p, ctx->curr_ptr += 64) {
      struct radv_bvh_aabb_node *node = (void*)ctx->curr_ptr;
      uint32_t node_offset = ctx->curr_ptr - ctx->base;
      uint32_t node_id = (node_offset >> 3) | 6;
      *ctx->write_scratch++ = node_id;

      const VkAabbPositionsKHR *aabb =
         (const VkAabbPositionsKHR *)((const char *)aabb_data->data.hostAddress +
                                      p * aabb_data->stride);

      node->aabb[0][0] = aabb->minX;
      node->aabb[0][1] = aabb->minY;
      node->aabb[0][2] = aabb->minZ;
      node->aabb[1][0] = aabb->maxX;
      node->aabb[1][1] = aabb->maxY;
      node->aabb[1][2] = aabb->maxZ;
      node->primitive_id = p;
      node->geometry_id_and_flags = geometry_id;
   }
}

static uint32_t
leaf_node_count(const VkAccelerationStructureBuildGeometryInfoKHR *info,
                const VkAccelerationStructureBuildRangeInfoKHR *ranges)
{
   uint32_t count = 0;
   for (uint32_t i = 0; i < info->geometryCount; ++i) {
      count += ranges[i].primitiveCount;
   }
   return count;
}

static void
compute_bounds(const char *base_ptr, uint32_t node_id, float *bounds)
{
   for (unsigned i = 0; i < 3; ++i)
      bounds[i] = INFINITY;
   for (unsigned i = 0; i < 3; ++i)
      bounds[3 + i] = -INFINITY;

   switch (node_id & 7) {
   case 0: {
      const struct radv_bvh_triangle_node *node = (const void*)(base_ptr + (node_id / 8 * 64));
      for (unsigned v = 0; v < 3; ++v) {
         for (unsigned j = 0; j < 3; ++j) {
            bounds[j] = MIN2(bounds[j], node->coords[v][j]);
            bounds[3 + j] = MAX2(bounds[3 + j], node->coords[v][j]);
         }
      }
      break;
   }
   case 5: {
      const struct radv_bvh_box32_node *node = (const void*)(base_ptr + (node_id / 8 * 64));
      for (unsigned c2 = 0; c2 < 4; ++c2) {
         if (isnan(node->coords[c2][0][0]))
            continue;
         for (unsigned j = 0; j < 3; ++j) {
            bounds[j] = MIN2(bounds[j], node->coords[c2][0][j]);
            bounds[3 + j] = MAX2(bounds[3 + j], node->coords[c2][1][j]);
         }
      }
      break;
   }
   case 6: {
      const struct radv_bvh_instance_node *node = (const void*)(base_ptr + (node_id / 8 * 64));
      for (unsigned j = 0; j < 3; ++j) {
         bounds[j] = MIN2(bounds[j], node->aabb[0][j]);
         bounds[3 + j] = MAX2(bounds[3 + j], node->aabb[1][j]);
      }
      break;
   }
   case 7: {
      const struct radv_bvh_aabb_node *node = (const void*)(base_ptr + (node_id / 8 * 64));
      for (unsigned j = 0; j < 3; ++j) {
         bounds[j] = MIN2(bounds[j], node->aabb[0][j]);
         bounds[3 + j] = MAX2(bounds[3 + j], node->aabb[1][j]);
      }
      break;
   }
   }
}

static VkResult
build_bvh(struct radv_device *device, const VkAccelerationStructureBuildGeometryInfoKHR *info,
          const VkAccelerationStructureBuildRangeInfoKHR *ranges)
{
   RADV_FROM_HANDLE(radv_acceleration_structure, accel, info->dstAccelerationStructure);
   VkResult result = VK_SUCCESS;

   uint32_t *scratch[2];
   scratch[0] = info->scratchData.hostAddress;
   scratch[1] = scratch[0] + leaf_node_count(info, ranges);

   char *base_ptr = (char*)device->ws->buffer_map(accel->bo);
   if (!base_ptr)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   base_ptr = base_ptr + accel->mem_offset;
   struct radv_accel_struct_header *header = (void*)base_ptr;
   void *first_node_ptr = (char *)base_ptr + ALIGN(sizeof(*header), 64);

   struct radv_bvh_build_ctx ctx = {.write_scratch = scratch[0],
                                    .base = base_ptr,
                                    .curr_ptr = (char *)first_node_ptr + 128};

   /* This initializes the leaf nodes of the BVH all at the same level. */
   for (uint32_t i = 0; i < info->geometryCount; ++i) {
      const VkAccelerationStructureGeometryKHR *geom =
         info->pGeometries ? &info->pGeometries[i] : info->ppGeometries[i];

      switch (geom->geometryType) {
      case VK_GEOMETRY_TYPE_TRIANGLES_KHR:
         build_triangles(&ctx, geom, ranges + i, i);
         break;
      case VK_GEOMETRY_TYPE_AABBS_KHR:
         build_aabbs(&ctx, geom, ranges + i, i);
         break;
      case VK_GEOMETRY_TYPE_INSTANCES_KHR: {
         result = build_instances(device, &ctx, geom, ranges + i);
         if (result != VK_SUCCESS)
            goto fail;
         break;
      }
      case VK_GEOMETRY_TYPE_MAX_ENUM_KHR:
         unreachable("VK_GEOMETRY_TYPE_MAX_ENUM_KHR unhandled");
      }
   }

   uint32_t node_counts[2] = {ctx.write_scratch - scratch[0], 0};
   unsigned d;

   /*
    * This is the most naive BVH building algorithm I could think of:
    * just iteratively builds each level from bottom to top with
    * the children of each node being in-order and tightly packed.
    *
    * Is probably terrible for traversal but should be easy to build an
    * equivalent GPU version.
    */
   for (d = 0; node_counts[d & 1] > 1 || d == 0; ++d) {
      uint32_t child_count = node_counts[d & 1];
      const uint32_t *children = scratch[d & 1];
      uint32_t *dst_ids = scratch[(d & 1) ^ 1];
      unsigned dst_count;
      unsigned child_idx = 0;
      for (dst_count = 0; child_idx < MAX2(1, child_count); ++dst_count, child_idx += 4) {
         unsigned local_child_count = MIN2(4, child_count - child_idx);
         uint32_t child_ids[4];
         float bounds[4][6];

         for (unsigned c = 0; c < local_child_count; ++c) {
            uint32_t id = children[child_idx + c];
            child_ids[c] = id;

            compute_bounds(base_ptr, id, bounds[c]);
         }

         struct radv_bvh_box32_node *node;

         /* Put the root node at base_ptr so the id = 0, which allows some
          * traversal optimizations. */
         if (child_idx == 0 && local_child_count == child_count) {
            node = first_node_ptr;
            header->root_node_offset = ((char *)first_node_ptr - (char *)base_ptr) / 64 * 8 + 5;
         } else {
            uint32_t dst_id = (ctx.curr_ptr - base_ptr) / 64;
            dst_ids[dst_count] = dst_id * 8 + 5;

            node = (void*)ctx.curr_ptr;
            ctx.curr_ptr += 128;
         }

         for (unsigned c = 0; c < local_child_count; ++c) {
            node->children[c] = child_ids[c];
            for (unsigned i = 0; i < 2; ++i)
               for (unsigned j = 0; j < 3; ++j)
                  node->coords[c][i][j] = bounds[c][i * 3 + j];
         }
         for (unsigned c = local_child_count; c < 4; ++c) {
            for (unsigned i = 0; i < 2; ++i)
               for (unsigned j = 0; j < 3; ++j)
                  node->coords[c][i][j] = NAN;
         }
      }

      node_counts[(d & 1) ^ 1] = dst_count;
   }

   compute_bounds(base_ptr, header->root_node_offset, &header->aabb[0][0]);

   /* TODO init sizes and figure out what is needed for serialization. */

fail:
   device->ws->buffer_unmap(accel->bo);
   return result;
}

VkResult
radv_BuildAccelerationStructuresKHR(
   VkDevice _device, VkDeferredOperationKHR deferredOperation, uint32_t infoCount,
   const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
   const VkAccelerationStructureBuildRangeInfoKHR *const *ppBuildRangeInfos)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < infoCount; ++i) {
      result = build_bvh(device, pInfos + i, ppBuildRangeInfos[i]);
      if (result != VK_SUCCESS)
         break;
   }
   return result;
}
