/*
 * Copyright © 2021 Google
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
#include "radv_shader.h"

#include "nir/nir.h"
#include "nir/nir_builder.h"

static VkRayTracingPipelineCreateInfoKHR
radv_create_merged_rt_create_info(const VkRayTracingPipelineCreateInfoKHR *pCreateInfo)
{
   VkRayTracingPipelineCreateInfoKHR local_create_info = *pCreateInfo;
   uint32_t total_stages = pCreateInfo->stageCount;
   uint32_t total_groups = pCreateInfo->groupCount;

   if (pCreateInfo->pLibraryInfo) {
      for (unsigned i = 0; i < pCreateInfo->pLibraryInfo->libraryCount; ++i) {
         RADV_FROM_HANDLE(radv_pipeline, library, pCreateInfo->pLibraryInfo->pLibraries[i]);
         total_stages += library->library.stage_count;
         total_groups += library->library.group_count;
      }
   }
   VkPipelineShaderStageCreateInfo *stages = NULL;
   VkRayTracingShaderGroupCreateInfoKHR *groups = NULL;
   local_create_info.stageCount = total_stages;
   local_create_info.groupCount = total_groups;
   local_create_info.pStages = stages =
      malloc(sizeof(VkPipelineShaderStageCreateInfo) * total_stages);
   local_create_info.pGroups = groups =
      malloc(sizeof(VkRayTracingShaderGroupCreateInfoKHR) * total_groups);
   if (!local_create_info.pStages || !local_create_info.pGroups)
      return local_create_info;

   total_stages = pCreateInfo->stageCount;
   total_groups = pCreateInfo->groupCount;
   for (unsigned j = 0; j < pCreateInfo->stageCount; ++j)
      stages[j] = pCreateInfo->pStages[j];
   for (unsigned j = 0; j < pCreateInfo->groupCount; ++j)
      groups[j] = pCreateInfo->pGroups[j];

   if (pCreateInfo->pLibraryInfo) {
      for (unsigned i = 0; i < pCreateInfo->pLibraryInfo->libraryCount; ++i) {
         RADV_FROM_HANDLE(radv_pipeline, library, pCreateInfo->pLibraryInfo->pLibraries[i]);
         for (unsigned j = 0; j < library->library.stage_count; ++j)
            stages[total_stages + j] = library->library.stages[j];
         for (unsigned j = 0; j < library->library.group_count; ++j) {
            VkRayTracingShaderGroupCreateInfoKHR *dst = &groups[total_groups + j];
            *dst = library->library.groups[j];
            if (dst->generalShader != VK_SHADER_UNUSED_KHR)
               dst->generalShader += total_stages;
            if (dst->closestHitShader != VK_SHADER_UNUSED_KHR)
               dst->closestHitShader += total_stages;
            if (dst->anyHitShader != VK_SHADER_UNUSED_KHR)
               dst->anyHitShader += total_stages;
            if (dst->intersectionShader != VK_SHADER_UNUSED_KHR)
               dst->intersectionShader += total_stages;
         }
         total_stages += library->library.stage_count;
         total_groups += library->library.group_count;
      }
   }
   return local_create_info;
}

static VkResult
radv_rt_pipeline_library_create(VkDevice _device, VkPipelineCache _cache,
                                const VkRayTracingPipelineCreateInfoKHR *pCreateInfo,
                                const VkAllocationCallbacks *pAllocator, VkPipeline *pPipeline)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   struct radv_pipeline *pipeline;

   pipeline = vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*pipeline), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &pipeline->base, VK_OBJECT_TYPE_PIPELINE);
   pipeline->type = RADV_PIPELINE_LIBRARY;

   VkRayTracingPipelineCreateInfoKHR local_create_info =
      radv_create_merged_rt_create_info(pCreateInfo);
   if (!local_create_info.pStages || !local_create_info.pGroups)
      goto fail;

   if (local_create_info.stageCount) {
      size_t size = sizeof(VkPipelineShaderStageCreateInfo) * local_create_info.stageCount;
      pipeline->library.stage_count = local_create_info.stageCount;
      pipeline->library.stages = malloc(size);
      if (!pipeline->library.stages)
         goto fail;
      memcpy(pipeline->library.stages, local_create_info.pStages, size);
   }

   if (local_create_info.groupCount) {
      size_t size = sizeof(VkRayTracingShaderGroupCreateInfoKHR) * local_create_info.groupCount;
      pipeline->library.group_count = local_create_info.groupCount;
      pipeline->library.groups = malloc(size);
      if (!pipeline->library.groups)
         goto fail;
      memcpy(pipeline->library.groups, local_create_info.pGroups, size);
   }

   *pPipeline = radv_pipeline_to_handle(pipeline);

   free((void *)local_create_info.pGroups);
   free((void *)local_create_info.pStages);
   return VK_SUCCESS;
fail:
   free(pipeline->library.groups);
   free(pipeline->library.stages);
   free((void *)local_create_info.pGroups);
   free((void *)local_create_info.pStages);
   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

/*
 * Global variables for an RT pipeline
 */
struct rt_variables {
   /* idx of the next shader to run in the next iteration of the main loop */
   nir_variable *idx;

   /* scratch offset of the argument area relative to stack_ptr */
   nir_variable *arg;

   nir_variable *stack_ptr;

   /* global address of the SBT entry used for the shader */
   nir_variable *shader_record_ptr;

   /* trace_ray arguments */
   nir_variable *accel_struct;
   nir_variable *flags;
   nir_variable *cull_mask;
   nir_variable *sbt_offset;
   nir_variable *sbt_stride;
   nir_variable *miss_index;
   nir_variable *origin;
   nir_variable *tmin;
   nir_variable *direction;
   nir_variable *tmax;

   /* from the BTAS instance currently being visited */
   nir_variable *custom_instance_and_mask;

   /* Properties of the primitive currently being visited. */
   nir_variable *primitive_id;
   nir_variable *geometry_id_and_flags;
   nir_variable *instance_id;
   nir_variable *instance_addr;
   nir_variable *hit_kind;
   nir_variable *opaque;

   /* Safeguard to ensure we don't end up in an infinite loop of non-existing case. Should not be
    * needed but is extra anti-hang safety during bring-up. */
   nir_variable *main_loop_case_visited;

   /* Output variable for intersection & anyhit shaders. */
   nir_variable *ahit_status;

   /* Array of stack size struct for recording the max stack size for each group. */
   struct radv_pipeline_shader_stack_size *stack_sizes;
   unsigned group_idx;
};

static struct rt_variables
create_rt_variables(nir_shader *shader, struct radv_pipeline_shader_stack_size *stack_sizes)
{
   struct rt_variables vars = {
      NULL,
   };
   vars.idx = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "idx");
   vars.arg = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "arg");
   vars.stack_ptr = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "stack_ptr");
   vars.shader_record_ptr =
      nir_variable_create(shader, nir_var_shader_temp, glsl_uint64_t_type(), "shader_record_ptr");

   const struct glsl_type *vec3_type = glsl_vector_type(GLSL_TYPE_FLOAT, 3);
   vars.accel_struct =
      nir_variable_create(shader, nir_var_shader_temp, glsl_uint64_t_type(), "accel_struct");
   vars.flags = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "ray_flags");
   vars.cull_mask = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "cull_mask");
   vars.sbt_offset =
      nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "sbt_offset");
   vars.sbt_stride =
      nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "sbt_stride");
   vars.miss_index =
      nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "miss_index");
   vars.origin = nir_variable_create(shader, nir_var_shader_temp, vec3_type, "ray_origin");
   vars.tmin = nir_variable_create(shader, nir_var_shader_temp, glsl_float_type(), "ray_tmin");
   vars.direction = nir_variable_create(shader, nir_var_shader_temp, vec3_type, "ray_direction");
   vars.tmax = nir_variable_create(shader, nir_var_shader_temp, glsl_float_type(), "ray_tmax");

   vars.custom_instance_and_mask = nir_variable_create(
      shader, nir_var_shader_temp, glsl_uint_type(), "custom_instance_and_mask");
   vars.primitive_id =
      nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "primitive_id");
   vars.geometry_id_and_flags =
      nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "geometry_id_and_flags");
   vars.instance_id =
      nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "instance_id");
   vars.instance_addr =
      nir_variable_create(shader, nir_var_shader_temp, glsl_uint64_t_type(), "instance_addr");
   vars.hit_kind = nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "hit_kind");
   vars.opaque = nir_variable_create(shader, nir_var_shader_temp, glsl_bool_type(), "opaque");

   vars.main_loop_case_visited =
      nir_variable_create(shader, nir_var_shader_temp, glsl_bool_type(), "main_loop_case_visited");
   vars.ahit_status =
      nir_variable_create(shader, nir_var_shader_temp, glsl_uint_type(), "ahit_status");

   vars.stack_sizes = stack_sizes;
   return vars;
}

/*
 * Remap all the variables between the two rt_variables struct for inlining.
 */
static void
map_rt_variables(struct hash_table *var_remap, struct rt_variables *src,
                 const struct rt_variables *dst)
{
   _mesa_hash_table_insert(var_remap, src->idx, dst->idx);
   _mesa_hash_table_insert(var_remap, src->arg, dst->arg);
   _mesa_hash_table_insert(var_remap, src->stack_ptr, dst->stack_ptr);
   _mesa_hash_table_insert(var_remap, src->shader_record_ptr, dst->shader_record_ptr);

   _mesa_hash_table_insert(var_remap, src->accel_struct, dst->accel_struct);
   _mesa_hash_table_insert(var_remap, src->flags, dst->flags);
   _mesa_hash_table_insert(var_remap, src->cull_mask, dst->cull_mask);
   _mesa_hash_table_insert(var_remap, src->sbt_offset, dst->sbt_offset);
   _mesa_hash_table_insert(var_remap, src->sbt_stride, dst->sbt_stride);
   _mesa_hash_table_insert(var_remap, src->miss_index, dst->miss_index);
   _mesa_hash_table_insert(var_remap, src->origin, dst->origin);
   _mesa_hash_table_insert(var_remap, src->tmin, dst->tmin);
   _mesa_hash_table_insert(var_remap, src->direction, dst->direction);
   _mesa_hash_table_insert(var_remap, src->tmax, dst->tmax);

   _mesa_hash_table_insert(var_remap, src->custom_instance_and_mask, dst->custom_instance_and_mask);
   _mesa_hash_table_insert(var_remap, src->primitive_id, dst->primitive_id);
   _mesa_hash_table_insert(var_remap, src->geometry_id_and_flags, dst->geometry_id_and_flags);
   _mesa_hash_table_insert(var_remap, src->instance_id, dst->instance_id);
   _mesa_hash_table_insert(var_remap, src->instance_addr, dst->instance_addr);
   _mesa_hash_table_insert(var_remap, src->hit_kind, dst->hit_kind);
   _mesa_hash_table_insert(var_remap, src->opaque, dst->opaque);
   _mesa_hash_table_insert(var_remap, src->ahit_status, dst->ahit_status);

   src->stack_sizes = dst->stack_sizes;
   src->group_idx = dst->group_idx;
}

/*
 * Create a copy of the global rt variables where the primitive/instance related variables are
 * independent.This is needed as we need to keep the old values of the global variables around
 * in case e.g. an anyhit shader reject the collision. So there are inner variables that get copied
 * to the outer variables once we commit to a better hit.
 */
static struct rt_variables
create_inner_vars(nir_builder *b, const struct rt_variables *vars)
{
   struct rt_variables inner_vars = *vars;
   inner_vars.idx =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "inner_idx");
   inner_vars.shader_record_ptr = nir_variable_create(
      b->shader, nir_var_shader_temp, glsl_uint64_t_type(), "inner_shader_record_ptr");
   inner_vars.primitive_id =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "inner_primitive_id");
   inner_vars.geometry_id_and_flags = nir_variable_create(
      b->shader, nir_var_shader_temp, glsl_uint_type(), "inner_geometry_id_and_flags");
   inner_vars.tmax =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_float_type(), "inner_tmax");
   inner_vars.instance_id =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "inner_instance_id");
   inner_vars.instance_addr = nir_variable_create(b->shader, nir_var_shader_temp,
                                                  glsl_uint64_t_type(), "inner_instance_addr");
   inner_vars.hit_kind =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_uint_type(), "inner_hit_kind");
   inner_vars.custom_instance_and_mask = nir_variable_create(
      b->shader, nir_var_shader_temp, glsl_uint_type(), "inner_custom_instance_and_mask");

   return inner_vars;
}

static nir_shader *
create_rt_shader(struct radv_device *device, const VkRayTracingPipelineCreateInfoKHR *pCreateInfo,
                 struct radv_pipeline_shader_stack_size *stack_sizes)
{
   /* TODO */
   return NULL;
}

static VkResult
radv_rt_pipeline_create(VkDevice _device, VkPipelineCache _cache,
                        const VkRayTracingPipelineCreateInfoKHR *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator, VkPipeline *pPipeline)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   VkResult result;
   struct radv_pipeline *pipeline = NULL;
   struct radv_pipeline_shader_stack_size *stack_sizes = NULL;

   if (pCreateInfo->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR)
      return radv_rt_pipeline_library_create(_device, _cache, pCreateInfo, pAllocator, pPipeline);

   VkRayTracingPipelineCreateInfoKHR local_create_info =
      radv_create_merged_rt_create_info(pCreateInfo);
   if (!local_create_info.pStages || !local_create_info.pGroups) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   stack_sizes = calloc(sizeof(*stack_sizes), local_create_info.groupCount);
   if (!stack_sizes) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   nir_shader *shader = create_rt_shader(device, &local_create_info, stack_sizes);
   VkComputePipelineCreateInfo compute_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .pNext = NULL,
      .flags = pCreateInfo->flags,
      .stage =
         {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = vk_shader_module_handle_from_nir(shader),
            .pName = "main",
         },
      .layout = pCreateInfo->layout,
   };
   result = radv_compute_pipeline_create(_device, _cache, &compute_info, pAllocator, pPipeline);
   if (result != VK_SUCCESS)
      goto shader_fail;

   pipeline = radv_pipeline_from_handle(*pPipeline);

   pipeline->compute.rt_group_handles =
      calloc(sizeof(*pipeline->compute.rt_group_handles), local_create_info.groupCount);
   if (!pipeline->compute.rt_group_handles) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto shader_fail;
   }

   pipeline->compute.rt_stack_sizes = stack_sizes;
   stack_sizes = NULL;

   for (unsigned i = 0; i < local_create_info.groupCount; ++i) {
      const VkRayTracingShaderGroupCreateInfoKHR *group_info = &local_create_info.pGroups[i];
      switch (group_info->type) {
      case VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR:
         if (group_info->generalShader != VK_SHADER_UNUSED_KHR)
            pipeline->compute.rt_group_handles[i].handles[0] = i + 2;
         break;
      case VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR:
         if (group_info->intersectionShader != VK_SHADER_UNUSED_KHR)
            pipeline->compute.rt_group_handles[i].handles[1] = i + 2;
         FALLTHROUGH;
      case VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR:
         if (group_info->closestHitShader != VK_SHADER_UNUSED_KHR)
            pipeline->compute.rt_group_handles[i].handles[0] = i + 2;
         if (group_info->anyHitShader != VK_SHADER_UNUSED_KHR)
            pipeline->compute.rt_group_handles[i].handles[1] = i + 2;
         break;
      case VK_SHADER_GROUP_SHADER_MAX_ENUM_KHR:
         unreachable("VK_SHADER_GROUP_SHADER_MAX_ENUM_KHR");
      }
   }

shader_fail:
   if (result != VK_SUCCESS && pipeline)
      radv_pipeline_destroy(device, pipeline, pAllocator);
   ralloc_free(shader);
fail:
   free((void *)local_create_info.pGroups);
   free((void *)local_create_info.pStages);
   free(stack_sizes);
   return result;
}

VkResult
radv_CreateRayTracingPipelinesKHR(VkDevice _device, VkDeferredOperationKHR deferredOperation,
                                  VkPipelineCache pipelineCache, uint32_t count,
                                  const VkRayTracingPipelineCreateInfoKHR *pCreateInfos,
                                  const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
   VkResult result = VK_SUCCESS;

   unsigned i = 0;
   for (; i < count; i++) {
      VkResult r;
      r = radv_rt_pipeline_create(_device, pipelineCache, &pCreateInfos[i], pAllocator,
                                  &pPipelines[i]);
      if (r != VK_SUCCESS) {
         result = r;
         pPipelines[i] = VK_NULL_HANDLE;

         if (pCreateInfos[i].flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT_EXT)
            break;
      }
   }

   for (; i < count; ++i)
      pPipelines[i] = VK_NULL_HANDLE;

   return result;
}