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
#include "radv_acceleration_structure.h"
#include "radv_private.h"

#include "nir_builder.h"
#include "radv_cs.h"
#include "radv_meta.h"

#include "radix_sort/radv_radix_sort.h"

static const uint32_t morton_spv[] = {
#include "bvh/morton.comp.spv.h"
};

/* Min and max bounds of the bvh used to compute morton codes */
#define SCRATCH_TOTAL_BOUNDS_SIZE (6 * sizeof(float))

#define KEY_ID_PAIR_SIZE 8

VKAPI_ATTR void VKAPI_CALL
radv_GetAccelerationStructureBuildSizesKHR(
   VkDevice _device, VkAccelerationStructureBuildTypeKHR buildType,
   const VkAccelerationStructureBuildGeometryInfoKHR *pBuildInfo,
   const uint32_t *pMaxPrimitiveCounts, VkAccelerationStructureBuildSizesInfoKHR *pSizeInfo)
{
   RADV_FROM_HANDLE(radv_device, device, _device);

   uint64_t triangles = 0, boxes = 0, instances = 0;

   STATIC_ASSERT(sizeof(struct radv_bvh_triangle_node) == 64);
   STATIC_ASSERT(sizeof(struct radv_bvh_aabb_node) == 64);
   STATIC_ASSERT(sizeof(struct radv_bvh_instance_node) == 128);
   STATIC_ASSERT(sizeof(struct radv_bvh_box16_node) == 64);
   STATIC_ASSERT(sizeof(struct radv_bvh_box32_node) == 128);

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
   /* Initialize to 1 to have enought space for the root node. */
   uint64_t internal_nodes = 1;
   while (children > 1) {
      children = DIV_ROUND_UP(children, 4);
      internal_nodes += children;
   }

   uint64_t size = boxes * 128 + instances * 128 + triangles * 64 + internal_nodes * 128 +
                   ALIGN(sizeof(struct radv_accel_struct_header), 64);

   pSizeInfo->accelerationStructureSize = size;

   /* 2x the max number of nodes in a BVH layer and order information for sorting. */
   uint32_t leaf_count = boxes + instances + triangles;
   VkDeviceSize scratchSize = 2 * leaf_count * KEY_ID_PAIR_SIZE;

   radix_sort_vk_memory_requirements_t requirements;
   radix_sort_vk_get_memory_requirements(device->meta_state.accel_struct_build.radix_sort,
                                         leaf_count, &requirements);

   /* Make sure we have the space required by the radix sort. */
   scratchSize = MAX2(scratchSize, requirements.keyvals_size * 2);

   scratchSize += requirements.internal_size + SCRATCH_TOTAL_BOUNDS_SIZE;

   scratchSize = MAX2(4096, scratchSize);
   pSizeInfo->updateScratchSize = scratchSize;
   pSizeInfo->buildScratchSize = scratchSize;
}

VKAPI_ATTR VkResult VKAPI_CALL
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
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &accel->base, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR);

   accel->mem_offset = buffer->offset + pCreateInfo->offset;
   accel->size = pCreateInfo->size;
   accel->bo = buffer->bo;

   *pAccelerationStructure = radv_acceleration_structure_to_handle(accel);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
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

VKAPI_ATTR VkDeviceAddress VKAPI_CALL
radv_GetAccelerationStructureDeviceAddressKHR(
   VkDevice _device, const VkAccelerationStructureDeviceAddressInfoKHR *pInfo)
{
   RADV_FROM_HANDLE(radv_acceleration_structure, accel, pInfo->accelerationStructure);
   return radv_accel_struct_get_va(accel);
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_WriteAccelerationStructuresPropertiesKHR(
   VkDevice _device, uint32_t accelerationStructureCount,
   const VkAccelerationStructureKHR *pAccelerationStructures, VkQueryType queryType,
   size_t dataSize, void *pData, size_t stride)
{
   unreachable("Unimplemented");
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_BuildAccelerationStructuresKHR(
   VkDevice _device, VkDeferredOperationKHR deferredOperation, uint32_t infoCount,
   const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
   const VkAccelerationStructureBuildRangeInfoKHR *const *ppBuildRangeInfos)
{
   unreachable("Unimplemented");
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CopyAccelerationStructureKHR(VkDevice _device, VkDeferredOperationKHR deferredOperation,
                                  const VkCopyAccelerationStructureInfoKHR *pInfo)
{
   unreachable("Unimplemented");
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

static nir_builder
create_accel_build_shader(struct radv_device *device, const char *name)
{
   nir_builder b = radv_meta_init_shader(device, MESA_SHADER_COMPUTE, "%s", name);
   b.shader->info.workgroup_size[0] = 64;

   assert(b.shader->info.workgroup_size[1] == 1);
   assert(b.shader->info.workgroup_size[2] == 1);
   assert(!b.shader->info.workgroup_size_variable);

   return b;
}

static nir_ssa_def *
get_indices(nir_builder *b, nir_ssa_def *addr, nir_ssa_def *type, nir_ssa_def *id)
{
   const struct glsl_type *uvec3_type = glsl_vector_type(GLSL_TYPE_UINT, 3);
   nir_variable *result =
      nir_variable_create(b->shader, nir_var_shader_temp, uvec3_type, "indices");

   nir_push_if(b, nir_ult(b, type, nir_imm_int(b, 2)));
   nir_push_if(b, nir_ieq_imm(b, type, VK_INDEX_TYPE_UINT16));
   {
      nir_ssa_def *index_id = nir_umul24(b, id, nir_imm_int(b, 6));
      nir_ssa_def *indices[3];
      for (unsigned i = 0; i < 3; ++i) {
         indices[i] = nir_build_load_global(
            b, 1, 16, nir_iadd(b, addr, nir_u2u64(b, nir_iadd_imm(b, index_id, 2 * i))));
      }
      nir_store_var(b, result, nir_u2u32(b, nir_vec(b, indices, 3)), 7);
   }
   nir_push_else(b, NULL);
   {
      nir_ssa_def *index_id = nir_umul24(b, id, nir_imm_int(b, 12));
      nir_ssa_def *indices =
         nir_build_load_global(b, 3, 32, nir_iadd(b, addr, nir_u2u64(b, index_id)));
      nir_store_var(b, result, indices, 7);
   }
   nir_pop_if(b, NULL);
   nir_push_else(b, NULL);
   {
      nir_ssa_def *index_id = nir_umul24(b, id, nir_imm_int(b, 3));
      nir_ssa_def *indices[] = {
         index_id,
         nir_iadd_imm(b, index_id, 1),
         nir_iadd_imm(b, index_id, 2),
      };

      nir_push_if(b, nir_ieq_imm(b, type, VK_INDEX_TYPE_NONE_KHR));
      {
         nir_store_var(b, result, nir_vec(b, indices, 3), 7);
      }
      nir_push_else(b, NULL);
      {
         for (unsigned i = 0; i < 3; ++i) {
            indices[i] =
               nir_build_load_global(b, 1, 8, nir_iadd(b, addr, nir_u2u64(b, indices[i])));
         }
         nir_store_var(b, result, nir_u2u32(b, nir_vec(b, indices, 3)), 7);
      }
      nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);
   return nir_load_var(b, result);
}

static void
get_vertices(nir_builder *b, nir_ssa_def *addresses, nir_ssa_def *format, nir_ssa_def *positions[3])
{
   const struct glsl_type *vec3_type = glsl_vector_type(GLSL_TYPE_FLOAT, 3);
   nir_variable *results[3] = {
      nir_variable_create(b->shader, nir_var_shader_temp, vec3_type, "vertex0"),
      nir_variable_create(b->shader, nir_var_shader_temp, vec3_type, "vertex1"),
      nir_variable_create(b->shader, nir_var_shader_temp, vec3_type, "vertex2")};

   VkFormat formats[] = {
      VK_FORMAT_R32G32B32_SFLOAT,
      VK_FORMAT_R32G32B32A32_SFLOAT,
      VK_FORMAT_R16G16B16_SFLOAT,
      VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_FORMAT_R16G16_SFLOAT,
      VK_FORMAT_R32G32_SFLOAT,
      VK_FORMAT_R16G16_SNORM,
      VK_FORMAT_R16G16_UNORM,
      VK_FORMAT_R16G16B16A16_SNORM,
      VK_FORMAT_R16G16B16A16_UNORM,
      VK_FORMAT_R8G8_SNORM,
      VK_FORMAT_R8G8_UNORM,
      VK_FORMAT_R8G8B8A8_SNORM,
      VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_A2B10G10R10_UNORM_PACK32,
   };

   for (unsigned f = 0; f < ARRAY_SIZE(formats); ++f) {
      if (f + 1 < ARRAY_SIZE(formats))
         nir_push_if(b, nir_ieq_imm(b, format, formats[f]));

      for (unsigned i = 0; i < 3; ++i) {
         switch (formats[f]) {
         case VK_FORMAT_R32G32B32_SFLOAT:
         case VK_FORMAT_R32G32B32A32_SFLOAT:
            nir_store_var(b, results[i],
                          nir_build_load_global(b, 3, 32, nir_channel(b, addresses, i)), 7);
            break;
         case VK_FORMAT_R32G32_SFLOAT:
         case VK_FORMAT_R16G16_SFLOAT:
         case VK_FORMAT_R16G16B16_SFLOAT:
         case VK_FORMAT_R16G16B16A16_SFLOAT:
         case VK_FORMAT_R16G16_SNORM:
         case VK_FORMAT_R16G16_UNORM:
         case VK_FORMAT_R16G16B16A16_SNORM:
         case VK_FORMAT_R16G16B16A16_UNORM:
         case VK_FORMAT_R8G8_SNORM:
         case VK_FORMAT_R8G8_UNORM:
         case VK_FORMAT_R8G8B8A8_SNORM:
         case VK_FORMAT_R8G8B8A8_UNORM:
         case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
            unsigned components = MIN2(3, vk_format_get_nr_components(formats[f]));
            unsigned comp_bits =
               vk_format_get_blocksizebits(formats[f]) / vk_format_get_nr_components(formats[f]);
            unsigned comp_bytes = comp_bits / 8;
            nir_ssa_def *values[3];
            nir_ssa_def *addr = nir_channel(b, addresses, i);

            if (formats[f] == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
               comp_bits = 10;
               nir_ssa_def *val = nir_build_load_global(b, 1, 32, addr);
               for (unsigned j = 0; j < 3; ++j)
                  values[j] = nir_ubfe(b, val, nir_imm_int(b, j * 10), nir_imm_int(b, 10));
            } else {
               for (unsigned j = 0; j < components; ++j)
                  values[j] =
                     nir_build_load_global(b, 1, comp_bits, nir_iadd_imm(b, addr, j * comp_bytes));

               for (unsigned j = components; j < 3; ++j)
                  values[j] = nir_imm_intN_t(b, 0, comp_bits);
            }

            nir_ssa_def *vec;
            if (util_format_is_snorm(vk_format_to_pipe_format(formats[f]))) {
               for (unsigned j = 0; j < 3; ++j) {
                  values[j] = nir_fdiv(b, nir_i2f32(b, values[j]),
                                       nir_imm_float(b, (1u << (comp_bits - 1)) - 1));
                  values[j] = nir_fmax(b, values[j], nir_imm_float(b, -1.0));
               }
               vec = nir_vec(b, values, 3);
            } else if (util_format_is_unorm(vk_format_to_pipe_format(formats[f]))) {
               for (unsigned j = 0; j < 3; ++j) {
                  values[j] =
                     nir_fdiv(b, nir_u2f32(b, values[j]), nir_imm_float(b, (1u << comp_bits) - 1));
                  values[j] = nir_fmin(b, values[j], nir_imm_float(b, 1.0));
               }
               vec = nir_vec(b, values, 3);
            } else if (comp_bits == 16)
               vec = nir_f2f32(b, nir_vec(b, values, 3));
            else
               vec = nir_vec(b, values, 3);
            nir_store_var(b, results[i], vec, 7);
            break;
         }
         default:
            unreachable("Unhandled format");
         }
      }
      if (f + 1 < ARRAY_SIZE(formats))
         nir_push_else(b, NULL);
   }
   for (unsigned f = 1; f < ARRAY_SIZE(formats); ++f) {
      nir_pop_if(b, NULL);
   }

   for (unsigned i = 0; i < 3; ++i)
      positions[i] = nir_load_var(b, results[i]);
}

struct build_primitive_constants {
   uint64_t node_dst_addr;
   uint64_t scratch_addr;
   uint32_t dst_offset;
   uint32_t dst_scratch_offset;
   uint32_t geometry_type;
   uint32_t geometry_id;

   union {
      struct {
         uint64_t vertex_addr;
         uint64_t index_addr;
         uint64_t transform_addr;
         uint32_t vertex_stride;
         uint32_t vertex_format;
         uint32_t index_format;
      };
      struct {
         uint64_t instance_data;
         uint32_t array_of_pointers;
      };
      struct {
         uint64_t aabb_addr;
         uint32_t aabb_stride;
      };
   };
};

struct morton_constants {
   uint64_t bvh_addr;
   uint64_t bounds_addr;
   uint64_t ids_addr;
};

struct build_internal_constants {
   uint64_t node_dst_addr;
   uint64_t scratch_addr;
   uint32_t dst_offset;
   uint32_t dst_scratch_offset;
   uint32_t src_scratch_offset;
   uint32_t fill_header;
};

/* This inverts a 3x3 matrix using cofactors, as in e.g.
 * https://www.mathsisfun.com/algebra/matrix-inverse-minors-cofactors-adjugate.html */
static void
nir_invert_3x3(nir_builder *b, nir_ssa_def *in[3][3], nir_ssa_def *out[3][3])
{
   nir_ssa_def *cofactors[3][3];
   for (unsigned i = 0; i < 3; ++i) {
      for (unsigned j = 0; j < 3; ++j) {
         cofactors[i][j] =
            nir_fsub(b, nir_fmul(b, in[(i + 1) % 3][(j + 1) % 3], in[(i + 2) % 3][(j + 2) % 3]),
                     nir_fmul(b, in[(i + 1) % 3][(j + 2) % 3], in[(i + 2) % 3][(j + 1) % 3]));
      }
   }

   nir_ssa_def *det = NULL;
   for (unsigned i = 0; i < 3; ++i) {
      nir_ssa_def *det_part = nir_fmul(b, in[0][i], cofactors[0][i]);
      det = det ? nir_fadd(b, det, det_part) : det_part;
   }

   nir_ssa_def *det_inv = nir_frcp(b, det);
   for (unsigned i = 0; i < 3; ++i) {
      for (unsigned j = 0; j < 3; ++j) {
         out[i][j] = nir_fmul(b, cofactors[j][i], det_inv);
      }
   }
}

static void
atomic_fminmax(struct radv_device *dev, nir_builder *b, nir_ssa_def *addr, bool is_max,
               nir_ssa_def *val)
{
   /* Use an integer comparison to work correctly with negative zero. */
   val = nir_bcsel(b, nir_ilt(b, val, nir_imm_int(b, 0)),
                   nir_isub(b, nir_imm_int(b, -2147483648), val), val);

   if (is_max)
      nir_global_atomic_imax(b, 32, addr, val);
   else
      nir_global_atomic_imin(b, 32, addr, val);
}

static nir_shader *
build_leaf_shader(struct radv_device *dev)
{
   const struct glsl_type *vec3_type = glsl_vector_type(GLSL_TYPE_FLOAT, 3);
   nir_builder b = create_accel_build_shader(dev, "accel_build_leaf_shader");

   nir_ssa_def *pconst0 =
      nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .base = 0, .range = 16);
   nir_ssa_def *pconst1 =
      nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .base = 16, .range = 16);
   nir_ssa_def *pconst2 =
      nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .base = 32, .range = 16);
   nir_ssa_def *pconst3 =
      nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .base = 48, .range = 16);
   nir_ssa_def *index_format =
      nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0), .base = 64, .range = 4);

   nir_ssa_def *node_dst_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst0, 0b0011));
   nir_ssa_def *scratch_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst0, 0b1100));
   nir_ssa_def *node_dst_offset = nir_channel(&b, pconst1, 0);
   nir_ssa_def *scratch_offset = nir_channel(&b, pconst1, 1);
   nir_ssa_def *geom_type = nir_channel(&b, pconst1, 2);
   nir_ssa_def *geometry_id = nir_channel(&b, pconst1, 3);

   nir_ssa_def *global_id =
      nir_iadd(&b,
               nir_imul_imm(&b, nir_channels(&b, nir_load_workgroup_id(&b, 32), 1),
                            b.shader->info.workgroup_size[0]),
               nir_channels(&b, nir_load_local_invocation_id(&b), 1));

   nir_ssa_def *scratch_dst_addr = nir_iadd(
      &b, scratch_addr,
      nir_u2u64(&b, nir_iadd(&b, scratch_offset, nir_imul_imm(&b, global_id, KEY_ID_PAIR_SIZE))));
   scratch_dst_addr = nir_iadd_imm(&b, scratch_dst_addr, SCRATCH_TOTAL_BOUNDS_SIZE);

   nir_variable *bounds[2] = {
      nir_variable_create(b.shader, nir_var_shader_temp, vec3_type, "min_bound"),
      nir_variable_create(b.shader, nir_var_shader_temp, vec3_type, "max_bound"),
   };

   nir_push_if(&b, nir_ieq_imm(&b, geom_type, VK_GEOMETRY_TYPE_TRIANGLES_KHR));
   { /* Triangles */
      nir_ssa_def *vertex_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst2, 0b0011));
      nir_ssa_def *index_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst2, 0b1100));
      nir_ssa_def *transform_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst3, 3));
      nir_ssa_def *vertex_stride = nir_channel(&b, pconst3, 2);
      nir_ssa_def *vertex_format = nir_channel(&b, pconst3, 3);
      unsigned repl_swizzle[4] = {0, 0, 0, 0};

      nir_ssa_def *node_offset = nir_iadd(&b, node_dst_offset, nir_imul_imm(&b, global_id, 64));
      nir_ssa_def *triangle_node_dst_addr = nir_iadd(&b, node_dst_addr, nir_u2u64(&b, node_offset));

      nir_ssa_def *indices = get_indices(&b, index_addr, index_format, global_id);
      nir_ssa_def *vertex_addresses = nir_iadd(
         &b, nir_u2u64(&b, nir_imul(&b, indices, nir_swizzle(&b, vertex_stride, repl_swizzle, 3))),
         nir_swizzle(&b, vertex_addr, repl_swizzle, 3));
      nir_ssa_def *positions[3];
      get_vertices(&b, vertex_addresses, vertex_format, positions);

      nir_ssa_def *node_data[16];
      memset(node_data, 0, sizeof(node_data));

      nir_variable *transform[] = {
         nir_variable_create(b.shader, nir_var_shader_temp, glsl_vec4_type(), "transform0"),
         nir_variable_create(b.shader, nir_var_shader_temp, glsl_vec4_type(), "transform1"),
         nir_variable_create(b.shader, nir_var_shader_temp, glsl_vec4_type(), "transform2"),
      };
      nir_store_var(&b, transform[0], nir_imm_vec4(&b, 1.0, 0.0, 0.0, 0.0), 0xf);
      nir_store_var(&b, transform[1], nir_imm_vec4(&b, 0.0, 1.0, 0.0, 0.0), 0xf);
      nir_store_var(&b, transform[2], nir_imm_vec4(&b, 0.0, 0.0, 1.0, 0.0), 0xf);

      nir_push_if(&b, nir_ine_imm(&b, transform_addr, 0));
      nir_store_var(&b, transform[0],
                    nir_build_load_global(&b, 4, 32, nir_iadd_imm(&b, transform_addr, 0),
                                          .access = ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER),
                    0xf);
      nir_store_var(&b, transform[1],
                    nir_build_load_global(&b, 4, 32, nir_iadd_imm(&b, transform_addr, 16),
                                          .access = ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER),
                    0xf);
      nir_store_var(&b, transform[2],
                    nir_build_load_global(&b, 4, 32, nir_iadd_imm(&b, transform_addr, 32),
                                          .access = ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER),
                    0xf);
      nir_pop_if(&b, NULL);

      for (unsigned i = 0; i < 3; ++i)
         for (unsigned j = 0; j < 3; ++j)
            node_data[i * 3 + j] = nir_fdph(&b, positions[i], nir_load_var(&b, transform[j]));

      nir_ssa_def *min_bound = NULL;
      nir_ssa_def *max_bound = NULL;
      for (unsigned i = 0; i < 3; ++i) {
         nir_ssa_def *position = nir_vec(&b, node_data + i * 3, 3);
         if (min_bound) {
            min_bound = nir_fmin(&b, min_bound, position);
            max_bound = nir_fmax(&b, max_bound, position);
         } else {
            min_bound = position;
            max_bound = position;
         }
      }

      nir_store_var(&b, bounds[0], min_bound, 7);
      nir_store_var(&b, bounds[1], max_bound, 7);

      node_data[12] = global_id;
      node_data[13] = geometry_id;
      node_data[15] = nir_imm_int(&b, 9);
      for (unsigned i = 0; i < ARRAY_SIZE(node_data); ++i)
         if (!node_data[i])
            node_data[i] = nir_imm_int(&b, 0);

      for (unsigned i = 0; i < 4; ++i) {
         nir_build_store_global(&b, nir_vec(&b, node_data + i * 4, 4),
                                nir_iadd_imm(&b, triangle_node_dst_addr, i * 16), .align_mul = 16);
      }

      nir_ssa_def *node_id =
         nir_iadd_imm(&b, nir_ushr_imm(&b, node_offset, 3), radv_bvh_node_triangle);
      nir_build_store_global(&b, node_id, scratch_dst_addr);
   }
   nir_push_else(&b, NULL);
   nir_push_if(&b, nir_ieq_imm(&b, geom_type, VK_GEOMETRY_TYPE_AABBS_KHR));
   { /* AABBs */
      nir_ssa_def *aabb_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst2, 0b0011));
      nir_ssa_def *aabb_stride = nir_channel(&b, pconst2, 2);

      nir_ssa_def *node_offset = nir_iadd(&b, node_dst_offset, nir_imul_imm(&b, global_id, 64));
      nir_ssa_def *aabb_node_dst_addr = nir_iadd(&b, node_dst_addr, nir_u2u64(&b, node_offset));

      nir_ssa_def *node_id = nir_iadd_imm(&b, nir_ushr_imm(&b, node_offset, 3), radv_bvh_node_aabb);
      nir_build_store_global(&b, node_id, scratch_dst_addr);

      aabb_addr = nir_iadd(&b, aabb_addr, nir_u2u64(&b, nir_imul(&b, aabb_stride, global_id)));

      nir_ssa_def *min_bound =
         nir_build_load_global(&b, 3, 32, nir_iadd_imm(&b, aabb_addr, 0),
                               .access = ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER);
      nir_ssa_def *max_bound =
         nir_build_load_global(&b, 3, 32, nir_iadd_imm(&b, aabb_addr, 12),
                               .access = ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER);

      nir_store_var(&b, bounds[0], min_bound, 7);
      nir_store_var(&b, bounds[1], max_bound, 7);

      nir_ssa_def *values[] = {nir_channel(&b, min_bound, 0),
                               nir_channel(&b, min_bound, 1),
                               nir_channel(&b, min_bound, 2),
                               nir_channel(&b, max_bound, 0),
                               nir_channel(&b, max_bound, 1),
                               nir_channel(&b, max_bound, 2),
                               global_id,
                               geometry_id};

      nir_build_store_global(&b, nir_vec(&b, values + 0, 4),
                             nir_iadd_imm(&b, aabb_node_dst_addr, 0), .align_mul = 16);
      nir_build_store_global(&b, nir_vec(&b, values + 4, 4),
                             nir_iadd_imm(&b, aabb_node_dst_addr, 16), .align_mul = 16);
   }
   nir_push_else(&b, NULL);
   { /* Instances */

      nir_variable *instance_addr_var =
         nir_variable_create(b.shader, nir_var_shader_temp, glsl_uint64_t_type(), "instance_addr");
      nir_push_if(&b, nir_ine_imm(&b, nir_channel(&b, pconst2, 2), 0));
      {
         nir_ssa_def *ptr = nir_iadd(&b, nir_pack_64_2x32(&b, nir_channels(&b, pconst2, 0b0011)),
                                     nir_u2u64(&b, nir_imul_imm(&b, global_id, 8)));
         nir_ssa_def *addr =
            nir_pack_64_2x32(&b, nir_build_load_global(&b, 2, 32, ptr, .align_mul = 8));
         nir_store_var(&b, instance_addr_var, addr, 1);
      }
      nir_push_else(&b, NULL);
      {
         nir_ssa_def *addr = nir_iadd(&b, nir_pack_64_2x32(&b, nir_channels(&b, pconst2, 0b0011)),
                                      nir_u2u64(&b, nir_imul_imm(&b, global_id, 64)));
         nir_store_var(&b, instance_addr_var, addr, 1);
      }
      nir_pop_if(&b, NULL);
      nir_ssa_def *instance_addr = nir_load_var(&b, instance_addr_var);

      nir_ssa_def *inst_transform[] = {
         nir_build_load_global(&b, 4, 32, nir_iadd_imm(&b, instance_addr, 0)),
         nir_build_load_global(&b, 4, 32, nir_iadd_imm(&b, instance_addr, 16)),
         nir_build_load_global(&b, 4, 32, nir_iadd_imm(&b, instance_addr, 32))};
      nir_ssa_def *inst3 = nir_build_load_global(&b, 4, 32, nir_iadd_imm(&b, instance_addr, 48));

      nir_ssa_def *node_offset = nir_iadd(&b, node_dst_offset, nir_imul_imm(&b, global_id, 128));
      node_dst_addr = nir_iadd(&b, node_dst_addr, nir_u2u64(&b, node_offset));

      nir_ssa_def *node_id =
         nir_iadd_imm(&b, nir_ushr_imm(&b, node_offset, 3), radv_bvh_node_instance);
      nir_build_store_global(&b, node_id, scratch_dst_addr);

      nir_ssa_def *header_addr = nir_pack_64_2x32(&b, nir_channels(&b, inst3, 12));
      nir_push_if(&b, nir_ine_imm(&b, header_addr, 0));
      nir_ssa_def *header_root_offset =
         nir_build_load_global(&b, 1, 32, nir_iadd_imm(&b, header_addr, 0));
      nir_ssa_def *header_min = nir_build_load_global(&b, 3, 32, nir_iadd_imm(&b, header_addr, 8));
      nir_ssa_def *header_max = nir_build_load_global(&b, 3, 32, nir_iadd_imm(&b, header_addr, 20));

      nir_ssa_def *bound_defs[2][3];
      for (unsigned i = 0; i < 3; ++i) {
         bound_defs[0][i] = bound_defs[1][i] = nir_channel(&b, inst_transform[i], 3);

         nir_ssa_def *mul_a = nir_fmul(&b, nir_channels(&b, inst_transform[i], 7), header_min);
         nir_ssa_def *mul_b = nir_fmul(&b, nir_channels(&b, inst_transform[i], 7), header_max);
         nir_ssa_def *mi = nir_fmin(&b, mul_a, mul_b);
         nir_ssa_def *ma = nir_fmax(&b, mul_a, mul_b);
         for (unsigned j = 0; j < 3; ++j) {
            bound_defs[0][i] = nir_fadd(&b, bound_defs[0][i], nir_channel(&b, mi, j));
            bound_defs[1][i] = nir_fadd(&b, bound_defs[1][i], nir_channel(&b, ma, j));
         }
      }

      nir_store_var(&b, bounds[0], nir_vec(&b, bound_defs[0], 3), 7);
      nir_store_var(&b, bounds[1], nir_vec(&b, bound_defs[1], 3), 7);

      /* Store object to world matrix */
      for (unsigned i = 0; i < 3; ++i) {
         nir_ssa_def *vals[3];
         for (unsigned j = 0; j < 3; ++j)
            vals[j] = nir_channel(&b, inst_transform[j], i);

         nir_build_store_global(&b, nir_vec(&b, vals, 3),
                                nir_iadd_imm(&b, node_dst_addr, 92 + 12 * i));
      }

      nir_ssa_def *m_in[3][3], *m_out[3][3], *m_vec[3][4];
      for (unsigned i = 0; i < 3; ++i)
         for (unsigned j = 0; j < 3; ++j)
            m_in[i][j] = nir_channel(&b, inst_transform[i], j);
      nir_invert_3x3(&b, m_in, m_out);
      for (unsigned i = 0; i < 3; ++i) {
         for (unsigned j = 0; j < 3; ++j)
            m_vec[i][j] = m_out[i][j];
         m_vec[i][3] = nir_channel(&b, inst_transform[i], 3);
      }

      for (unsigned i = 0; i < 3; ++i) {
         nir_build_store_global(&b, nir_vec(&b, m_vec[i], 4),
                                nir_iadd_imm(&b, node_dst_addr, 16 + 16 * i));
      }

      nir_ssa_def *out0[4] = {
         nir_ior(&b, nir_channel(&b, nir_unpack_64_2x32(&b, header_addr), 0), header_root_offset),
         nir_channel(&b, nir_unpack_64_2x32(&b, header_addr), 1), nir_channel(&b, inst3, 0),
         nir_channel(&b, inst3, 1)};
      nir_build_store_global(&b, nir_vec(&b, out0, 4), nir_iadd_imm(&b, node_dst_addr, 0));
      nir_build_store_global(&b, global_id, nir_iadd_imm(&b, node_dst_addr, 88));
      nir_pop_if(&b, NULL);
      nir_build_store_global(&b, nir_load_var(&b, bounds[0]), nir_iadd_imm(&b, node_dst_addr, 64));
      nir_build_store_global(&b, nir_load_var(&b, bounds[1]), nir_iadd_imm(&b, node_dst_addr, 76));
   }
   nir_pop_if(&b, NULL);
   nir_pop_if(&b, NULL);

   nir_ssa_def *min = nir_load_var(&b, bounds[0]);
   nir_ssa_def *max = nir_load_var(&b, bounds[1]);

   nir_ssa_def *min_reduced = nir_reduce(&b, min, .reduction_op = nir_op_fmin);
   nir_ssa_def *max_reduced = nir_reduce(&b, max, .reduction_op = nir_op_fmax);

   nir_push_if(&b, nir_elect(&b, 1));

   atomic_fminmax(dev, &b, scratch_addr, false, nir_channel(&b, min_reduced, 0));
   atomic_fminmax(dev, &b, nir_iadd_imm(&b, scratch_addr, 4), false,
                  nir_channel(&b, min_reduced, 1));
   atomic_fminmax(dev, &b, nir_iadd_imm(&b, scratch_addr, 8), false,
                  nir_channel(&b, min_reduced, 2));

   atomic_fminmax(dev, &b, nir_iadd_imm(&b, scratch_addr, 12), true,
                  nir_channel(&b, max_reduced, 0));
   atomic_fminmax(dev, &b, nir_iadd_imm(&b, scratch_addr, 16), true,
                  nir_channel(&b, max_reduced, 1));
   atomic_fminmax(dev, &b, nir_iadd_imm(&b, scratch_addr, 20), true,
                  nir_channel(&b, max_reduced, 2));

   return b.shader;
}

static void
determine_bounds(nir_builder *b, nir_ssa_def *node_addr, nir_ssa_def *node_id,
                 nir_variable *bounds_vars[2])
{
   nir_ssa_def *node_type = nir_iand_imm(b, node_id, 7);
   node_addr =
      nir_iadd(b, node_addr, nir_u2u64(b, nir_ishl_imm(b, nir_iand_imm(b, node_id, ~7u), 3)));

   nir_push_if(b, nir_ieq_imm(b, node_type, radv_bvh_node_triangle));
   {
      nir_ssa_def *positions[3];
      for (unsigned i = 0; i < 3; ++i)
         positions[i] = nir_build_load_global(b, 3, 32, nir_iadd_imm(b, node_addr, i * 12));
      nir_ssa_def *bounds[] = {positions[0], positions[0]};
      for (unsigned i = 1; i < 3; ++i) {
         bounds[0] = nir_fmin(b, bounds[0], positions[i]);
         bounds[1] = nir_fmax(b, bounds[1], positions[i]);
      }
      nir_store_var(b, bounds_vars[0], bounds[0], 7);
      nir_store_var(b, bounds_vars[1], bounds[1], 7);
   }
   nir_push_else(b, NULL);
   nir_push_if(b, nir_ieq_imm(b, node_type, radv_bvh_node_internal));
   {
      nir_ssa_def *input_bounds[4][2];
      for (unsigned i = 0; i < 4; ++i)
         for (unsigned j = 0; j < 2; ++j)
            input_bounds[i][j] =
               nir_build_load_global(b, 3, 32, nir_iadd_imm(b, node_addr, 16 + i * 24 + j * 12));
      nir_ssa_def *bounds[] = {input_bounds[0][0], input_bounds[0][1]};
      for (unsigned i = 1; i < 4; ++i) {
         bounds[0] = nir_fmin(b, bounds[0], input_bounds[i][0]);
         bounds[1] = nir_fmax(b, bounds[1], input_bounds[i][1]);
      }

      nir_store_var(b, bounds_vars[0], bounds[0], 7);
      nir_store_var(b, bounds_vars[1], bounds[1], 7);
   }
   nir_push_else(b, NULL);
   nir_push_if(b, nir_ieq_imm(b, node_type, radv_bvh_node_instance));
   { /* Instances */
      nir_ssa_def *bounds[2];
      for (unsigned i = 0; i < 2; ++i)
         bounds[i] = nir_build_load_global(b, 3, 32, nir_iadd_imm(b, node_addr, 64 + i * 12));
      nir_store_var(b, bounds_vars[0], bounds[0], 7);
      nir_store_var(b, bounds_vars[1], bounds[1], 7);
   }
   nir_push_else(b, NULL);
   { /* AABBs */
      nir_ssa_def *bounds[2];
      for (unsigned i = 0; i < 2; ++i)
         bounds[i] = nir_build_load_global(b, 3, 32, nir_iadd_imm(b, node_addr, i * 12));
      nir_store_var(b, bounds_vars[0], bounds[0], 7);
      nir_store_var(b, bounds_vars[1], bounds[1], 7);
   }
   nir_pop_if(b, NULL);
   nir_pop_if(b, NULL);
   nir_pop_if(b, NULL);
}

static nir_shader *
build_internal_shader(struct radv_device *dev)
{
   const struct glsl_type *vec3_type = glsl_vector_type(GLSL_TYPE_FLOAT, 3);
   nir_builder b = create_accel_build_shader(dev, "accel_build_internal_shader");

   /*
    * push constants:
    *   i32 x 2: node dst address
    *   i32 x 2: scratch address
    *   i32: dst offset
    *   i32: dst scratch offset
    *   i32: src scratch offset
    *   i32: src_node_count | (fill_header << 31)
    */
   nir_ssa_def *pconst0 =
      nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .base = 0, .range = 16);
   nir_ssa_def *pconst1 =
      nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .base = 16, .range = 16);

   nir_ssa_def *node_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst0, 0b0011));
   nir_ssa_def *scratch_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst0, 0b1100));
   nir_ssa_def *node_dst_offset = nir_channel(&b, pconst1, 0);
   nir_ssa_def *dst_scratch_offset = nir_channel(&b, pconst1, 1);
   nir_ssa_def *src_scratch_offset = nir_channel(&b, pconst1, 2);
   nir_ssa_def *src_node_count = nir_iand_imm(&b, nir_channel(&b, pconst1, 3), 0x7FFFFFFFU);
   nir_ssa_def *fill_header =
      nir_ine_imm(&b, nir_iand_imm(&b, nir_channel(&b, pconst1, 3), 0x80000000U), 0);

   nir_ssa_def *global_id =
      nir_iadd(&b,
               nir_imul_imm(&b, nir_channels(&b, nir_load_workgroup_id(&b, 32), 1),
                            b.shader->info.workgroup_size[0]),
               nir_channels(&b, nir_load_local_invocation_id(&b), 1));
   nir_ssa_def *src_idx = nir_imul_imm(&b, global_id, 4);
   nir_ssa_def *src_count = nir_umin(&b, nir_imm_int(&b, 4), nir_isub(&b, src_node_count, src_idx));

   nir_ssa_def *node_offset = nir_iadd(&b, node_dst_offset, nir_ishl_imm(&b, global_id, 7));
   nir_ssa_def *node_dst_addr = nir_iadd(&b, node_addr, nir_u2u64(&b, node_offset));

   nir_ssa_def *src_base_addr = nir_iadd(
      &b, scratch_addr,
      nir_u2u64(&b, nir_iadd(&b, src_scratch_offset, nir_imul_imm(&b, src_idx, KEY_ID_PAIR_SIZE))));

   nir_ssa_def *src_nodes[4];
   for (uint32_t i = 0; i < 4; i++) {
      src_nodes[i] =
         nir_build_load_global(&b, 1, 32, nir_iadd_imm(&b, src_base_addr, i * KEY_ID_PAIR_SIZE));
      nir_build_store_global(&b, src_nodes[i], nir_iadd_imm(&b, node_dst_addr, i * 4));
   }

   nir_ssa_def *total_bounds[2] = {
      nir_channels(&b, nir_imm_vec4(&b, NAN, NAN, NAN, NAN), 7),
      nir_channels(&b, nir_imm_vec4(&b, NAN, NAN, NAN, NAN), 7),
   };

   for (unsigned i = 0; i < 4; ++i) {
      nir_variable *bounds[2] = {
         nir_variable_create(b.shader, nir_var_shader_temp, vec3_type, "min_bound"),
         nir_variable_create(b.shader, nir_var_shader_temp, vec3_type, "max_bound"),
      };
      nir_store_var(&b, bounds[0], nir_channels(&b, nir_imm_vec4(&b, NAN, NAN, NAN, NAN), 7), 7);
      nir_store_var(&b, bounds[1], nir_channels(&b, nir_imm_vec4(&b, NAN, NAN, NAN, NAN), 7), 7);

      nir_push_if(&b, nir_ilt(&b, nir_imm_int(&b, i), src_count));
      determine_bounds(&b, node_addr, src_nodes[i], bounds);
      nir_pop_if(&b, NULL);
      nir_build_store_global(&b, nir_load_var(&b, bounds[0]),
                             nir_iadd_imm(&b, node_dst_addr, 16 + 24 * i));
      nir_build_store_global(&b, nir_load_var(&b, bounds[1]),
                             nir_iadd_imm(&b, node_dst_addr, 28 + 24 * i));
      total_bounds[0] = nir_fmin(&b, total_bounds[0], nir_load_var(&b, bounds[0]));
      total_bounds[1] = nir_fmax(&b, total_bounds[1], nir_load_var(&b, bounds[1]));
   }

   nir_ssa_def *node_id =
      nir_iadd_imm(&b, nir_ushr_imm(&b, node_offset, 3), radv_bvh_node_internal);
   nir_ssa_def *dst_scratch_addr = nir_iadd(
      &b, scratch_addr,
      nir_u2u64(&b,
                nir_iadd(&b, dst_scratch_offset, nir_imul_imm(&b, global_id, KEY_ID_PAIR_SIZE))));
   nir_build_store_global(&b, node_id, dst_scratch_addr);

   nir_push_if(&b, fill_header);
   nir_build_store_global(&b, node_id, node_addr);
   nir_build_store_global(&b, total_bounds[0], nir_iadd_imm(&b, node_addr, 8));
   nir_build_store_global(&b, total_bounds[1], nir_iadd_imm(&b, node_addr, 20));
   nir_pop_if(&b, NULL);
   return b.shader;
}

enum copy_mode {
   COPY_MODE_COPY,
   COPY_MODE_SERIALIZE,
   COPY_MODE_DESERIALIZE,
};

struct copy_constants {
   uint64_t src_addr;
   uint64_t dst_addr;
   uint32_t mode;
};

static nir_shader *
build_copy_shader(struct radv_device *dev)
{
   nir_builder b = create_accel_build_shader(dev, "accel_copy");

   nir_ssa_def *invoc_id = nir_load_local_invocation_id(&b);
   nir_ssa_def *wg_id = nir_load_workgroup_id(&b, 32);
   nir_ssa_def *block_size =
      nir_imm_ivec4(&b, b.shader->info.workgroup_size[0], b.shader->info.workgroup_size[1],
                    b.shader->info.workgroup_size[2], 0);

   nir_ssa_def *global_id =
      nir_channel(&b, nir_iadd(&b, nir_imul(&b, wg_id, block_size), invoc_id), 0);

   nir_variable *offset_var =
      nir_variable_create(b.shader, nir_var_shader_temp, glsl_uint_type(), "offset");
   nir_ssa_def *offset = nir_imul_imm(&b, global_id, 16);
   nir_store_var(&b, offset_var, offset, 1);

   nir_ssa_def *increment = nir_imul_imm(&b, nir_channel(&b, nir_load_num_workgroups(&b, 32), 0),
                                         b.shader->info.workgroup_size[0] * 16);

   nir_ssa_def *pconst0 =
      nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .base = 0, .range = 16);
   nir_ssa_def *pconst1 =
      nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0), .base = 16, .range = 4);
   nir_ssa_def *src_base_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst0, 0b0011));
   nir_ssa_def *dst_base_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst0, 0b1100));
   nir_ssa_def *mode = nir_channel(&b, pconst1, 0);

   nir_variable *compacted_size_var =
      nir_variable_create(b.shader, nir_var_shader_temp, glsl_uint64_t_type(), "compacted_size");
   nir_variable *src_offset_var =
      nir_variable_create(b.shader, nir_var_shader_temp, glsl_uint_type(), "src_offset");
   nir_variable *dst_offset_var =
      nir_variable_create(b.shader, nir_var_shader_temp, glsl_uint_type(), "dst_offset");
   nir_variable *instance_offset_var =
      nir_variable_create(b.shader, nir_var_shader_temp, glsl_uint_type(), "instance_offset");
   nir_variable *instance_count_var =
      nir_variable_create(b.shader, nir_var_shader_temp, glsl_uint_type(), "instance_count");
   nir_variable *value_var =
      nir_variable_create(b.shader, nir_var_shader_temp, glsl_vec4_type(), "value");

   nir_push_if(&b, nir_ieq_imm(&b, mode, COPY_MODE_SERIALIZE));
   {
      nir_ssa_def *instance_count = nir_build_load_global(
         &b, 1, 32,
         nir_iadd_imm(&b, src_base_addr,
                      offsetof(struct radv_accel_struct_header, instance_count)));
      nir_ssa_def *compacted_size = nir_build_load_global(
         &b, 1, 64,
         nir_iadd_imm(&b, src_base_addr,
                      offsetof(struct radv_accel_struct_header, compacted_size)));
      nir_ssa_def *serialization_size = nir_build_load_global(
         &b, 1, 64,
         nir_iadd_imm(&b, src_base_addr,
                      offsetof(struct radv_accel_struct_header, serialization_size)));

      nir_store_var(&b, compacted_size_var, compacted_size, 1);
      nir_store_var(&b, instance_offset_var,
                    nir_build_load_global(
                       &b, 1, 32,
                       nir_iadd_imm(&b, src_base_addr,
                                    offsetof(struct radv_accel_struct_header, instance_offset))),
                    1);
      nir_store_var(&b, instance_count_var, instance_count, 1);

      nir_ssa_def *dst_offset = nir_iadd_imm(&b, nir_imul_imm(&b, instance_count, sizeof(uint64_t)),
                                             sizeof(struct radv_accel_struct_serialization_header));
      nir_store_var(&b, src_offset_var, nir_imm_int(&b, 0), 1);
      nir_store_var(&b, dst_offset_var, dst_offset, 1);

      nir_push_if(&b, nir_ieq_imm(&b, global_id, 0));
      {
         nir_build_store_global(&b, serialization_size,
                                nir_iadd_imm(&b, dst_base_addr,
                                             offsetof(struct radv_accel_struct_serialization_header,
                                                      serialization_size)));
         nir_build_store_global(
            &b, compacted_size,
            nir_iadd_imm(&b, dst_base_addr,
                         offsetof(struct radv_accel_struct_serialization_header, compacted_size)));
         nir_build_store_global(
            &b, nir_u2u64(&b, instance_count),
            nir_iadd_imm(&b, dst_base_addr,
                         offsetof(struct radv_accel_struct_serialization_header, instance_count)));
      }
      nir_pop_if(&b, NULL);
   }
   nir_push_else(&b, NULL);
   nir_push_if(&b, nir_ieq_imm(&b, mode, COPY_MODE_DESERIALIZE));
   {
      nir_ssa_def *instance_count = nir_build_load_global(
         &b, 1, 32,
         nir_iadd_imm(&b, src_base_addr,
                      offsetof(struct radv_accel_struct_serialization_header, instance_count)));
      nir_ssa_def *src_offset = nir_iadd_imm(&b, nir_imul_imm(&b, instance_count, sizeof(uint64_t)),
                                             sizeof(struct radv_accel_struct_serialization_header));

      nir_ssa_def *header_addr = nir_iadd(&b, src_base_addr, nir_u2u64(&b, src_offset));
      nir_store_var(&b, compacted_size_var,
                    nir_build_load_global(
                       &b, 1, 64,
                       nir_iadd_imm(&b, header_addr,
                                    offsetof(struct radv_accel_struct_header, compacted_size))),
                    1);
      nir_store_var(&b, instance_offset_var,
                    nir_build_load_global(
                       &b, 1, 32,
                       nir_iadd_imm(&b, header_addr,
                                    offsetof(struct radv_accel_struct_header, instance_offset))),
                    1);
      nir_store_var(&b, instance_count_var, instance_count, 1);
      nir_store_var(&b, src_offset_var, src_offset, 1);
      nir_store_var(&b, dst_offset_var, nir_imm_int(&b, 0), 1);
   }
   nir_push_else(&b, NULL); /* COPY_MODE_COPY */
   {
      nir_store_var(&b, compacted_size_var,
                    nir_build_load_global(
                       &b, 1, 64,
                       nir_iadd_imm(&b, src_base_addr,
                                    offsetof(struct radv_accel_struct_header, compacted_size))),
                    1);

      nir_store_var(&b, src_offset_var, nir_imm_int(&b, 0), 1);
      nir_store_var(&b, dst_offset_var, nir_imm_int(&b, 0), 1);
      nir_store_var(&b, instance_offset_var, nir_imm_int(&b, 0), 1);
      nir_store_var(&b, instance_count_var, nir_imm_int(&b, 0), 1);
   }
   nir_pop_if(&b, NULL);
   nir_pop_if(&b, NULL);

   nir_ssa_def *instance_bound =
      nir_imul_imm(&b, nir_load_var(&b, instance_count_var), sizeof(struct radv_bvh_instance_node));
   nir_ssa_def *compacted_size = nir_build_load_global(
      &b, 1, 32,
      nir_iadd_imm(&b, src_base_addr, offsetof(struct radv_accel_struct_header, compacted_size)));

   nir_push_loop(&b);
   {
      offset = nir_load_var(&b, offset_var);
      nir_push_if(&b, nir_ilt(&b, offset, compacted_size));
      {
         nir_ssa_def *src_offset = nir_iadd(&b, offset, nir_load_var(&b, src_offset_var));
         nir_ssa_def *dst_offset = nir_iadd(&b, offset, nir_load_var(&b, dst_offset_var));
         nir_ssa_def *src_addr = nir_iadd(&b, src_base_addr, nir_u2u64(&b, src_offset));
         nir_ssa_def *dst_addr = nir_iadd(&b, dst_base_addr, nir_u2u64(&b, dst_offset));

         nir_ssa_def *value = nir_build_load_global(&b, 4, 32, src_addr, .align_mul = 16);
         nir_store_var(&b, value_var, value, 0xf);

         nir_ssa_def *instance_offset = nir_isub(&b, offset, nir_load_var(&b, instance_offset_var));
         nir_ssa_def *in_instance_bound =
            nir_iand(&b, nir_uge(&b, offset, nir_load_var(&b, instance_offset_var)),
                     nir_ult(&b, instance_offset, instance_bound));
         nir_ssa_def *instance_start = nir_ieq_imm(
            &b, nir_iand_imm(&b, instance_offset, sizeof(struct radv_bvh_instance_node) - 1), 0);

         nir_push_if(&b, nir_iand(&b, in_instance_bound, instance_start));
         {
            nir_ssa_def *instance_id = nir_ushr_imm(&b, instance_offset, 7);

            nir_push_if(&b, nir_ieq_imm(&b, mode, COPY_MODE_SERIALIZE));
            {
               nir_ssa_def *instance_addr = nir_imul_imm(&b, instance_id, sizeof(uint64_t));
               instance_addr = nir_iadd_imm(&b, instance_addr,
                                            sizeof(struct radv_accel_struct_serialization_header));
               instance_addr = nir_iadd(&b, dst_base_addr, nir_u2u64(&b, instance_addr));

               nir_build_store_global(&b, nir_channels(&b, value, 3), instance_addr,
                                      .align_mul = 8);
            }
            nir_push_else(&b, NULL);
            {
               nir_ssa_def *instance_addr = nir_imul_imm(&b, instance_id, sizeof(uint64_t));
               instance_addr = nir_iadd_imm(&b, instance_addr,
                                            sizeof(struct radv_accel_struct_serialization_header));
               instance_addr = nir_iadd(&b, src_base_addr, nir_u2u64(&b, instance_addr));

               nir_ssa_def *instance_value =
                  nir_build_load_global(&b, 2, 32, instance_addr, .align_mul = 8);

               nir_ssa_def *values[] = {
                  nir_channel(&b, instance_value, 0),
                  nir_channel(&b, instance_value, 1),
                  nir_channel(&b, value, 2),
                  nir_channel(&b, value, 3),
               };

               nir_store_var(&b, value_var, nir_vec(&b, values, 4), 0xf);
            }
            nir_pop_if(&b, NULL);
         }
         nir_pop_if(&b, NULL);

         nir_store_var(&b, offset_var, nir_iadd(&b, offset, increment), 1);

         nir_build_store_global(&b, nir_load_var(&b, value_var), dst_addr, .align_mul = 16);
      }
      nir_push_else(&b, NULL);
      {
         nir_jump(&b, nir_jump_break);
      }
      nir_pop_if(&b, NULL);
   }
   nir_pop_loop(&b, NULL);
   return b.shader;
}

void
radv_device_finish_accel_struct_build_state(struct radv_device *device)
{
   struct radv_meta_state *state = &device->meta_state;
   radv_DestroyPipeline(radv_device_to_handle(device), state->accel_struct_build.copy_pipeline,
                        &state->alloc);
   radv_DestroyPipeline(radv_device_to_handle(device), state->accel_struct_build.internal_pipeline,
                        &state->alloc);
   radv_DestroyPipeline(radv_device_to_handle(device), state->accel_struct_build.leaf_pipeline,
                        &state->alloc);
   radv_DestroyPipeline(radv_device_to_handle(device), state->accel_struct_build.morton_pipeline,
                        &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device),
                              state->accel_struct_build.copy_p_layout, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device),
                              state->accel_struct_build.internal_p_layout, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device),
                              state->accel_struct_build.leaf_p_layout, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device),
                              state->accel_struct_build.morton_p_layout, &state->alloc);

   if (state->accel_struct_build.radix_sort)
      radix_sort_vk_destroy(state->accel_struct_build.radix_sort, radv_device_to_handle(device),
                            &state->alloc);
}

static VkResult
create_build_pipeline(struct radv_device *device, nir_shader *shader, unsigned push_constant_size,
                      VkPipeline *pipeline, VkPipelineLayout *layout)
{
   const VkPipelineLayoutCreateInfo pl_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 0,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges =
         &(VkPushConstantRange){VK_SHADER_STAGE_COMPUTE_BIT, 0, push_constant_size},
   };

   VkResult result = radv_CreatePipelineLayout(radv_device_to_handle(device), &pl_create_info,
                                               &device->meta_state.alloc, layout);
   if (result != VK_SUCCESS) {
      ralloc_free(shader);
      return result;
   }

   VkPipelineShaderStageCreateInfo shader_stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = vk_shader_module_handle_from_nir(shader),
      .pName = "main",
      .pSpecializationInfo = NULL,
   };

   VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = shader_stage,
      .flags = 0,
      .layout = *layout,
   };

   result = radv_CreateComputePipelines(radv_device_to_handle(device),
                                        radv_pipeline_cache_to_handle(&device->meta_state.cache), 1,
                                        &pipeline_info, &device->meta_state.alloc, pipeline);

   if (result != VK_SUCCESS) {
      ralloc_free(shader);
      return result;
   }

   return VK_SUCCESS;
}

static VkResult
create_build_pipeline_spv(struct radv_device *device, const uint32_t *spv, uint32_t spv_size,
                          unsigned push_constant_size, VkPipeline *pipeline,
                          VkPipelineLayout *layout)
{
   const VkPipelineLayoutCreateInfo pl_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 0,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges =
         &(VkPushConstantRange){VK_SHADER_STAGE_COMPUTE_BIT, 0, push_constant_size},
   };

   VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .codeSize = spv_size,
      .pCode = spv,
   };

   VkShaderModule module;
   VkResult result = device->vk.dispatch_table.CreateShaderModule(
      radv_device_to_handle(device), &module_info, &device->meta_state.alloc, &module);
   if (result != VK_SUCCESS)
      return result;

   result = radv_CreatePipelineLayout(radv_device_to_handle(device), &pl_create_info,
                                      &device->meta_state.alloc, layout);
   if (result != VK_SUCCESS)
      goto cleanup;

   VkPipelineShaderStageCreateInfo shader_stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = module,
      .pName = "main",
      .pSpecializationInfo = NULL,
   };

   VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = shader_stage,
      .flags = 0,
      .layout = *layout,
   };

   result = radv_CreateComputePipelines(radv_device_to_handle(device),
                                        radv_pipeline_cache_to_handle(&device->meta_state.cache), 1,
                                        &pipeline_info, &device->meta_state.alloc, pipeline);

cleanup:
   device->vk.dispatch_table.DestroyShaderModule(radv_device_to_handle(device), module,
                                                 &device->meta_state.alloc);
   return result;
}

static void
radix_sort_fill_buffer(VkCommandBuffer commandBuffer,
                       radix_sort_vk_buffer_info_t const *buffer_info, VkDeviceSize offset,
                       VkDeviceSize size, uint32_t data)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   assert(size != VK_WHOLE_SIZE);

   radv_fill_buffer(cmd_buffer, NULL, NULL, buffer_info->devaddr + buffer_info->offset + offset,
                    size, data);
}

VkResult
radv_device_init_accel_struct_build_state(struct radv_device *device)
{
   VkResult result;
   nir_shader *leaf_cs = build_leaf_shader(device);
   nir_shader *internal_cs = build_internal_shader(device);
   nir_shader *copy_cs = build_copy_shader(device);

   result = create_build_pipeline(device, leaf_cs, sizeof(struct build_primitive_constants),
                                  &device->meta_state.accel_struct_build.leaf_pipeline,
                                  &device->meta_state.accel_struct_build.leaf_p_layout);
   if (result != VK_SUCCESS)
      return result;

   result = create_build_pipeline(device, internal_cs, sizeof(struct build_internal_constants),
                                  &device->meta_state.accel_struct_build.internal_pipeline,
                                  &device->meta_state.accel_struct_build.internal_p_layout);
   if (result != VK_SUCCESS)
      return result;

   result = create_build_pipeline(device, copy_cs, sizeof(struct copy_constants),
                                  &device->meta_state.accel_struct_build.copy_pipeline,
                                  &device->meta_state.accel_struct_build.copy_p_layout);

   if (result != VK_SUCCESS)
      return result;

   result = create_build_pipeline_spv(device, morton_spv, sizeof(morton_spv),
                                      sizeof(struct morton_constants),
                                      &device->meta_state.accel_struct_build.morton_pipeline,
                                      &device->meta_state.accel_struct_build.morton_p_layout);
   if (result != VK_SUCCESS)
      return result;

   device->meta_state.accel_struct_build.radix_sort =
      radv_create_radix_sort_u64(radv_device_to_handle(device), &device->meta_state.alloc,
                                 radv_pipeline_cache_to_handle(&device->meta_state.cache));

   struct radix_sort_vk_sort_devaddr_info *radix_sort_info =
      &device->meta_state.accel_struct_build.radix_sort_info;
   radix_sort_info->ext = NULL;
   radix_sort_info->key_bits = 24;
   radix_sort_info->fill_buffer = radix_sort_fill_buffer;

   return result;
}

struct bvh_state {
   uint32_t node_offset;
   uint32_t node_count;
   uint32_t scratch_offset;
   uint32_t buffer_1_offset;
   uint32_t buffer_2_offset;

   uint32_t instance_offset;
   uint32_t instance_count;
};

VKAPI_ATTR void VKAPI_CALL
radv_CmdBuildAccelerationStructuresKHR(
   VkCommandBuffer commandBuffer, uint32_t infoCount,
   const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
   const VkAccelerationStructureBuildRangeInfoKHR *const *ppBuildRangeInfos)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_meta_saved_state saved_state;

   enum radv_cmd_flush_bits flush_bits =
      RADV_CMD_FLAG_CS_PARTIAL_FLUSH |
      radv_src_access_flush(cmd_buffer, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                            NULL) |
      radv_dst_access_flush(cmd_buffer, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                            NULL);

   radv_meta_save(
      &saved_state, cmd_buffer,
      RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_DESCRIPTORS | RADV_META_SAVE_CONSTANTS);
   struct bvh_state *bvh_states = calloc(infoCount, sizeof(struct bvh_state));

   for (uint32_t i = 0; i < infoCount; ++i) {
      /* Clear the bvh bounds with int max/min. */
      si_cp_dma_clear_buffer(cmd_buffer, pInfos[i].scratchData.deviceAddress, 3 * sizeof(float),
                             0x7fffffff);
      si_cp_dma_clear_buffer(cmd_buffer, pInfos[i].scratchData.deviceAddress + 3 * sizeof(float),
                             3 * sizeof(float), 0x80000000);
   }

   cmd_buffer->state.flush_bits |= flush_bits;

   radv_CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.leaf_pipeline);

   for (uint32_t i = 0; i < infoCount; ++i) {
      RADV_FROM_HANDLE(radv_acceleration_structure, accel_struct,
                       pInfos[i].dstAccelerationStructure);

      struct build_primitive_constants prim_consts = {
         .node_dst_addr = radv_accel_struct_get_va(accel_struct),
         .scratch_addr = pInfos[i].scratchData.deviceAddress,
         .dst_offset = ALIGN(sizeof(struct radv_accel_struct_header), 64) + 128,
         .dst_scratch_offset = 0,
      };
      bvh_states[i].node_offset = prim_consts.dst_offset;
      bvh_states[i].instance_offset = prim_consts.dst_offset;

      for (int inst = 1; inst >= 0; --inst) {
         for (unsigned j = 0; j < pInfos[i].geometryCount; ++j) {
            const VkAccelerationStructureGeometryKHR *geom =
               pInfos[i].pGeometries ? &pInfos[i].pGeometries[j] : pInfos[i].ppGeometries[j];

            if (!inst == (geom->geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR))
               continue;

            const VkAccelerationStructureBuildRangeInfoKHR *buildRangeInfo =
               &ppBuildRangeInfos[i][j];

            prim_consts.geometry_type = geom->geometryType;
            prim_consts.geometry_id = j | (geom->flags << 28);
            unsigned prim_size;
            switch (geom->geometryType) {
            case VK_GEOMETRY_TYPE_TRIANGLES_KHR:
               prim_consts.vertex_addr =
                  geom->geometry.triangles.vertexData.deviceAddress +
                  buildRangeInfo->firstVertex * geom->geometry.triangles.vertexStride;
               prim_consts.index_addr = geom->geometry.triangles.indexData.deviceAddress;

               if (geom->geometry.triangles.indexType == VK_INDEX_TYPE_NONE_KHR)
                  prim_consts.vertex_addr += buildRangeInfo->primitiveOffset;
               else
                  prim_consts.index_addr += buildRangeInfo->primitiveOffset;

               prim_consts.transform_addr = geom->geometry.triangles.transformData.deviceAddress;
               if (prim_consts.transform_addr)
                  prim_consts.transform_addr += buildRangeInfo->transformOffset;

               prim_consts.vertex_stride = geom->geometry.triangles.vertexStride;
               prim_consts.vertex_format = geom->geometry.triangles.vertexFormat;
               prim_consts.index_format = geom->geometry.triangles.indexType;
               prim_size = 64;
               break;
            case VK_GEOMETRY_TYPE_AABBS_KHR:
               prim_consts.aabb_addr =
                  geom->geometry.aabbs.data.deviceAddress + buildRangeInfo->primitiveOffset;
               prim_consts.aabb_stride = geom->geometry.aabbs.stride;
               prim_size = 64;
               break;
            case VK_GEOMETRY_TYPE_INSTANCES_KHR:
               prim_consts.instance_data =
                  geom->geometry.instances.data.deviceAddress + buildRangeInfo->primitiveOffset;
               prim_consts.array_of_pointers = geom->geometry.instances.arrayOfPointers ? 1 : 0;
               prim_size = 128;
               bvh_states[i].instance_count += buildRangeInfo->primitiveCount;
               break;
            default:
               unreachable("Unknown geometryType");
            }

            radv_CmdPushConstants(
               commandBuffer, cmd_buffer->device->meta_state.accel_struct_build.leaf_p_layout,
               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(prim_consts), &prim_consts);
            radv_unaligned_dispatch(cmd_buffer, buildRangeInfo->primitiveCount, 1, 1);
            prim_consts.dst_offset += prim_size * buildRangeInfo->primitiveCount;
            prim_consts.dst_scratch_offset += KEY_ID_PAIR_SIZE * buildRangeInfo->primitiveCount;
         }
      }
      bvh_states[i].node_offset = prim_consts.dst_offset;
      bvh_states[i].node_count = prim_consts.dst_scratch_offset / KEY_ID_PAIR_SIZE;
   }

   cmd_buffer->state.flush_bits |= flush_bits;

   radv_CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.morton_pipeline);

   for (uint32_t i = 0; i < infoCount; ++i) {
      RADV_FROM_HANDLE(radv_acceleration_structure, accel_struct,
                       pInfos[i].dstAccelerationStructure);

      const struct morton_constants consts = {
         .bvh_addr = radv_accel_struct_get_va(accel_struct),
         .bounds_addr = pInfos[i].scratchData.deviceAddress,
         .ids_addr = pInfos[i].scratchData.deviceAddress + SCRATCH_TOTAL_BOUNDS_SIZE,
      };

      radv_CmdPushConstants(commandBuffer,
                            cmd_buffer->device->meta_state.accel_struct_build.morton_p_layout,
                            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(consts), &consts);
      radv_unaligned_dispatch(cmd_buffer, bvh_states[i].node_count, 1, 1);
   }

   cmd_buffer->state.flush_bits |= flush_bits;

   for (uint32_t i = 0; i < infoCount; ++i) {
      struct radix_sort_vk_memory_requirements requirements;
      radix_sort_vk_get_memory_requirements(
         cmd_buffer->device->meta_state.accel_struct_build.radix_sort, bvh_states[i].node_count,
         &requirements);

      struct radix_sort_vk_sort_devaddr_info info =
         cmd_buffer->device->meta_state.accel_struct_build.radix_sort_info;
      info.count = bvh_states[i].node_count;

      VkDeviceAddress base_addr = pInfos[i].scratchData.deviceAddress + SCRATCH_TOTAL_BOUNDS_SIZE;

      info.keyvals_even.buffer = VK_NULL_HANDLE;
      info.keyvals_even.offset = 0;
      info.keyvals_even.devaddr = base_addr;

      info.keyvals_odd = base_addr + requirements.keyvals_size;

      info.internal.buffer = VK_NULL_HANDLE;
      info.internal.offset = 0;
      info.internal.devaddr = base_addr + requirements.keyvals_size * 2;

      VkDeviceAddress result_addr;
      radix_sort_vk_sort_devaddr(cmd_buffer->device->meta_state.accel_struct_build.radix_sort,
                                 &info, radv_device_to_handle(cmd_buffer->device), commandBuffer,
                                 &result_addr);

      assert(result_addr == info.keyvals_even.devaddr || result_addr == info.keyvals_odd);

      if (result_addr == info.keyvals_even.devaddr) {
         bvh_states[i].buffer_1_offset = SCRATCH_TOTAL_BOUNDS_SIZE;
         bvh_states[i].buffer_2_offset = SCRATCH_TOTAL_BOUNDS_SIZE + requirements.keyvals_size;
      } else {
         bvh_states[i].buffer_1_offset = SCRATCH_TOTAL_BOUNDS_SIZE + requirements.keyvals_size;
         bvh_states[i].buffer_2_offset = SCRATCH_TOTAL_BOUNDS_SIZE;
      }
      bvh_states[i].scratch_offset = bvh_states[i].buffer_1_offset;
   }

   cmd_buffer->state.flush_bits |= flush_bits;

   radv_CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.internal_pipeline);
   bool progress = true;
   for (unsigned iter = 0; progress; ++iter) {
      progress = false;
      for (uint32_t i = 0; i < infoCount; ++i) {
         RADV_FROM_HANDLE(radv_acceleration_structure, accel_struct,
                          pInfos[i].dstAccelerationStructure);

         if (iter && bvh_states[i].node_count == 1)
            continue;

         if (!progress)
            cmd_buffer->state.flush_bits |= flush_bits;

         progress = true;

         uint32_t dst_node_count = MAX2(1, DIV_ROUND_UP(bvh_states[i].node_count, 4));
         bool final_iter = dst_node_count == 1;

         uint32_t src_scratch_offset = bvh_states[i].scratch_offset;
         uint32_t buffer_1_offset = bvh_states[i].buffer_1_offset;
         uint32_t buffer_2_offset = bvh_states[i].buffer_2_offset;
         uint32_t dst_scratch_offset =
            (src_scratch_offset == buffer_1_offset) ? buffer_2_offset : buffer_1_offset;

         uint32_t dst_node_offset = bvh_states[i].node_offset;
         if (final_iter)
            dst_node_offset = ALIGN(sizeof(struct radv_accel_struct_header), 64);

         const struct build_internal_constants consts = {
            .node_dst_addr = radv_accel_struct_get_va(accel_struct),
            .scratch_addr = pInfos[i].scratchData.deviceAddress,
            .dst_offset = dst_node_offset,
            .dst_scratch_offset = dst_scratch_offset,
            .src_scratch_offset = src_scratch_offset,
            .fill_header = bvh_states[i].node_count | (final_iter ? 0x80000000U : 0),
         };

         radv_CmdPushConstants(commandBuffer,
                               cmd_buffer->device->meta_state.accel_struct_build.internal_p_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(consts), &consts);
         radv_unaligned_dispatch(cmd_buffer, dst_node_count, 1, 1);
         if (!final_iter)
            bvh_states[i].node_offset += dst_node_count * 128;
         bvh_states[i].node_count = dst_node_count;
         bvh_states[i].scratch_offset = dst_scratch_offset;
      }
   }
   for (uint32_t i = 0; i < infoCount; ++i) {
      RADV_FROM_HANDLE(radv_acceleration_structure, accel_struct,
                       pInfos[i].dstAccelerationStructure);
      const size_t base = offsetof(struct radv_accel_struct_header, compacted_size);
      struct radv_accel_struct_header header;

      header.instance_offset = bvh_states[i].instance_offset;
      header.instance_count = bvh_states[i].instance_count;
      header.compacted_size = bvh_states[i].node_offset;

      header.copy_dispatch_size[0] = DIV_ROUND_UP(header.compacted_size, 16 * 64);
      header.copy_dispatch_size[1] = 1;
      header.copy_dispatch_size[2] = 1;

      header.serialization_size =
         header.compacted_size + align(sizeof(struct radv_accel_struct_serialization_header) +
                                          sizeof(uint64_t) * header.instance_count,
                                       128);

      header.size = header.serialization_size -
                    sizeof(struct radv_accel_struct_serialization_header) -
                    sizeof(uint64_t) * header.instance_count;

      radv_update_buffer_cp(cmd_buffer,
                            radv_buffer_get_va(accel_struct->bo) + accel_struct->mem_offset + base,
                            (const char *)&header + base, sizeof(header) - base);
   }
   free(bvh_states);
   radv_meta_restore(&saved_state, cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdCopyAccelerationStructureKHR(VkCommandBuffer commandBuffer,
                                     const VkCopyAccelerationStructureInfoKHR *pInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_acceleration_structure, src, pInfo->src);
   RADV_FROM_HANDLE(radv_acceleration_structure, dst, pInfo->dst);
   struct radv_meta_saved_state saved_state;

   radv_meta_save(
      &saved_state, cmd_buffer,
      RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_DESCRIPTORS | RADV_META_SAVE_CONSTANTS);

   uint64_t src_addr = radv_accel_struct_get_va(src);
   uint64_t dst_addr = radv_accel_struct_get_va(dst);

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.copy_pipeline);

   const struct copy_constants consts = {
      .src_addr = src_addr,
      .dst_addr = dst_addr,
      .mode = COPY_MODE_COPY,
   };

   radv_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer),
                         cmd_buffer->device->meta_state.accel_struct_build.copy_p_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(consts), &consts);

   cmd_buffer->state.flush_bits |=
      radv_dst_access_flush(cmd_buffer, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, NULL);

   radv_indirect_dispatch(cmd_buffer, src->bo,
                          src_addr + offsetof(struct radv_accel_struct_header, copy_dispatch_size));
   radv_meta_restore(&saved_state, cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_GetDeviceAccelerationStructureCompatibilityKHR(
   VkDevice _device, const VkAccelerationStructureVersionInfoKHR *pVersionInfo,
   VkAccelerationStructureCompatibilityKHR *pCompatibility)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   uint8_t zero[VK_UUID_SIZE] = {
      0,
   };
   bool compat =
      memcmp(pVersionInfo->pVersionData, device->physical_device->driver_uuid, VK_UUID_SIZE) == 0 &&
      memcmp(pVersionInfo->pVersionData + VK_UUID_SIZE, zero, VK_UUID_SIZE) == 0;
   *pCompatibility = compat ? VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR
                            : VK_ACCELERATION_STRUCTURE_COMPATIBILITY_INCOMPATIBLE_KHR;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CopyMemoryToAccelerationStructureKHR(VkDevice _device,
                                          VkDeferredOperationKHR deferredOperation,
                                          const VkCopyMemoryToAccelerationStructureInfoKHR *pInfo)
{
   unreachable("Unimplemented");
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CopyAccelerationStructureToMemoryKHR(VkDevice _device,
                                          VkDeferredOperationKHR deferredOperation,
                                          const VkCopyAccelerationStructureToMemoryInfoKHR *pInfo)
{
   unreachable("Unimplemented");
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdCopyMemoryToAccelerationStructureKHR(
   VkCommandBuffer commandBuffer, const VkCopyMemoryToAccelerationStructureInfoKHR *pInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_acceleration_structure, dst, pInfo->dst);
   struct radv_meta_saved_state saved_state;

   radv_meta_save(
      &saved_state, cmd_buffer,
      RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_DESCRIPTORS | RADV_META_SAVE_CONSTANTS);

   uint64_t dst_addr = radv_accel_struct_get_va(dst);

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.copy_pipeline);

   const struct copy_constants consts = {
      .src_addr = pInfo->src.deviceAddress,
      .dst_addr = dst_addr,
      .mode = COPY_MODE_DESERIALIZE,
   };

   radv_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer),
                         cmd_buffer->device->meta_state.accel_struct_build.copy_p_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(consts), &consts);

   radv_CmdDispatch(commandBuffer, 512, 1, 1);
   radv_meta_restore(&saved_state, cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdCopyAccelerationStructureToMemoryKHR(
   VkCommandBuffer commandBuffer, const VkCopyAccelerationStructureToMemoryInfoKHR *pInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_acceleration_structure, src, pInfo->src);
   struct radv_meta_saved_state saved_state;

   radv_meta_save(
      &saved_state, cmd_buffer,
      RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_DESCRIPTORS | RADV_META_SAVE_CONSTANTS);

   uint64_t src_addr = radv_accel_struct_get_va(src);

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.copy_pipeline);

   const struct copy_constants consts = {
      .src_addr = src_addr,
      .dst_addr = pInfo->dst.deviceAddress,
      .mode = COPY_MODE_SERIALIZE,
   };

   radv_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer),
                         cmd_buffer->device->meta_state.accel_struct_build.copy_p_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(consts), &consts);

   cmd_buffer->state.flush_bits |=
      radv_dst_access_flush(cmd_buffer, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, NULL);

   radv_indirect_dispatch(cmd_buffer, src->bo,
                          src_addr + offsetof(struct radv_accel_struct_header, copy_dispatch_size));
   radv_meta_restore(&saved_state, cmd_buffer);

   /* Set the header of the serialized data. */
   uint8_t header_data[2 * VK_UUID_SIZE] = {0};
   memcpy(header_data, cmd_buffer->device->physical_device->driver_uuid, VK_UUID_SIZE);

   radv_update_buffer_cp(cmd_buffer, pInfo->dst.deviceAddress, header_data, sizeof(header_data));
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBuildAccelerationStructuresIndirectKHR(
   VkCommandBuffer commandBuffer, uint32_t infoCount,
   const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
   const VkDeviceAddress *pIndirectDeviceAddresses, const uint32_t *pIndirectStrides,
   const uint32_t *const *ppMaxPrimitiveCounts)
{
   unreachable("Unimplemented");
}
